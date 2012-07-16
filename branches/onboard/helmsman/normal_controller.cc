// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, June 2011

#include "normal_controller.h"

#include <math.h>
#include <stdint.h>
//#include <sys/time.h>
#include <time.h>
#include "lib/delta_angle.h"
#include "lib/normalize.h"
#include "lib/now.h"
#include "lib/limit_rate.h"
#include "lib/polar_diagram.h"
#include "lib/unknown.h"
#include "apparent.h"
#include "new_gamma_sail.h"
#include "sampling_period.h"

extern int debug;
static const double kTackOvershootRad = 15 / 180.0 * M_PI;

NormalController::NormalController(RudderController* rudder_controller,
                                   SailController* sail_controller)
   : rudder_controller_(rudder_controller),
     sail_controller_(sail_controller),
     alpha_star_rate_limit_(Deg2Rad(4)),  // 4 deg/s
     old_phi_z_star_(0),
     give_up_counter_(0),
     start_time_ms_(now_ms()),
     trap2_(999) {
  if (debug) fprintf(stderr, "NormalController::Entry time  %lld s\n",
                     start_time_ms_ / 1000);
  if (debug) fprintf(stderr, "NormalController::rate limit  %lf rad/s, %lf deg/s\n",
                     alpha_star_rate_limit_, Rad2Deg(alpha_star_rate_limit_));
  CHECK_EQ(999, trap2_);
}

NormalController::~NormalController() {}

// Entry needs to set all variable states.
void NormalController::Entry(const ControllerInput& in,
                             const FilteredMeasurements& filtered) {
  // Make sure we get the right idea of the direction change when we come
  // from another state.
  old_phi_z_star_ = SymmetricRad(filtered.phi_z_boat);
  sail_controller_->BestGammaSail(filtered.angle_app, filtered.mag_app);
  ref_.SetReferenceValues(old_phi_z_star_, in.drives.gamma_sail_rad);
  give_up_counter_ = 0;
  start_time_ms_ = now_ms();
  if (debug) {
    fprintf(stderr, "NormalController::Entry old_phi_z_star_: %6.1lf deg\n",  Rad2Deg(old_phi_z_star_));
    fprintf(stderr, "Entry Time: %10.1lf s\n", (double)start_time_ms_ / 1000);
  }
  rudder_controller_->Reset();
}

void NormalController::Run(const ControllerInput& in,
                           const FilteredMeasurements& filtered,
                           ControllerOutput* out) {
  if (debug) {
    fprintf(stderr, "------------NormalController::Run----------\n");
    fprintf(stderr, "Actuals: True %6.1lf deg %6.1lf m/s ", Rad2Deg(filtered.alpha_true), filtered.mag_true);
    fprintf(stderr, "Boat %6.1lf deg %6.1lf m/s ", Rad2Deg(filtered.phi_z_boat), filtered.mag_boat);
    fprintf(stderr, "App. %6.1lf deg %6.1lf m/s\n", Rad2Deg(filtered.angle_app), filtered.mag_app);
  }
  CHECK_EQ(999, trap2_);  // Triggers at incomplete compilation errors.
  double phi_star;
  double omega_star;
  double gamma_sail_star;

  ShapeReferenceValue(SymmetricRad(in.alpha_star_rad),
		      SymmetricRad(filtered.alpha_true), filtered.mag_true,
		      SymmetricRad(filtered.phi_z_boat), filtered.mag_boat,
		      SymmetricRad(filtered.angle_app),  filtered.mag_app,
		      in.drives.gamma_sail_rad,
		      &phi_star,
		      &omega_star,
		      &gamma_sail_star);
  if (debug) fprintf(stderr, "IntRef: %6.2lf %6.2lf\n", Rad2Deg(phi_star), Rad2Deg(omega_star));

  double gamma_rudder_star;

  // The boat speed is an unreliable measurement value. It has big systematic and stochastic errors and is
  // therefore filtered and clipped in the filter block. The NormalController can work only
  // with postive boat speeds, but at the transition from the InitialController the very slowly
  // boat speed may be still negative. We simply clip the speed here. If the speed gets too low then
  // we'll leave the NormalController anyway (see GiveUp() ).
  double positive_speed = std::max(0.25, filtered.mag_boat);
  rudder_controller_->Control(phi_star,
                              omega_star,
                              SymmetricRad(filtered.phi_z_boat),
                              filtered.omega_boat,
                              positive_speed,
                              &gamma_rudder_star);
  if (isnan(gamma_rudder_star)) {
    fprintf(stderr, "gamma_R* isnan: phi_star %6.2lfdeg, omega_star %6.2lf, phi_z_boat %6.2lf, omega_boat %6.2lf, mag_boat %6.2lf\n",
            Rad2Deg(phi_star), omega_star, SymmetricRad(filtered.phi_z_boat), filtered.omega_boat, filtered.mag_boat);
  }

  out->drives_reference.gamma_rudder_star_left_rad  = gamma_rudder_star;
  out->drives_reference.gamma_rudder_star_right_rad = gamma_rudder_star;
  out->drives_reference.gamma_sail_star_rad = gamma_sail_star;
  if (debug) fprintf(stderr, "Controls: %6.2lf %6.2lf\n\n", Rad2Deg(gamma_rudder_star), Rad2Deg(gamma_sail_star));
}

