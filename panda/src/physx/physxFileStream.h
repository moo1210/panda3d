// Filename: physxFileStream.h
// Created by:  enn0x (11Oct09)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#ifndef PHYSXFILESTREAM_H
#define PHYSXFILESTREAM_H

#include "pandabase.h"

#include "NoMinMax.h"
#include "NxStream.h"

////////////////////////////////////////////////////////////////////
//       Class : PhysxFileStream
// Description : 
////////////////////////////////////////////////////////////////////
class EXPCL_PANDAPHYSX PhysxFileStream : public NxStream {

public:
  PhysxFileStream(const char *filename, bool load);
  virtual ~PhysxFileStream();

  virtual NxU8 readByte() const;
  virtual NxU16 readWord() const;
  virtual NxU32 readDword() const;
  virtual float readFloat() const;
  virtual double readDouble() const;
  virtual void readBuffer(void *buffer, NxU32 size) const;

  virtual NxStream &storeByte(NxU8 b);
  virtual NxStream &storeWord(NxU16 w);
  virtual NxStream &storeDword(NxU32 d);
  virtual NxStream &storeFloat(NxReal f);
  virtual NxStream &storeDouble(NxF64 f);
  virtual NxStream &storeBuffer(const void *buffer, NxU32 size);

private:
  FILE* fp;
};

#endif // PHYSXFILESTREAM_H
