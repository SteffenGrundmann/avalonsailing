// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
//
// Main loop for helmsman: open client sockets to wind and imu, server
// socket for helsman clients (e.g. the skipper and sysmon) and shovel
// data between all open file descriptors and the main controller.
//

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "proto/rudder.h"
#include "proto/wind.h"
#include "proto/imu.h"
#include "proto/helmsman.h"
#include "proto/remote.h"
#include "proto/skipper_input.h"

#include "common/convert.h"
#include "common/unknown.h"
#include "sampling_period.h"
#include "ship_control.h"

extern int debug;

// -----------------------------------------------------------------------------
namespace {

// -----------------------------------------------------------------------------
//   Together with getopt in main, this is our minimalistic UI
// -----------------------------------------------------------------------------

//const char* version = "$Id: $";
const char* argv0;
int verbose = 0;

void crash(const char* fmt, ...) {
  va_list ap;
  char buf[1000];
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  if (debug)
    fprintf(stderr, "%s:%s%s%s\n", argv0, buf,
      (errno) ? ": " : "",
      (errno) ? strerror(errno):"" );
  else
    syslog(LOG_CRIT, "%s%s%s\n", buf,
           (errno) ? ": " : "",
           (errno) ? strerror(errno):"" );
  exit(1);
  va_end(ap);
  return;
}

static void fault(int i) { crash("fault"); }

void usage(void) {
  fprintf(stderr,
    "usage: [plug /path/to/bus] %s [options]\n"
    "options:\n"
    "\t-d debug (don't go daemon, don't syslog)\n"
    , argv0);
  exit(2);
}


static void set_fd(fd_set* s, int* maxfd, FILE* f) {
        int fd = fileno(f);
        FD_SET(fd, s);
        if (*maxfd < fd) *maxfd = fd;
}

uint64_t now_ms() {
  timeval tv;
  if (gettimeofday(&tv, NULL) < 0) crash("gettimeofday");
  uint64_t ms1 = tv.tv_sec;  ms1 *= 1000;
  uint64_t ms2 = tv.tv_usec; ms2 /= 1000;
  return ms1 + ms2;
}

uint64_t now_micros() {
  timeval tv;
  if (gettimeofday(&tv, NULL) < 0) crash("gettimeofday");
  return tv.tv_sec * 1000000LL + tv.tv_usec;
}

uint64_t nanos_to_wait(uint64_t last_controller_call_micros) {
  uint64_t now = now_micros();
  const uint64_t period = 100000LL;
  uint64_t timeout = last_controller_call_micros + period > now ?
     last_controller_call_micros + period - now :
     0;
  return 1000 * timeout;
}

void HandleRemoteControl(RemoteProto remote, int* control_mode) {
  if (remote.command != *control_mode)
    fprintf(stderr, "Helmsman switched to control mode %d\n", remote.command);
  switch (remote.command) {
    case kNormalControlMode:
      *control_mode = remote.command;
      ShipControl::Normal();
      break;
    case kDockingControlMode:
      *control_mode = remote.command;
       ShipControl::Docking();
      break;
    case kBrakeControlMode:
    case kPowerShutdownMode:
      *control_mode = remote.command;
      ShipControl::Brake();
      break;
    case kOverrideSkipperMode:
      *control_mode = remote.command;
      ShipControl::Normal();
      break;
  default:
    fprintf(stderr, "Illegal remote control");
  }
}

} // namespace