void NormalController::Exit() {
  if (debug) fprintf(stderr, " NormalController::Exit\n");
}

bool NormalController::IsJump(double old_direction, double new_direction) {
  // All small direction changes are handled directly by the rudder control,
  // with rate limitation for the reference value. Bigger changes and maneuvers
  // require planning because of the synchronized motion of boat and sail.
  // For them IsJump returns true.
  const double JibeZoneWidth = M_PI - JibeZoneRad();
  CHECK(JibeZoneWidth < TackZoneRad());
  return fabs(DeltaOldNewRad(old_direction, new_direction)) >
              1.8 * JibeZoneWidth;
}

// a in [b-eps, b+eps] interval
bool NormalController::Near(double a, double b) {
  const double tolerance = 2 * kSamplingPeriod * alpha_star_rate_limit_;
  return b - tolerance <= a && a <= b + tolerance;
}

// The current bearing is near the TackZone (close reach) or near the Jibe Zone (broad reach)
// and we will have to do a maneuver.
bool NormalController::IsGoodForManeuver(double old_direction, double new_direction, double angle_true) {
  //const double JibeZoneWidth = M_PI - JibeZoneRad();
  double old_relative = SymmetricRad(old_direction - angle_true);
  double new_relative = SymmetricRad(new_direction - angle_true);
  // Critical angles to the wind vector. Because the PolarDiagram follows the
  // "wind blows from" convention, we have to turn the tack zone and jibe zone angles.
  double tack_zone = M_PI-TackZoneRad();
  double jibe_zone = M_PI-JibeZoneRad();
  if (new_relative < 0)
    return Near(old_relative, jibe_zone) || Near(old_relative, tack_zone);
  else
    return Near(old_relative, -jibe_zone) || Near(old_relative, -tack_zone);
}

// Every tack or jibe uses the same angle, i.e. we have standardized jibes and tacks.
double LimitToMinimalManeuver(double old, double new_bearing, double alpha_true, double maneuver_type) {
  if (maneuver_type == kChange)
    return new_bearing;
  double old_to_wind = DeltaOldNewRad(old, alpha_true);
  if (maneuver_type == kTack) {
    double tack_angle = 2 * TackZoneRad() + kTackOvershootRad;
    if (old_to_wind < 0)
      return SymmetricRad(old + tack_angle);
    else
      return SymmetricRad(old - tack_angle);
  }
  if (maneuver_type == kJibe) {
    double jibe_angle = 2 * (M_PI - JibeZoneRad());
    if (old_to_wind < 0)
      return SymmetricRad(old - jibe_angle);
    else
      return SymmetricRad(old + jibe_angle);
  }
  CHECK(0);  // Define behaviour for new maneuver type!
  return new_bearing;
}

