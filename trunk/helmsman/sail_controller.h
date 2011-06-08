// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, May 2011
#ifndef HELMSMAN_SAIL_CONTROLLER_H
#define HELMSMAN_SAIL_CONTROLLER_H

#include "helmsman/sail_controller.h"

enum SailMode {
  WING,       // for sailing at the wind
  SPINNAKER   // optimal for broad reach
};

class SailModeLogic {
 public:
  SailModeLogic();
  SailMode BestMode(double alpha_apparent_wind_rad) const;
  SailMode BestStabilizedMode(double alpha_apparent_wind_rad);
 private:
  SailMode mode_;  // need state for hysteresis
  int delay_counter_;
};

class SailController {
 public:
  SailController();

  // Optimal gamma wind for a given apparent wind direction and strength.
  // alpha wind: direction of the apparent wind vector relative to the boats x-axis
  // (in radians, pi/2 is wind from port side, 0 rad is wind
  // from behind) in [0, 2*pi) and
  // the wind speed (magnitude of wind vector) in m/s. mag_wind >= 0
  double BestGammaSail(double alpha_wind_rad,
                       double mag_wind);

  // The same as BestGammaSail but with hysteresis and stabilization to avoid
  // frequent switching.
  double BestStabilizedGammaSail(double alpha_wind_rad,
                                 double mag_wind);

  void SetOptimalAngleOfAttack(double optimal_angle_of_attack_rad);

 private:
  double GammaSailInternal(double alpha_wind_rad,
                           double mag_wind,
                           bool stabilized);

  double optimal_angle_of_attack_rad_;
  SailModeLogic logic_;
};

#endif  // HELMSMAN_SAIL_CONTROLLER_H