// -----------------------------------------------------------------------------
//   Main.
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {

  int ch;
  argv0 = strrchr(argv[0], '/');
  if (argv0) ++argv0; else argv0 = argv[0];

  while ((ch = getopt(argc, argv, "dhv")) != -1){
    switch (ch) {
    case 'd': ++debug; break;
    case 'v': ++verbose; break;
    case 'h':
    default:
      usage();
    }
  }

  argv += optind;
  argc -= optind;

  if (argc != 0) usage();

  setlinebuf(stdout);

  if (!debug) openlog(argv0, LOG_PERROR, LOG_LOCAL0);

  if (signal(SIGBUS, fault) == SIG_ERR)  crash("signal(SIGBUS)");
  if (signal(SIGSEGV, fault) == SIG_ERR)  crash("signal(SIGSEGV)");
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) crash("signal");

  if (debug) {
    fprintf(stderr, "Helmsman started\n");
  } else {
    daemon(0,0);
    char* path_to_pidfile = NULL;
    asprintf(&path_to_pidfile, "/var/run/%s.pid", argv0);
    FILE* pidfile = fopen(path_to_pidfile, "w");
    if(!pidfile) crash("writing pidfile (%s)", path_to_pidfile);
    fprintf(pidfile, "%d\n", getpid());
    fclose(pidfile);
    free(path_to_pidfile);

    syslog(LOG_INFO, "Helmsman started");
  }

  ControllerInput ctrl_in;

  int nn = 0;

  struct WindProto wind  = INIT_WINDPROTO;
  struct RudderProto sts = INIT_RUDDERPROTO;
  struct IMUProto imu    = INIT_IMUPROTO;
  struct HelmsmanCtlProto ctl = INIT_HELMSMANCTLPROTO;
  struct RemoteProto remote = INIT_REMOTEPROTO;
  ctrl_in.alpha_star_rad = Deg2Rad(225);  // Going SouthWest is a good guess (and breaks up a deadlock)
  int control_mode = kNormalControlMode;
  int loops = 0;
  uint64_t last_run_micros = 0;


  while (!feof(stdin)) {

    if (ferror(stdin)) clearerr(stdin);

    struct timespec timeout = { 0, nanos_to_wait(last_run_micros) }; // Run ship cotroller exactly once every 100ms
    fd_set rfds;
    int max_fd = -1;
    FD_ZERO(&rfds);
    set_fd(&rfds, &max_fd, stdin);

    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    int r = pselect(max_fd + 1, &rfds,  NULL, NULL, &timeout, &empty_mask);
    if (r == -1 && errno != EINTR) crash("pselect");

    if (debug>2) fprintf(stderr, "Woke up %d\n", r);

    if (FD_ISSET(fileno(stdin), &rfds)) {
      char line[1024];
      if (!fgets(line, sizeof line, stdin))
        crash("Error reading stdin");

      if (sscanf(line, IFMT_WINDPROTO(&wind, &nn)) > 0) {
        ctrl_in.wind.Reset();
        ctrl_in.wind.alpha_deg = SymmetricDeg(NormalizeDeg(wind.angle_deg));
        ctrl_in.wind.mag_kn = MeterPerSecondToKnots(wind.speed_m_s);

      } else if (sscanf(line, IFMT_IMUPROTO(&imu, &nn)) > 0) {
        ctrl_in.imu.Reset();
        ctrl_in.imu.temperature_c = imu.temp_c;

        ctrl_in.imu.acceleration.x_m_s2 = imu.acc_x_m_s2;
        ctrl_in.imu.acceleration.y_m_s2 = imu.acc_y_m_s2;
        ctrl_in.imu.acceleration.z_m_s2 = imu.acc_z_m_s2;

        ctrl_in.imu.gyro.omega_x_rad_s  = imu.gyr_x_rad_s;
        ctrl_in.imu.gyro.omega_y_rad_s  = imu.gyr_y_rad_s;
        ctrl_in.imu.gyro.omega_z_rad_s  = imu.gyr_z_rad_s;

        ctrl_in.imu.attitude.phi_x_rad = Deg2Rad(imu.roll_deg);
        ctrl_in.imu.attitude.phi_y_rad = Deg2Rad(imu.pitch_deg);
        ctrl_in.imu.attitude.phi_z_rad = SymmetricRad(NormalizeRad(Deg2Rad(imu.yaw_deg)));

        ctrl_in.imu.position.latitude_deg = imu.lat_deg;
        ctrl_in.imu.position.longitude_deg = imu.lng_deg;
        ctrl_in.imu.position.altitude_m = imu.alt_m;

        ctrl_in.imu.velocity.x_m_s = imu.vel_x_m_s;
        ctrl_in.imu.velocity.y_m_s = imu.vel_y_m_s;
        ctrl_in.imu.velocity.z_m_s = imu.vel_z_m_s;

        ctrl_in.imu.speed_m_s = imu.vel_x_m_s;

      } else if (sscanf(line, IFMT_RUDDERPROTO_STS(&sts, &nn)) > 0) {
        ctrl_in.drives.gamma_rudder_left_rad  = Deg2Rad(sts.rudder_l_deg);
        ctrl_in.drives.gamma_rudder_right_rad = Deg2Rad(sts.rudder_r_deg);
        ctrl_in.drives.gamma_sail_rad         = Deg2Rad(sts.sail_deg);
        ctrl_in.drives.homed_rudder_left = !isnan(sts.rudder_l_deg);
        ctrl_in.drives.homed_rudder_right = !isnan(sts.rudder_r_deg);
        ctrl_in.drives.homed_sail = !isnan(sts.sail_deg);

      } else if (sscanf(line, IFMT_HELMSMANCTLPROTO(&ctl, &nn)) > 0) {
          if (control_mode != kOverrideSkipperMode &&
              !isnan(ctl.alpha_star_deg))
            ctrl_in.alpha_star_rad = Deg2Rad(ctl.alpha_star_deg);
      } else if (sscanf(line, IFMT_REMOTEPROTO(&remote, &nn)) > 0) {
        HandleRemoteControl(remote, &control_mode);
        if (control_mode == kOverrideSkipperMode)
          ctrl_in.alpha_star_rad = Deg2Rad(remote.alpha_star_deg);
      }
    }

    if (nanos_to_wait(last_run_micros) > 100)
      continue;
    ControllerOutput ctrl_out;
    last_run_micros = now_micros();
    ShipControl::Run(ctrl_in, &ctrl_out);
    struct RudderProto ctl = {
      now_ms(),
      Rad2Deg(ctrl_out.drives_reference.gamma_rudder_star_left_rad),
      Rad2Deg(ctrl_out.drives_reference.gamma_rudder_star_right_rad),
      Rad2Deg(ctrl_out.drives_reference.gamma_sail_star_rad) };
    printf(OFMT_RUDDERPROTO_CTL(ctl, &nn));

    if (loops == 0) {
      struct SkipperInputProto to_skipper = {
        now_ms(),
        ctrl_out.skipper_input.longitude_deg,
        ctrl_out.skipper_input.latitude_deg,
        ctrl_out.skipper_input.angle_true_deg,
        ctrl_out.skipper_input.mag_true_kn
      };
      printf(OFMT_SKIPPER_INPUTPROTO(to_skipper, &nn));
    }
      /*
      if (loops == 5) {
        struct HelmsmanStatus = {
          Rad2Deg(ctrl_in.alpha_star_rad)
        };
        printf(OFMT_HELMSMAN_STATUSPROTO);
      */
    loops = loops == 9 ? 0 : loops+1;


  }  // for ever

  crash("Main loop exit");

  return 0;
}

