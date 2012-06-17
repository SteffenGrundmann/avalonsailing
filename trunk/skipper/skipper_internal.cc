// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, July 2011

#include "skipper/skipper_internal.h"

#include "common/unknown.h"
#include "common/convert.h"
#include "common/polar_diagram.h"
#include "helmsman/normal_controller.h"
#include "lib/fm/log.h"
#include "skipper/planner.h"

static const double kDefaultDirection = 225;  // SouthWest as an approximation of the whole journey.

extern int debug;

void SkipperInternal::Run(const SkipperInput& in,
                          const vector<skipper::AisInfo>& ais,
                          double* alpha_star_deg) {

  if (in.angle_true_deg == kUnknown ||
      in.mag_true_kn == kUnknown ||
      isnan(in.angle_true_deg) ||
      isnan(in.mag_true_kn)) {
    *alpha_star_deg = kDefaultDirection;  // Southwest is our general direction.
    fprintf(stderr, "No true wind info so far, going SW.\n");
    return;
  }
  if (in.longitude_deg == kUnknown ||
      in.latitude_deg == kUnknown ||
      isnan(in.longitude_deg) ||
      isnan(in.latitude_deg)) {
    *alpha_star_deg = kDefaultDirection;  // Southwest is our general direction.
    fprintf(stderr, "No position info so far, going SW.\n");
    return;
  }
  double planned = Planner::ToDeg(in.latitude_deg, in.longitude_deg);


  // Remove comment when it works!
  // double safe = RunCollisionAvoider(planned, in, ais);
  double safe = planned;





  if (fabs(planned - safe) > 0.1)
      fprintf(stderr,
              "Override %8.6lg with %8.6lg because it is not collision free.\n",
              planned, safe);

  double feasible = BestSailableHeadingDeg(safe, in.angle_true_deg);
  if (fabs(safe - feasible) > 0.1)
      fprintf(stderr, "Override %8.6lg with %8.6lg because it is not sailable.\n",
              safe, feasible);
  if (debug) fprintf(stderr, "planner %lg , alpha* %lg,  true wind: %lg, feasible: %lg\n",
                     planned, safe, in.angle_true_deg, feasible);

  *alpha_star_deg = feasible;
}

void SkipperInternal::Init(const SkipperInput& in) {
  Planner::Init(LatLon(in.latitude_deg, in.longitude_deg));
}

bool SkipperInternal::TargetReached(const LatLon& lat_lon){
  return Planner::TargetReached(lat_lon);
}

double SkipperInternal::RunCollisionAvoider(
    double planned,
    const SkipperInput& in,
    const vector<skipper::AisInfo>& ais) {
  skipper::AvalonState avalon;
  avalon.timestamp_ms = ais.size() ? ais[0].timestamp_ms : 0;
  avalon.position = skipper::LatLon(in.longitude_deg, in.latitude_deg);
  avalon.target = skipper::Bearing::Degrees(planned);
  avalon.wind_from = skipper::Bearing::Degrees(in.angle_true_deg + 180);
  avalon.wind_speed_m_s = KnotsToMeterPerSecond(in.mag_true_kn);
  skipper::Bearing skipper_out = RunVSkipper(avalon, ais, 0);
  return skipper_out.deg();
}



