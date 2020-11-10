// See LICENSE_CELLO file for license and copyright information

/// @file     control_new_refresh.cpp
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     2019-05-23
/// @brief    Charm-related functions associated with refreshing ghost zones
/// @ingroup  Control

#include "simulation.hpp"
#include "mesh.hpp"
#include "control.hpp"

#include "charm_simulation.hpp"
#include "charm_mesh.hpp"

// #define DEBUG_NEW_REFRESH
// #define DEBUG_NEW_REFRESH_SYNC
#define DEBUG_ENZO_PROLONG

#ifdef DEBUG_NEW_REFRESH
#   define TRACE_NEW_REFRESH(BLOCK,REFRESH,MSG)                         \
      CkPrintf ("DEBUG_NEW_REFRESH %s %d: %s: sync %d  type %d \n",     \
                BLOCK->name().c_str(),CkMyPe(),MSG,REFRESH->sync_id(),  \
                REFRESH->neighbor_type());                              \
      fflush(stdout);
#   define TRACE_SYNC(MSG)                                              \
      CkPrintf ("DEBUG_NEW_REFRESH_SYNC %s %d sync %s %s:%d refresh %d %d %d\n", \
                name().c_str(),CkMyPe(),MSG,__FILE__,__LINE__,          \
                id_refresh,sync->value(),sync->stop());                 \
      fflush(stdout);
#else
#   define TRACE_NEW_REFRESH(BLOCK,REFRESH,MSG)  /* ... */
#   define TRACE_SYNC(MSG) /* ... */
#endif
                                                \
    
#define CHECK_ID(ID) ASSERT1 ("CHECK_ID","Invalid id %d",ID,(ID>=0));

//----------------------------------------------------------------------

void Block::new_refresh_start (int id_refresh, int callback)
{
  CHECK_ID(id_refresh);
  Refresh * refresh = cello::refresh(id_refresh);
  Sync * sync = sync_(id_refresh);
#ifdef DEBUG_NEW_REFRESH
  refresh->print();
#endif  

  // Send field and/or particle data associated with the given refresh
  // object to corresponding neighbors

  if ( refresh->is_active() ) {

    ASSERT1 ("Block::new_refresh_start()",
	     "refresh[%d] state is not inactive",
	     id_refresh,
	     (sync->state() == RefreshState::INACTIVE));

    sync->set_state(RefreshState::ACTIVE);

    // send Field face data

    int count_field=0;
    if (refresh->any_fields()) {
      count_field = new_refresh_load_field_faces_ (*refresh);
    }

    // send Particle face data
    int count_particle=0;
    if (refresh->any_particles()){
      count_particle = new_refresh_load_particle_faces_(*refresh);
    }

    // send Flux face data
    int count_flux=0;
    if (refresh->any_fluxes()){
      count_flux = new_refresh_load_flux_faces_(*refresh);
    }

    const int count = count_field + count_particle + count_flux;
    
    // Make sure sync counter is not active
    ASSERT4 ("Block::new_refresh_start()",
	     "refresh[%d] sync object %p is active (%d/%d)",
	     id_refresh, sync, sync->value(), sync->stop(),
	     (sync->value() == 0 && sync->stop() == 0));

    // Initialize sync counter
    sync->set_stop(count);

    TRACE_SYNC("A start");

    new_refresh_wait(id_refresh,callback);

  } else {

    new_refresh_exit(*refresh);

  }
}

//----------------------------------------------------------------------
void Block::new_refresh_wait (int id_refresh, int callback)
{
  CHECK_ID(id_refresh);

  Refresh * refresh = cello::refresh(id_refresh);
  Sync * sync = sync_(id_refresh);

  ASSERT1("Block::new_refresh_wait()",
          "Wait called with inactive Refresh[%d]",
          id_refresh,
          (refresh->is_active()));

  // make sure the callback parameter matches that in the refresh object

  ASSERT3("Block::new_refresh_wait()",
          "Refresh[%d] mismatch between refresh callback %d and parameter callback %d",
          id_refresh,callback, refresh->callback(),
          (callback == refresh->callback()) );

  // make sure we aren't already in a "ready" state

  ASSERT1("Block::new_refresh_wait()",
          "Refresh[%d] not in 'active' state",
          id_refresh,
          (sync->state() == RefreshState::ACTIVE) );

  // tell refresh we're ready to start processing messages

  sync->set_state(RefreshState::READY);

  // process any existing messages in the refresh message list

  TRACE_SYNC("B wait");

  for (auto id_msg=0;
       id_msg<new_refresh_msg_list_[id_refresh].size();
       id_msg++) {

    MsgRefresh * msg = new_refresh_msg_list_[id_refresh][id_msg];

    // unpack message data into Block data
    msg->update(data());
      
    delete msg;
    sync->advance();
  }
    
  // clear the message queue

  new_refresh_msg_list_[id_refresh].resize(0);

  // and check if we're finished

  new_refresh_check_done(id_refresh);
}

