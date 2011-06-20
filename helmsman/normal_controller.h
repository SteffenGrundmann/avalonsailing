// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, May 2011
#ifndef HELMSMAN_NORMAL_CONTROLLER_H
#define HELMSMAN_NORMAL_CONTROLLER_H

#include "helmsman/controller.h"
#include "helmsman/rudder_controller.h"
#include "helmsman/reference_values.h"
#include "helmsman/sail_controller.h"

class NormalController : public Controller {
 public:
  NormalController(RudderController* rudder_controller,
                   SailController* sail_controller);
  virtual ~NormalController();
  virtual void Entry(const ControllerInput& in,
                     const FilteredMeasurements& filtered);
  virtual void Run(const ControllerInput& in,
                   const FilteredMeasurements& filtered,
                   ControllerOutput* out);
  virtual void Exit();
  virtual const char* Name() {
    return "Normal";
  }
  bool Tacking();
 private: 
  void ReferenceValueSwitch(double alpha_star,
                            double alpha_true, double mag_true,
                            double alpha_boat, double mag_boat,
                            double alpha_app,  double mag_app,
                            double* phi_z_star,
                            double* omega_z_star,
                            double* gamma_sail_star);
  RudderController* rudder_controller_;
  SailController* sail_controller_;
  double prev_alpha_star_limited_;
  ReferenceValues ref_;
};

#endif  // HELMSMAN_NORMAL_CONTROLLER_H
