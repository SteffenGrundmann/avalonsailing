// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
// Steffen Grundmann, July 2011

#include "skipper/skipper_internal.h"

#include <syslog.h>
#include <stdint.h>
#include  <string.h>

#include "common/unknown.h"
#include "common/convert.h"
#include "common/delta_angle.h"
#include "common/now.h"
#include "common/polar_diagram.h"
#include "helmsman/normal_controller.h"
#include "skipper/planner.h"
#include "vskipper/util.h"

extern int debug;

static const double kDefaultDirection = 225;  // SouthWest as an approximation of the whole journey.

double SkipperInternal::old_alpha_star_deg_ = kDefaultDirection;
WindStrengthRange SkipperInternal::wind_strength_ = kCalmWind;
bool SkipperInternal::storm_ = false;
bool SkipperInternal::storm_sign_plus_ = false;
bool SkipperInternal::full_plan_ = true;

void SkipperInternal::Run(const SkipperInput& in,
                          const vector<skipper::AisInfo>& ais,
                          double* alpha_star_deg,
                          TCStatus* tc_status) {
  if (in.angle_true_deg == kUnknown ||
      in.mag_true_kn == kUnknown ||
      isnan(in.angle_true_deg) ||
      isnan(in.mag_true_kn)) {
    *alpha_star_deg = kDefaultDirection;  // Southwest is our general direction.
    syslog(LOG_NOTICE, "No true wind info so far, going SW.\n");
    return;
  }

  // If there is a valid simple plan file then we go to the point specified there.
  // If the file exists, it shall contain just 2 numbers separated by a space character
  // like "44.66 6.3332" .
  // Otherwise we follow the plan we prepared.
  ReadSimplePlanFile("/tmp/simple.txt");

  wind_strength_ = WindStrength(wind_strength_, KnotsToMeterPerSecond(in.mag_true_kn));

  // This is a serial decision process
  // planned   ->   planned2   ->    safe       ->   feasible
  // from map       modified      modified to       is sailable
  //                if storm    avoid obstacles
  double planned = 0;
  double planned2 = 0;
  double safe = 0;
  if (in.longitude_deg == kUnknown ||
      in.latitude_deg == kUnknown ||
      isnan(in.longitude_deg) ||
      isnan(in.latitude_deg)) {
    if (!Planner::Initialized()) {
      *alpha_star_deg = kDefaultDirection;  // Southwest is our general direction.
      syslog(LOG_NOTICE, "No position info so far, going SW.\n");
      return;
    } else {
      // Temporary loss of GPS fix. This might happen due to big
      // waves washing over the GPS antenna, excessive boat heel or
      // other temporary effects. But we have had a GPS fix already and we know
      // were to go for the next days, so it is ok to go on.
      planned = old_alpha_star_deg_;
      // Without GPS fix there is no way to find a safe course, but it is likely
      // that our last safe course is still valid.
      safe = old_alpha_star_deg_;
      safe = HandleStorm(wind_strength_, in.angle_true_deg, safe);
    }
  } else {
    planned = Planner::ToDeg(in.latitude_deg, in.longitude_deg, tc_status);
    planned2 = HandleStorm(wind_strength_, in.angle_true_deg, planned);
    safe = RunCollisionAvoider(planned2, in, ais);
    old_alpha_star_deg_ = safe;
  }

  if (fabs(planned - planned2) > 0.1)
	syslog(LOG_NOTICE,
            	"Override %8.6lg with %8.6lg because we have a storm.\n",
            planned, planned2);
  if (fabs(planned2 - safe) > 0.1) {
	syslog(LOG_NOTICE,
              "Override %8.6lg with %8.6lg because it is not collision free.\n",
              planned2, safe);
  } else {
     syslog(LOG_NOTICE, "Bearing %8.6lg is collision free.\n", safe);
  }

  double feasible = BestSailableHeadingDeg(safe, in.angle_true_deg);

  if (fabs(DeltaOldNewDeg(safe, feasible)) > 0.1)
    syslog(LOG_NOTICE, "Override %8.6lg deg with %8.6lg deg because it is not sailable.\n",
            safe, feasible);

    syslog(LOG_DEBUG, "planner %6.1lf, alpha* %6.1lf,  true wind: %6.1lf, feasible: %6.1lf degrees\n",
            planned, safe, in.angle_true_deg, NormalizeDeg(feasible));

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
  avalon.position = skipper::LatLon::Degrees(in.latitude_deg, in.longitude_deg);
  avalon.target = skipper::Bearing::Degrees(planned);
  avalon.wind_from = skipper::Bearing::Degrees(in.angle_true_deg + 180);
  avalon.wind_speed_m_s = KnotsToMeterPerSecond(in.mag_true_kn);
  int64_t vskipper_start = now_micros();
  skipper::Bearing skipper_out = RunVSkipper(avalon, ais, 1);
    syslog(LOG_DEBUG, "Vskipper runtime %lld micros\n", now_micros() - vskipper_start);
  if (skipper::kVSkipperNoWay == skipper_out.deg()) {
    // In this situation we keep the bearing,
    // such that at least the approaching ship has a chance to avoid us.
    return old_alpha_star_deg_;
  }
  return skipper_out.deg();
}