//----------------------------------------------------------------------

void Block::new_refresh_check_done (int id_refresh)
{
  CHECK_ID(id_refresh);

  Refresh * refresh = cello::refresh(id_refresh);
  Sync * sync = sync_(id_refresh);

  TRACE_SYNC("C check");
  
  ASSERT1("Block::new_refresh_check_done()",
	  "Refresh[%d] must not be in inactive state",
	  id_refresh,
	  (sync->state() != RefreshState::INACTIVE) );

  if ( (sync->stop()==0) ||
      (sync->is_done() && (sync->state() == RefreshState::READY))) {

    // Make sure incoming message queue is empty

    ASSERT2("Block::new_refresh_wait()",
	   "Refresh %d message list has size %lu instead of 0",
	    id_refresh,new_refresh_msg_list_[id_refresh].size(),
	    (new_refresh_msg_list_[id_refresh].size() == 0));

    // reset sync counter
    sync->reset();
    sync->set_stop(0);
    
    // reset refresh state to inactive

    sync->set_state(RefreshState::INACTIVE);

    // Call callback

    new_refresh_exit(*refresh);
  }
}

//----------------------------------------------------------------------

void Block::p_new_refresh_recv (MsgRefresh * msg)
{

  const int id_refresh = msg->id_refresh();
  CHECK_ID(id_refresh);
  TRACE_NEW_REFRESH(this,cello::refresh(id_refresh),"recv");

  Sync * sync = sync_(id_refresh);

  TRACE_SYNC("D recv");
  
  if (sync->state() == RefreshState::READY) {
    // unpack message data into Block data if ready
    msg->update(data());
      
    delete msg;

    sync->advance();
    TRACE_SYNC("E advance");

    // check if it's the last message processed
    new_refresh_check_done(id_refresh);
  
  } else {

    // save message if not ready
    new_refresh_msg_list_[id_refresh].push_back(msg);

  }

}

//----------------------------------------------------------------------

void Block::new_refresh_exit (Refresh & refresh)
{
  CHECK_ID(refresh.id());
  update_boundary_();
  control_sync (refresh.callback(),
  		refresh.sync_type(),
  		refresh.sync_exit(),
  		refresh.min_face_rank(),
  		refresh.neighbor_type(),
  		refresh.root_level());
  // CkCallback
  //   (refresh.callback(),
  //    CkArrayIndexIndex(index_),thisProxy).send(NULL);
}

//----------------------------------------------------------------------

int Block::new_refresh_load_field_faces_ (Refresh & refresh)
{
  int count = 0;

  const int min_face_rank = refresh.min_face_rank();
  const int neighbor_type = refresh.neighbor_type();

  if (neighbor_type == neighbor_leaf ||
      neighbor_type == neighbor_tree) {

    // Loop over neighbor leaf Blocks (not necessarily same level)

    const int min_level = cello::config()->mesh_min_level;
    
    ItNeighbor it_neighbor =
      this->it_neighbor(min_face_rank,index_,
			neighbor_type,min_level,refresh.root_level());

    int if3[3];
    while (it_neighbor.next(if3)) {

      Index index_neighbor = it_neighbor.index();

      int ic3[3];

      const int level = this->level();
      const int level_face = it_neighbor.face_level();

      if (level_face > level) {
        index_neighbor.child(level_face,ic3,ic3+1,ic3+2);
      } else if (level > level_face) {
        index_.child(level,ic3,ic3+1,ic3+2);
      }
      const int refresh_type = 
	(level_face == level - 1) ? refresh_coarse :
	(level_face == level)     ? refresh_same :
	(level_face == level + 1) ? refresh_fine : refresh_unknown;

      // handle padded interpolation special case if needed

      count += refresh_extra_field_faces_
        (refresh,index_neighbor, level,level_face,if3,ic3);

      new_refresh_load_field_face_
        (refresh,refresh_type,index_neighbor,if3,ic3);
      
      ++count;
    }

  } else if (neighbor_type == neighbor_level) {

    // Loop over neighbor Blocks in same level (not necessarily leaves)

    ItFace it_face = this->it_face(min_face_rank,index_);

    int if3[3];
    while (it_face.next(if3)) {

      // count all faces if not a leaf, else don't count if face level
      // is less than this block's level
      
      if ( ! is_leaf() || face_level(if3) >= level()) {
	
	Index index_face = it_face.index();
	int ic3[3] = {0,0,0};
	new_refresh_load_field_face_ (refresh,refresh_same,index_face,if3,ic3);
	++count;
      }

    }
  }

  return count;
}