// Shape the reference value alpha* into something
// sailable, feasible (not too fast changing) and calculate the
// suitable sail angle.
// States: old_phi_z_star_ because phi_z_star must not jump
//   and ref_ because a running plan trumps everything else.
ManeuverType NormalController::ShapeReferenceValue(double alpha_star,
                                                   double alpha_true, double mag_true,
                                                   double phi_z_boat, double mag_boat,
                                                   double angle_app,  double mag_app,
                                                   double old_gamma_sail,
                                                   double* phi_z_star,
                                                   double* omega_z_star,
                                                   double* gamma_sail_star) {
  ManeuverType maneuver_type = kChange;
  if (debug) fprintf(stderr, "app %6.2lf true %6.2lf old_sail %6.2lf\n", angle_app, alpha_true, old_gamma_sail);

  // 3 cases:
  // If a maneuver is running (tack or jibe or change) we finish that maneuver first
  // else if the new alpha* differs from the old alpha* by more than 20 degrees then
  //     we start a new plan
  // else we restrict alpha* to the sailable range and rate limit it to [-4deg/s, 4deg/s]
  if (ref_.RunningPlan()) {
    // The plan includes a stabilization period of a second at the end
    // with constant reference values.
    ref_.GetReferenceValues(phi_z_star, omega_z_star, gamma_sail_star);
    if (debug) fprintf(stderr, "* %6.2lf %6.2lf %6.2lf\n", alpha_star, alpha_star, *phi_z_star);
  } else {
    // Stay in sailable zone
    double new_sailable = BestSailableHeading(alpha_star, alpha_true);
    if (debug) fprintf(stderr, "new sailable: %6.2lf\n", new_sailable);
    if (IsGoodForManeuver(old_phi_z_star_, new_sailable, alpha_true) &&
        IsJump(old_phi_z_star_, new_sailable)) {
      // We need a new plan ...
      maneuver_type = FindManeuverType(old_phi_z_star_,
                                       new_sailable,
                                       alpha_true);
      // Limit new_sailable to the other side of the maneuver zone.
      new_sailable = LimitToMinimalManeuver(old_phi_z_star_, new_sailable, alpha_true, maneuver_type);
      if (debug) fprintf(stderr, "Start %s maneuver, from  %lf to %lf degrees\n",
                         ManeuverToString(maneuver_type), Rad2Deg(old_phi_z_star_), Rad2Deg(new_sailable));
      double new_gamma_sail;
      double delta_gamma_sail;
      NewGammaSail(old_gamma_sail,
                   maneuver_type,
                   kTackOvershootRad,
                   &new_gamma_sail,
                   &delta_gamma_sail);

      ref_.SetReferenceValues(old_phi_z_star_, old_gamma_sail);
      ref_.NewPlan(new_sailable, delta_gamma_sail, mag_boat);
      ref_.GetReferenceValues(phi_z_star, omega_z_star, gamma_sail_star);
    } else {
      // Normal case, minor changes of the desired heading.
      // Rate limit alpha_star.
      double limited = old_phi_z_star_;
      LimitRateWrapRad(new_sailable, alpha_star_rate_limit_ * kSamplingPeriod, &limited);
      *phi_z_star = limited;
      // Feed forward of the rotation speed helps, but if the boats speed
      // is estimated too low, we get a big overshoot due to exaggerated gamma_0
      // values.
      *omega_z_star = 0;
      // The apparent wind data are filtered to suppress noise in
      // the sail drive reference value.
      *gamma_sail_star =
            sail_controller_->BestStabilizedGammaSail(angle_app, mag_app);
      // When sailing close hauled (hard to the wind) we observed sail angle oscillations
      // leading to the sail swinging over the boat symmetry axis. This will be
      // suppressed.
      const double kCloseHauledLimit = Deg2Rad(4);
      bool close_hauled_positive = DeltaOldNewRad(alpha_true, phi_z_boat) > Deg2Rad(105);
      bool close_hauled_negative = DeltaOldNewRad(alpha_true, phi_z_boat) < Deg2Rad(-105);
      if (close_hauled_positive) {
        if (debug) fprintf(stderr, "CHP %lf ", *gamma_sail_star);
        *gamma_sail_star = std::max(*gamma_sail_star, kCloseHauledLimit);
        if (debug) fprintf(stderr, "to %lf\n", *gamma_sail_star);
      }
      if (close_hauled_negative) {
        if (debug) fprintf(stderr, "CHN %lf ", *gamma_sail_star);
        *gamma_sail_star = std::min(*gamma_sail_star, -kCloseHauledLimit);
        if (debug) fprintf(stderr, "to %lf\n", *gamma_sail_star);
      }
      // TODO sail drive lazyness

      //if (debug) fprintf(stderr, "S %6.2lf %6.2lf %6.2lf\n", angle_app, mag_app, *gamma_sail_star);
    }
    if (debug) fprintf(stderr, "* %6.2lf %6.2lf %6.2lf\n", alpha_star, new_sailable, *phi_z_star);
  }

  old_phi_z_star_ = *phi_z_star;
  return maneuver_type;
}

bool NormalController::TackingOrJibing() {
  return ref_.RunningPlan();
}

bool NormalController::GiveUp(const ControllerInput& in,
                              const FilteredMeasurements& filtered) {
  // Got stuck for more than 2 minutes.
  // If the drift caused by stream is too big, we will not detect loss of control here
  // because our speed over ground is e.g. 0.5 knots.
  // Abort every 2 hours? Abort if epsilon is too big?
  // TODO: Contemplate this!
  if (filtered.mag_boat < 0.03)
    ++give_up_counter_;
  else
    give_up_counter_ = 0;
  return give_up_counter_ > 120.0 / kSamplingPeriod;  // The speed is filtered with 60s and rather imprecise.
}

double NormalController::NowSeconds() {
  return (now_ms() - start_time_ms_) / 1000.0;
}

double NormalController::RateLimit() {
  return alpha_star_rate_limit_;
}
