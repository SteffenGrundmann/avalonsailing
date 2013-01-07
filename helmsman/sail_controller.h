// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, May 2011
#ifndef HELMSMAN_SAIL_CONTROLLER_H
#define HELMSMAN_SAIL_CONTROLLER_H

#include "common/polar.h"

enum SailMode {
  WING,        // for sailing at the wind
  SPINNAKER    // optimal for broad reach
};

class SailModeLogic {
 public:
  SailModeLogic();
  // apparent is supposed to be the absolute value, but can go below zero (hysteresis effect).
  // Side effect: sets mode_
  SailMode BestMode(double alpha_apparent_wind_rad, double wind_strength_m_s);
  SailMode BestStabilizedMode(double alpha_apparent_wind_rad, double wind_strength_m_s);
  void UnlockMode();
  void Reset();
  const char* ModeString();
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
  // Return the absolute magnitude of the sail angle to push backwards.
  // Always positive.
  double BestGammaSailForReverseMotion(double alpha_wind_rad,
                                       double mag_wind);

  // The same as BestGammaSail but with hysteresis and stabilization to avoid
  // frequent switching.
  double BestStabilizedGammaSail(double alpha_wind_rad,
                                 double mag_wind);
  // set Tack, i.e. which side the wind is coming from.
  void SetAlphaSign(int sign);
  // phi_z_offset gives feedback from the sail controller
  // to the normal controller, if the apparent wind turns into
  // the forbidden zone around the eye of the wind. This may
  // happen because the true wind direction is filtered slowly.
  double StableGammaSail(const Polar& true_wind,      // double alpha_true, double mag_true,
                         const Polar& apparent_wind,  // double alpha_app, double mag_app,
                         double phi_z,
                         double* phi_z_offset);

  void SetOptimalAngleOfAttack(double optimal_angle_of_attack_rad);
  double GetOptimalAngleOfAttack();
  void UnlockMode();
  void Reset();
  double FilterOffset(double offset);

 private:
  // Optimal angle of attack, reduced at high wind strength.
  double AngleOfAttack(double mag_wind);

  double GammaSailInternal(double alpha_wind_rad,
                           double mag_wind,
                           bool stabilized);
  void HandleSign(double* alpha_wind_rad, int* sign);

  double optimal_angle_of_attack_rad_;
  SailModeLogic logic_;
  int sign_;  // The sign has no inertia
  // But we might get into a
  // situation if the apparent wind is around zero and we would have
  // to turn the sail back and forth between -90 and +90.

  int alpha_sign_;  // positive when sailing on starboard tack.
};



#endif  // HELMSMAN_SAIL_CONTROLLER_H