//----------------------------------------------------------------------

void Block::new_refresh_load_field_face_
( Refresh & refresh,
  int refresh_type,
  Index index_neighbor,
  int if3[3],
  int ic3[3])

{
  // ... coarse neighbor requires child index of self in parent
  if (refresh_type == refresh_coarse) {
    index_.child(index_.level(),ic3,ic3+1,ic3+2);
  }

  // ... copy field ghosts to array using FieldFace object

  MsgRefresh * msg_refresh = new MsgRefresh;

  bool lg3[3] = {false,false,false};

  FieldFace * field_face = create_face
    (if3, ic3, lg3, refresh_type, &refresh,false);

  DataMsg * data_msg = new DataMsg;
  data_msg -> set_field_face (field_face,true);
  data_msg -> set_field_data (data()->field_data(),false);

  const int id_refresh = refresh.id();
  CHECK_ID(id_refresh);
  
  ASSERT1 ("Block::new_refresh_load_field_face_()",
	  "id_refresh %d of refresh object is out of range",
	   id_refresh,
	   (0 <= id_refresh));
  msg_refresh->set_new_refresh_id (id_refresh);
  msg_refresh->set_data_msg (data_msg);

  TRACE_NEW_REFRESH(this,cello::refresh(id_refresh),"send");
  thisProxy[index_neighbor].p_new_refresh_recv (msg_refresh);

}

//----------------------------------------------------------------------