// In a storm the planner is overridden.
// The helmsman will steer a broad reach course relative to the wind.
// [http://www.gosailing.info/Sailing%20in%20Heavy%20Weather.htm]
// [Discussion at Zurich Sailing Club, 2012/08/17]
// No maneuvers possible. But a small risk of stalling.
// If we assume that the wave fronts are orthogonal to the wind
// direction then sailing 50 degrees off the wind vector direction
// means we hit the waves not directly but at an angle.

// If there is a storm this function writes the
// best bearing for this situation to storm_bearing;
// storm_ and storm_sign_plus_ are modified.
double SkipperInternal::HandleStorm(WindStrengthRange wind_strength,
                                    double angle_true_deg,
                                    double planned) {
  bool transition_to_storm = false;
  double storm_bearing = 0;
  if (storm_) {
    if (wind_strength != kStormWind) {
      storm_ = false;
    }
  } else {
    if (wind_strength == kStormWind) {
      storm_ = true;
      transition_to_storm = true;
    }
  }

  // The direction side is kept as long as the storm lasts.
  // TODO Adapt every 6h (25miles)
  // We keep a constant angle to the true wind.
  const double kStormAngle = 50.0;
  if (transition_to_storm) {
    double d1 = DeltaOldNewDeg(planned, NormalizeDeg(angle_true_deg + kStormAngle));
    double d2 = DeltaOldNewDeg(planned, NormalizeDeg(angle_true_deg - kStormAngle));
    storm_sign_plus_ = fabs(d1) < fabs(d2);
  }

  if (storm_) {
    if (storm_sign_plus_) {
      storm_bearing = NormalizeDeg(angle_true_deg + kStormAngle);
    } else {
      storm_bearing = NormalizeDeg(angle_true_deg - kStormAngle);
    }
    if (transition_to_storm) {
      syslog(LOG_NOTICE, "Storm, going to %6.2lf deg.\n", storm_bearing);
    }
  } else {
    storm_bearing = planned;
  }
  return storm_bearing;
}

namespace {

int sscan_ais(const char *line, int64_t now_ms, skipper::AisInfo* s) {
  s->timestamp_ms = now_ms;

  if (!strncmp(line, "ais: ", 5))
    line += 5;

  while (*line) {
    char key[16];
    double value = NAN;

    int skip = 0;
    // This is a bug as the timestamp is a 64 bit number but the double mantissa is 53 bits only.
    int n = sscanf(line, "%16[a-z_]:%lf %n", key, &value, &skip);
    if (n < 2) return 0;
    if (skip == 0) return 0;
    line += skip;

    if (!strcmp(key, "timestamp_ms")  && !isnan(value)) {
      s->timestamp_ms = int64_t(value);
      continue;
    }
    if (!strcmp(key, "lat_deg")       && !isnan(value)) {
      s->position = skipper::LatLon::Degrees(value, s->position.lon_deg());
      continue;
    }
    if (!strcmp(key, "lng_deg")       && !isnan(value)) {
      s->position = skipper::LatLon::Degrees(s->position.lat_deg(), value);
      continue;
    }
    if (!strcmp(key, "speed_m_s")     && !isnan(value)) {
      s->speed_m_s = value;
      continue;
    }
    if (!strcmp(key, "cog_deg")       && !isnan(value)) {
      s->bearing = skipper::Bearing::Degrees(value);
      continue;
    }
    // ignore anything else
    // return 0;
  }
  return 1;
}

} // namespace


void SkipperInternal::ReadAisFile(
    const char* ais_filename, std::vector<skipper::AisInfo>* ais) {
  FILE* fp = fopen(ais_filename, "r");
  if (!fp) {
    // This failure is possible in the beginning, when the ais_buf
    // hasn't written anything yet.
    syslog(LOG_NOTICE, "Could not open %s", ais_filename);

    return;
  }
  while (!feof(fp)) {
    char line[1024];
    if (!fgets(line, sizeof(line), fp))
      break;
    skipper::AisInfo ai;
    if (!sscan_ais(line, now_ms(), &ai))
      continue;
    ais->push_back(ai);

    syslog(LOG_DEBUG, "ais: %lld %lf %lf %lf %lf\n",
                       ai.timestamp_ms, ai.position.lat_deg(), ai.position.lon_deg(),
                       ai.speed_m_s, ai.bearing.deg());
  }
  fclose(fp);
}

void SkipperInternal::ReadSimplePlanFile(const char* simple_target_filename) {
  FILE* fp = fopen(simple_target_filename, "r");
  if (!fp) {
    // Normal case with long plan.
    if (!full_plan_) {
      // But now this file is gone so we return to the full plan.
      full_plan_ = true;
      Planner::Reset();
    }
    return;
  }
  // We follow a simple plan defined by the simple plan file
  // which has just this content: e.g. "46.4389 -9.40275" .
  while (!feof(fp)) {
    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
      full_plan_ = true;
      break;
    }
    double lat = 0;
    double lon = 0;
    if (2 == sscanf(line, "%lf %lf", &lat, &lon)) {
      syslog(LOG_NOTICE, "Follow simple plan to [%lf %lf]\n", lat, lon);
      if (lat > 70) lat = 70;
      if (lat < -70) lat = -70;
      Planner::SimplePlan(lat, lon);
      full_plan_ = false;
      break;
    }
    // Reading failed
    Planner::Reset();
    full_plan_ = true;
  }
  fclose(fp);
}
