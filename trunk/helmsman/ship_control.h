// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, May 2011
#ifndef HELMSMAN_STATE_CONTROL_H
#define HELMSMAN_STATE_CONTROL_H


#include "helmsman/controller_io.h"
#include "helmsman/filter_block.h"
#include "helmsman/wind_strength.h"

// Controllers for certain States
#include "helmsman/controller.h"         // base class
#include "helmsman/brake_controller.h"
#include "helmsman/docking_controller.h"
#include "helmsman/initial_controller.h"
#include "helmsman/normal_controller.h"

// Rudder and sail
#include "helmsman/rudder_controller.h"
#include "helmsman/sail_controller.h"


enum MetaState {
  kBraking = 0,
  kDocking,
  kNormal
};

class ShipControl {
 public:
  static void Run(const ControllerInput& in, ControllerOutput* out);
  static bool Brake();
  static bool Docking();
  static bool Normal();
  static void Reset();  // for tests only

 private:
  static void StateMachine(const ControllerInput& in);
  static void Transition(Controller* new_state,
                         const ControllerInput& in);

  static double alpha_star_;
  static WindStrengthRange wind_strength_;
  static WindStrengthRange wind_strength_apparent_;
    
  static FilterBlock* filter_block_;
  static FilteredMeasurements filtered_;
  static MetaState meta_state_;
  static Controller* controller_;

  static RudderController rudder_controller_;
  static SailController sail_controller_;
  
  static InitialController initial_controller_;
  static BrakeController brake_controller_;
  static DockingController docking_controller_;
  static NormalController normal_controller_;
};

#endif  // HELMSMAN_SHIP_CONTROL_H