int Block::refresh_extra_field_faces_
(Refresh refresh,
 Index index_neighbor,
 int level, int level_face,
 int if3[3],
 int ic3[3])
{
  int count = 0;

  const int padding = cello::problem()->prolong()->padding();

  if ((padding > 0) && (level != level_face)) {

    // Create box_face
    
    const int rank = cello::rank();
    int n3[3];
    data()->field().size(n3,n3+1,n3+2);
    const int g = refresh.ghost_depth();
    int g3[3] = {g3[0] = (rank >= 1) ? g : 0,
                 g3[1] = (rank >= 2) ? g : 0,
                 g3[2] = (rank >= 3) ? g : 0};

    // Boxes used Bs -> br, Be -> br, Bs -> be
    Box box_face (rank,n3,g3);
    Box box_Bsbe (rank,n3,g3);
    Box box_Bebr (rank,n3,g3);
    
    // Create iterator over extra blocks
    
    ItNeighbor it_extra =
      this->it_neighbor(refresh.min_face_rank(),index_,
                        refresh.neighbor_type(),
                        cello::config()->mesh_min_level,
                        refresh.root_level());
    
      // ... determine intersection region

    const bool l_send = (level < level_face);
    const bool l_recv = (level > level_face);

    int jf3[3] = { l_send ? if3[0] : -if3[0],
                   l_send ? if3[1] : -if3[1],
                   l_send ? if3[2] : -if3[2] };
    
    box_face.set_block(+1,jf3,ic3);
    box_face.set_padding(padding);

    box_face.compute_region();

    if (l_send) {

      // SENDER LOOP OVER EXTRA BLOCKS
      
      int ef3[3];
      while (it_extra.next(ef3)) {

        const Index index_extra = it_extra.index();
        const int   level_extra = it_extra.face_level();
        
        int ec3[3] = {0,0,0};
        if (level_extra > level) {
          index_extra.child(level_extra,ec3,ec3+1,ec3+2);
        }
        
        const bool l_face_match =
          (if3[0]==ef3[0]) && (if3[1]==ef3[1]) && (if3[2]==ef3[2]);
        const bool l_child_match =
          (ic3[0]==ec3[0]) && (ic3[1]==ec3[1]) && (ic3[2]==ec3[2]);

        const bool l_match = l_face_match && l_child_match;

        // ... skip extra block if it's the same as the face block
        if (! l_match) {

          // ... determine overlap of extra block with intersection region
          const int level_send = level;
          box_face.set_block ((level_extra-level_send), ef3,ec3);
          box_face.compute_block_start();
        
          int im3[3],ip3[3];
          bool overlap = box_face.get_limits (im3,ip3,Box::BlockType::extra);
          
          if (overlap) {

            if (level_extra == level) {
          
              // this block sends; extra block is coarse
          
              // handle contribution of this block Bs to Be -> br

              int tf3[3] =
                { if3[0]-ef3[0], if3[1]-ef3[1], if3[2]-ef3[2] };

              // Box Bs | Be -> br
              box_Bebr.set_block(+1,tf3,ic3);
              box_Bebr.set_padding(padding);
              box_Bebr.compute_region();

              tf3[0] = -ef3[0];
              tf3[1] = -ef3[1];
              tf3[2] = -ef3[2];
              box_Bebr.set_block(0,tf3,ic3); // ic3 ignored
              box_Bebr.compute_block_start();

              bool overlap = box_Bebr.get_limits
                (im3,ip3,Box::BlockType::extra);

              ASSERT3 ("Block::refresh_extra_field_faces_",
                       "Face tf3 %d %d %d out of bounds",
                       tf3[0],tf3[1],tf3[2],
                       (-1 <= tf3[0] && tf3[0] <= 1) &&
                       (-1 <= tf3[1] && tf3[1] <= 1) &&
                       (-1 <= tf3[2] && tf3[2] <= 1));
              ASSERT6 ("Block::refresh_extra_field_faces_",
                       "Face tf3 %d %d %d does not overlap region for face %d %d %d",
                       tf3[0],tf3[1],tf3[2],ef3[0],ef3[1],ef3[2],
                       overlap);
            } // level_extra == level
          } // overlap
        } // ! match
      } // while (it_extra.next())
      
    } else if (l_recv) {

      // RECEIVER LOOP OVER EXTRA BLOCKS

      int ef3[3];
      while (it_extra.next(ef3)) {
        
        const Index index_extra = it_extra.index();
        const int   level_extra = it_extra.face_level();
        
        int ec3[3] = {0,0,0};
        if (level_extra > level_face) {
          index_extra.child(level_extra,ec3,ec3+1,ec3+2);
        }

        const bool l_face_match =
          (if3[0]==ef3[0]) && (if3[1]==ef3[1]) && (if3[2]==ef3[2]);
        const bool l_child_match =
          (ic3[0]==ec3[0]) && (ic3[1]==ec3[1]) && (ic3[2]==ec3[2]);

        const bool l_match = l_face_match && l_child_match;

        // ... skip extra block if it's the same as the face block

        if (!l_match) {

          // *** count expected receive from Be or be ***

          // ... determine overlap of extra block with intersection region
          const int level_send = level_face;
          box_face.set_block ((level_extra-level_send), ef3,ec3);
          box_face.compute_block_start();
        
          int im3[3],ip3[3];
          bool overlap = box_face.get_limits
            (im3,ip3,Box::BlockType::extra);

          if (overlap) {

#ifndef DEBUG_ENZO_PROLONG
            ++count;
#endif        
        
            if (level_extra == level) {

              // this block receives; extra block is fine

              // handle contribution of this block br to Bs -> be

              int tf3[3] =
                { ef3[0]-if3[0], ef3[1]-if3[1], ef3[2]-if3[2] };
            
              // Box br | Bs -> be
              box_Bsbe.set_block(+1,tf3,ec3);
              box_Bsbe.set_padding(padding);
              box_Bsbe.compute_region();

              tf3[0] = -if3[0];
              tf3[1] = -if3[1];
              tf3[2] = -if3[2];

              box_Bsbe.set_block(0,tf3,ic3);
              box_Bsbe.compute_block_start();

              bool overlap = box_Bsbe.get_limits
                (im3,ip3,Box::BlockType::extra);
              
              ASSERT3 ("Block::refresh_extra_field_faces_",
                       "Face tf3 %d %d %d out of bounds",
                       tf3[0],tf3[1],tf3[2],
                       ((-1 <= tf3[0] && tf3[0] <= 1) &&
                        (-1 <= tf3[1] && tf3[1] <= 1) &&
                        (-1 <= tf3[2] && tf3[2] <= 1)));

              ASSERT6 ("Block::refresh_extra_field_faces_",
                       "Face %d %d %d does not overlap region for face %d %d %d",
                       tf3[0],tf3[1],tf3[2],ef3[0],ef3[1],ef3[2],
                       overlap);

            } // level_extra == level
          } // if (overlap)
        } // if (! match)
      } // while (it_extra.next())
    } // (level > level_face)
  } // (level != level_face)
  return count;
}

//----------------------------------------------------------------------

