//=============================================================================
//
//  Copyright (c) Kitware, Inc.
//  All rights reserved.
//  See LICENSE.txt for details.
//
//  This software is distributed WITHOUT ANY WARRANTY; without even
//  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
//  PURPOSE.  See the above copyright notice for more information.
//
//  Copyright 2012 Sandia Corporation.
//  Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
//  the U.S. Government retains certain rights in this software.
//
//=============================================================================
#define VTKM_DEVICE_ADAPTER VTKM_DEVICE_ADAPTER_CUDA
#define BOOST_SP_DISABLE_THREADS

#include "ArgumentsParser.h"
#include "compare.h"

int main(int argc, char* argv[])
  {
  vtkm::testing::ArgumentsParser parser;
  if (!parser.parseArguments(argc, argv))
    {
    return 1;
    }

  const std::string file = parser.file();
  const std::string writeLoc = parser.writeLocation();

  const float isoValue = parser.isovalue();
  const double ratio = parser.ratio();

  RunComparison("Cuda", file, writeLoc, 1, 1, isoValue, ratio);
  return 0;
}
