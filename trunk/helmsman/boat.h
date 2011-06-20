// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
#ifndef HELMSMAN_BOAT_H_
#define HELMSMAN_BOAT_H_
// Created by simulation/generate_boat_h.m on 
// 09:43 (CEST) Monday 20 June 2011
namespace {
const double kInertiaZ = 150;  // kg m^2
const double kAreaR = 0.085;     // m^2
const double kNumberR = 2;   // number of rudders
const double kLeverR = 1.43048;    // m, COG to rudder axis lever
const double kRhoWater = 1030;  // kg/m^3
const double kOmegaMaxSail = 0.241661;  // rad/s
}  // namespace
#endif  // HELMSMAN_BOAT_H_