int Block::new_refresh_load_particle_faces_ (Refresh & refresh)
{
  const int rank = cello::rank();

  const int npa3[3] = { 4, 4*4, 4*4*4 };
  const int npa = npa3[rank-1];

  ParticleData ** particle_array = new ParticleData*[npa];
  ParticleData ** particle_list = new ParticleData*[npa];
  std::fill_n (particle_array,npa,nullptr);
  std::fill_n (particle_list,npa,nullptr);

  Index * index_list = new Index[npa];
  
  // Sort particles that have left the Block into 4x4x4 array
  // corresponding to neighbors

  int nl = particle_load_faces_
    (npa,particle_list,particle_array, index_list, &refresh);

  // Send particle data to neighbors

  new_particle_send_(refresh,nl,index_list,particle_list);

  delete [] particle_array;
  delete [] particle_list;
  delete [] index_list;

  return nl;
}

//----------------------------------------------------------------------

void Block::new_particle_send_
(Refresh & refresh, int nl,Index index_list[], ParticleData * particle_list[])
{

  ParticleDescr * p_descr = cello::particle_descr();

  for (int il=0; il<nl; il++) {

    Index index           = index_list[il];
    ParticleData * p_data = particle_list[il];
    Particle particle_send (p_descr,p_data);
    
    const int id_refresh = refresh.id();
    CHECK_ID(id_refresh);

    ASSERT1 ("Block::new_particle_send_()",
	     "id_refresh %d of refresh object is out of range",
	     id_refresh,
	     (0 <= id_refresh));

  if (p_data && p_data->num_particles(p_descr)>0) {

      DataMsg * data_msg = new DataMsg;
#ifdef DEBUG_NEW_REFRESH
      CkPrintf ("%d %s:%d DEBUG_REFRESH %p new DataMsg\n",
                CkMyPe(),__FILE__,__LINE__,data_msg);
#endif      
      data_msg ->set_particle_data(p_data,true);

      MsgRefresh * msg_refresh = new MsgRefresh;
      msg_refresh->set_data_msg (data_msg);
      msg_refresh->set_new_refresh_id (id_refresh);

      thisProxy[index].p_new_refresh_recv (msg_refresh);

    } else if (p_data) {
      
      MsgRefresh * msg_refresh = new MsgRefresh;

      msg_refresh->set_data_msg (nullptr);
#ifdef DEBUG_NEW_REFRESH
      CkPrintf ("%d %s:%d DEBUG_REFRESH set data_msg=NULL\n",
                CkMyPe(),__FILE__,__LINE__);
#endif      
      msg_refresh->set_new_refresh_id (id_refresh);

      thisProxy[index].p_new_refresh_recv (msg_refresh);

      // assert ParticleData object exits but has no particles
      delete p_data;

    }

  }
}

//----------------------------------------------------------------------

int Block::particle_load_faces_ (int npa, 
				 ParticleData * particle_list[],
				 ParticleData * particle_array[],
				 Index index_list[],
				 Refresh *refresh)
{
  // Array elements correspond to child-sized blocks to
  // the left, inside, and right of the main Block.  Particles
  // are assumed to be (well) within this area.
  //
  //     +---+---+---+---+
  //     | 03| 13| 23| 33|
  //     +---+===+===+---+
  //     | 02||  :  || 32|
  //     +---+ - + - +---+
  //     | 01||  :  || 31|
  //     +---+=======+---+
  //     | 00| 10| 20| 30|
  //     +---+---+---+---+
  //
  // Actual neighbors may overlap multiple child-sized blocks.  In
  // that case, we have one ParticleData object per neighbor, but
  // with pointer duplicated.   So if neighbor configuration is:
  //
  //     +---+   5   +---+
  //     | 4 |       | 6 |
  // +---+---+===+===+---+
  // |       ||     ||    
  // |   2   +       +   3
  // |       ||     ||    
  // +-------+=======+-------+
  //         |            
  //     0   |            
  //                 1   
  //
  // Then the particle data array will be:
  //
  //     +---+---+---+---+
  //     | 4 | 5 | 5 | 6 |
  //     +---+===+===+---+
  //     | 2 ||  :  || 3 |
  //     +---+ - + - +---+
  //     | 2 ||  :  || 3 |
  //     +---+=======+---+
  //     | 0 | 1 | 1 | 1 |
  //     +---+---+---+---+

  //  TRACE_REFRESH("particle_load_faces()");

  // ... arrays for updating positions of particles that cross
  // periodic boundaries

  int nl = particle_create_array_neighbors_
    (refresh, particle_array,particle_list,index_list);

  // Scatter particles among particle_data array

  Particle particle (cello::particle_descr(),
		     data()->particle_data());

  std::vector<int> type_list;
  if (refresh->all_particles()) {
    const int nt = particle.num_types();
    type_list.resize(nt);
    for (int i=0; i<nt; i++) type_list[i] = i;
  } else {
    type_list = refresh->particle_list();
  }

  particle_scatter_neighbors_(npa,particle_array,type_list, particle);

  // Update positions particles crossing periodic boundaries

  particle_apply_periodic_update_  (nl,particle_list,refresh);

  return nl;
}

