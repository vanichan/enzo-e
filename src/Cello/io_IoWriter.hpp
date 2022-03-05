// See LICENSE_CELLO file for license and copyright information

/// @file     io_IoWriter.hpp
/// @author   James Bordner (jobordner@ucsd.edu)
/// @date     2022-02-01
/// @brief    [\ref Io] Declaration of the IoWriter class

#ifndef IO_IO_WRITER_HPP
#define IO_IO_WRITER_HPP

class IoWriter : public CBase_IoWriter {

  /// @class    IoWriter
  /// @ingroup  Io
  /// @brief    [\ref Io] 

public: // interface

  /// Constructor
  IoWriter() throw()
  { }

  /// CHARM++ migration constructor for CBase_IoWriter
  IoWriter (CkMigrateMessage *m) :
    CBase_IoWriter(m)
  { }

  /// CHARM++ Pack / Unpack function
  void pup (PUP::er &p) 
  {
    TRACEPUP;
    CBase_IoWriter::pup(p);
  }

public: // entry methods

  void p_write()
  { CkPrintf ("DEBUG_IO IoWriter::p_write()\n"); }

private: // functions


private: // attributes

  // NOTE: change pup() function whenever attributes change

};

#endif /* IO_IO_WRITER_HPP */

