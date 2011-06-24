// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, June 2011


#include "helmsman/boat_model.h"
#include "helmsman/ship_control.h"

#include <math.h>
//#include "common/unknown.h"
#include "common/convert.h"
#include "common/polar.h"


#include "lib/testing/testing.h"


namespace {
const double kSamplingPeriod = 0.1;
}

TEST(SimShip, All) {
  // omega_ phi_z_ = 0, v_x_ = 0., gamma_sail_ = 0.2
  BoatModel model(kSamplingPeriod, 0, 0, 0, -M_PI / 2);
  
  DriveReferenceValuesRad ref;
  Polar true_wind(0, 10);  // forward wind, 10m/s
  ControllerInput in;
  ControllerOutput out;
  in.alpha_star = Deg2Rad(90);         // want to go east
  
  model.PrintHeader(); 
  for (double t = 0; t < 100; t += kSamplingPeriod) {
    model.Simulate(out.drives_reference, 
                   true_wind,
                   &in);
    ShipControl::Run(in, &out);
    model.Print(t); 
  }                 
}

int main(int argc, char* argv[]) {
  SimShip_All();
  return 0;
}