//----------------------------------------------------------------------

int Block::particle_create_array_neighbors_
(Refresh * refresh, 
 ParticleData * particle_array[],
 ParticleData * particle_list[],
 Index index_list[])
{ 
  //  TRACE_REFRESH("particle_create_array_neighbors()");

  const int rank = cello::rank();
  const int level = this->level();

  const int min_face_rank = refresh->min_face_rank();

  ItNeighbor it_neighbor =
    this->it_neighbor(min_face_rank,index_, neighbor_leaf,0,0);

  int il = 0;

  int if3[3];
  for (il=0; it_neighbor.next(if3); il++) {

    const int level_face = it_neighbor.face_level();

    int ic3[3] = {0,0,0};

    const int refresh_type = 
      (level_face == level - 1) ? refresh_coarse :
      (level_face == level)     ? refresh_same :
      (level_face == level + 1) ? refresh_fine : refresh_unknown;

    if (refresh_type==refresh_coarse) {
      // coarse neighbor: need index of self in parent
      index_.child(index_.level(),ic3,ic3+1,ic3+2);
    } else if (refresh_type==refresh_fine) {
      // fine neighbor: need index of child in self
      it_neighbor.child(ic3);
    }
    // (else same-level neighbor: don't need child)

    int index_lower[3] = {0,0,0};
    int index_upper[3] = {1,1,1};
    refresh->get_particle_bin_limits
      (rank,refresh_type,if3,ic3,index_lower,index_upper);

    ParticleData * pd = new ParticleData;

    ParticleDescr * p_descr = cello::particle_descr();

    pd->allocate(p_descr);

    particle_list[il] = pd;

    index_list[il] = it_neighbor.index();

    for (int iz=index_lower[2]; iz<index_upper[2]; iz++) {
      for (int iy=index_lower[1]; iy<index_upper[1]; iy++) {
	for (int ix=index_lower[0]; ix<index_upper[0]; ix++) {
	  int i=ix + 4*(iy + 4*iz);
	  particle_array[i] = pd;
	}
      }
    }
  }
  
  return il;
}

//----------------------------------------------------------------------

void Block::particle_determine_periodic_update_
(int * index_lower, int * index_upper,
 double *dpx, double *dpy, double *dpz)
{
  //     ... domain extents
  double dxm,dym,dzm;
  double dxp,dyp,dzp;

  cello::hierarchy()->lower(&dxm,&dym,&dzm);
  cello::hierarchy()->upper(&dxp,&dyp,&dzp);

  //     ... periodicity
  bool p3[3];
  periodicity(p3);

  //     ... boundary
  bool b32[3][2];
  is_on_boundary (b32);

  const int rank = cello::rank();

  // Update (dpx,dpy,dpz) position correction if periodic domain
  // boundary is crossed

  if (rank >= 1) {
    if (index_lower[0]==0 && b32[0][0] && p3[0]) (*dpx) = +(dxp - dxm);
    if (index_upper[0]==4 && b32[0][1] && p3[0]) (*dpx) = -(dxp - dxm);
  }
  if (rank >= 2) {
    if (index_lower[1]==0 && b32[1][0] && p3[1]) (*dpy) = +(dyp - dym);
    if (index_upper[1]==4 && b32[1][1] && p3[1]) (*dpy) = -(dyp - dym);
  }
  if (rank >= 3) {
    if (index_lower[2]==0 && b32[2][0] && p3[2]) (*dpz) = +(dzp - dzm);
    if (index_upper[2]==4 && b32[2][1] && p3[2]) (*dpz) = -(dzp - dzm);
  }
}

//----------------------------------------------------------------------

void Block::particle_apply_periodic_update_
(int nl, ParticleData * particle_list[], Refresh * refresh)
{

  const int rank = cello::rank();
  const int level = this->level();
  const int min_face_rank = refresh->min_face_rank();

  std::vector<double> dpx(nl,0.0);
  std::vector<double> dpy(nl,0.0);
  std::vector<double> dpz(nl,0.0);

  // Compute position updates for particles crossing periodic boundaries

  ItNeighbor it_neighbor =
    this->it_neighbor(min_face_rank,index_, neighbor_leaf,0,0);

  int il=0;

  int if3[3];
  while (it_neighbor.next(if3)) {

    const int level_face = it_neighbor.face_level();

    int ic3[3];
    it_neighbor.child(ic3);

    const int refresh_type = 
      (level_face == level - 1) ? refresh_coarse :
      (level_face == level)     ? refresh_same :
      (level_face == level + 1) ? refresh_fine : refresh_unknown;

    int index_lower[3] = {0,0,0};
    int index_upper[3] = {1,1,1};
    refresh->get_particle_bin_limits
      (rank,refresh_type,if3,ic3,index_lower,index_upper);

    // ASSERT: il < nl
    particle_determine_periodic_update_
      (index_lower,index_upper,&dpx[il],&dpy[il],&dpz[il]);

    il++;

  }

  ParticleDescr * p_descr = cello::particle_descr();

  // Apply the updates to the list of particles

  for (int il=0; il<nl; il++) {

    ParticleData * p_data = particle_list[il];
    Particle particle_neighbor (p_descr,p_data);

    if ( ((rank >= 1) && dpx[il] != 0.0) ||
	 ((rank >= 2) && dpy[il] != 0.0) ||
	 ((rank >= 3) && dpz[il] != 0.0) ) {
	
      // ... for each particle type
      const int nt = particle_neighbor.num_types();
      for (int it=0; it<nt; it++) {

	// ... for each batch of particles
	const int nb = particle_neighbor.num_batches(it);
	for (int ib=0; ib<nb; ib++) {

	  particle_neighbor.position_update (it,ib,dpx[il],dpy[il],dpz[il]);

	}
      }
    }
  }
}
//----------------------------------------------------------------------

void Block::particle_scatter_neighbors_
(int npa,
 ParticleData * particle_array[],
 std::vector<int> & type_list,
 Particle particle)
{
  const int rank = cello::rank();

  //     ... get Block bounds 
  double xm,ym,zm;
  double xp,yp,zp;
  lower(&xm,&ym,&zm);
  upper(&xp,&yp,&zp);

  // find block center (x0,y0,z0) and width (xl,yl,zl)
  const double x0 = 0.5*(xm+xp);
  const double y0 = 0.5*(ym+yp);
  const double z0 = 0.5*(zm+zp);
  const double xl = xp-xm;
  const double yl = yp-ym;
  const double zl = zp-zm;

  int count = 0;
  // ...for each particle type to be moved

  for (auto it_type=type_list.begin(); it_type!=type_list.end(); it_type++) {

    int it = *it_type;

    const int ia_x  = particle.attribute_position(it,0);

    // (...positions may use absolute coordinates (float) or
    // block-local coordinates (int))
    const bool is_float = 
      (cello::type_is_float(particle.attribute_type(it,ia_x)));

    // (...stride may be != 1 if particle attributes are interleaved)
    const int d  = particle.stride(it,ia_x);

    // ...for each batch of particles

    const int nb = particle.num_batches(it);

    for (int ib=0; ib<nb; ib++) {

      const int np = particle.num_particles(it,ib);

      // ...extract particle position arrays

      std::vector<double> xa(np,0.0);
      std::vector<double> ya(np,0.0);
      std::vector<double> za(np,0.0);

      particle.position(it,ib,xa.data(),ya.data(),za.data());

      // ...initialize mask used for scatter and delete
      // ...and corresponding particle indices

      bool * mask = new bool[np];
      int * index = new int[np];
      
      for (int ip=0; ip<np; ip++) {

	double x = is_float ? 2.0*(xa[ip*d]-x0)/xl : xa[ip*d];
	double y = is_float ? 2.0*(ya[ip*d]-y0)/yl : ya[ip*d];
	double z = is_float ? 2.0*(za[ip*d]-z0)/zl : za[ip*d];

	int ix = (rank >= 1) ? (x + 2) : 0;
	int iy = (rank >= 2) ? (y + 2) : 0;
	int iz = (rank >= 3) ? (z + 2) : 0;

	if (! (0 <= ix && ix < 4) ||
	    ! (0 <= iy && iy < 4) ||
	    ! (0 <= iz && iz < 4)) {
	  
	  CkPrintf ("%d ix iy iz %d %d %d\n",CkMyPe(),ix,iy,iz);
	  CkPrintf ("%d x y z %f %f %f\n",CkMyPe(),x,y,z);
	  CkPrintf ("%d xa ya za %f %f %f\n",CkMyPe(),xa[ip*d],ya[ip*d],za[ip*d]);
	  CkPrintf ("%d xm ym zm %f %f %f\n",CkMyPe(),xm,ym,zm);
	  CkPrintf ("%d xp yp zp %f %f %f\n",CkMyPe(),xp,yp,zp);
	  ERROR3 ("Block::particle_scatter_neighbors_",
		  "particle indices (ix,iy,iz) = (%d,%d,%d) out of bounds",
		  ix,iy,iz);
	}

	const int i = ix + 4*(iy + 4*iz);
	index[ip] = i;
	bool in_block = true;
	in_block = in_block && (!(rank >= 1) || (1 <= ix && ix <= 2));
	in_block = in_block && (!(rank >= 2) || (1 <= iy && iy <= 2));
	in_block = in_block && (!(rank >= 3) || (1 <= iz && iz <= 2));
	mask[ip] = ! in_block;
      }

      // ...scatter particles to particle array
      particle.scatter (it,ib, np, mask, index, npa, particle_array);
      // ... delete scattered particles
      count += particle.delete_particles (it,ib,mask);

      delete [] mask;
      delete [] index;
    }
  }

  cello::simulation()->data_delete_particles(count);

}

//----------------------------------------------------------------------

int Block::new_refresh_load_flux_faces_ (Refresh & refresh)
{
  int count = 0;

  const int min_face_rank = cello::rank() - 1;
  const int neighbor_type = neighbor_leaf;
  
  // Loop over neighbor leaf Blocks (not necessarily same level)

  const int min_level = cello::config()->mesh_min_level;
    
  ItNeighbor it_neighbor =
    this->it_neighbor(min_face_rank,index_,
                      neighbor_type,min_level,refresh.root_level());

  int if3[3];
  while (it_neighbor.next(if3)) {

    Index index_neighbor = it_neighbor.index();

    int ic3[3];
    it_neighbor.child(ic3);

    const int level = this->level();
    const int level_face = it_neighbor.face_level();

    const int refresh_type = 
      (level_face < level) ? refresh_coarse :
      (level_face > level) ? refresh_fine : refresh_same;

    new_refresh_load_flux_face_
      (refresh,refresh_type,index_neighbor,if3,ic3);

    ++count;

  }

  return count;
}

//----------------------------------------------------------------------

void Block::new_refresh_load_flux_face_
( Refresh & refresh,
  int refresh_type,
  Index index_neighbor,
  int if3[3],
  int ic3[3])

{
  // ... coarse neighbor requires child index of self in parent
  TRACE_NEW_REFRESH(this,(&refresh),"load_flux_face");
  if (refresh_type == refresh_coarse) {
    index_.child(index_.level(),ic3,ic3+1,ic3+2);
  }

  // ... copy field ghosts to array using FieldFace object

  const int axis = (if3[0]!=0) ? 0 : (if3[1]!=0) ? 1 : 2;
  const int face = (if3[axis]==-1) ? 0 : 1;


  // neighbor is coarser
  DataMsg * data_msg = new DataMsg;
  FluxData * flux_data = data()->flux_data();
  
  const bool is_new = true;
  if (refresh_type == refresh_coarse) {
    // neighbor is coarser
    const int nf = flux_data->num_fields();
    data_msg -> set_num_face_fluxes(nf);
    for (int i=0; i<nf; i++) {
      FaceFluxes * face_fluxes = new FaceFluxes
        (*flux_data->block_fluxes(axis,face,i));
      face_fluxes->coarsen(ic3[0],ic3[1],ic3[2],cello::rank());
      data_msg -> set_face_fluxes (i,face_fluxes, is_new);
    }
  } else {
    data_msg -> set_num_face_fluxes(0);
  }

  const int id_refresh = refresh.id();
  CHECK_ID(id_refresh);
  
  ASSERT1 ("Block::new_refresh_load_flux_face_()",
           "id_refresh %d of refresh object is out of range",
           id_refresh,
           (0 <= id_refresh));

  MsgRefresh * msg_refresh = new MsgRefresh;

  msg_refresh->set_data_msg (data_msg);
  msg_refresh->set_new_refresh_id (id_refresh);

  TRACE_NEW_REFRESH(this,cello::refresh(id_refresh),"send");
  thisProxy[index_neighbor].p_new_refresh_recv (msg_refresh);

}



