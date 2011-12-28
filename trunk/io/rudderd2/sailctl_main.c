// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
//
// Issue epos commands to control the sail
// 

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../../proto/rudder.h"

#include "actuator.h"
#include "eposclient.h"

// -----------------------------------------------------------------------------
//   Together with getopt in main, this is our minimalistic UI
// -----------------------------------------------------------------------------
// static const char* version = "$Id: $";
static const char* argv0;
static int verbose = 0;
static int debug = 0;


static void crash(const char* fmt, ...) {
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

static void fault() { crash("fault"); }

static void usage(void) {
	fprintf(stderr,	"usage: plug /path/to/ebus | %s\n", argv0);
	exit(2);
}

static int64_t now_ms() {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) < 0) crash("no working clock");

        int64_t ms1 = tv.tv_sec;  ms1 *= 1000;
        int64_t ms2 = tv.tv_usec; ms2 /= 1000;
        return ms1 + ms2;
}

static Bus* bus = NULL;
static Device* motor;
static Device* bmmh;
static double target_angle_deg = NAN;

static int processinput() {
	char line[1024];
	if (!fgets(line, sizeof(line), stdin))
		crash("reading stdin");

	if (line[0] == '#') {
		struct RudderProto msg = INIT_RUDDERPROTO;
		int nn;
		int n = sscanf(line+1, IFMT_RUDDERPROTO_CTL(&msg, &nn));
		if (n != 4)
			return 0;
		target_angle_deg = msg.sail_deg;
	} else {
		if (!bus_receive(bus, line))
			return 0;
		bus_clocktick(bus);
	}
	return 1;
}

enum { DEFUNCT = 0, HOMING, TARGETTING, REACHED };
const char* status[] = { "DEFUNCT", "HOMING", "TARGETTING", "REACHED" };

const uint32_t MAX_CURRENT_MA     = 3000;
const double MAX_ABS_SPEED_DEG_S  = 30;
const double MAX_ABS_ACCEL_DEG_S2 = 500;

// todo, use -d flag and fprintf
#define VLOGF(...) do {} while (0)

int sail_init() {

        int r;
        uint32_t status;
        uint32_t error;

        VLOGF("sail_init");

        if (!device_get_register(motor, REG_STATUS, &status))
                return DEFUNCT;

        if (status & STATUS_FAULT) {
                VLOGF("sail_control: clearing fault");
                device_invalidate_register(motor, REG_CONTROL);   // force re-issue
                device_set_register(motor, REG_CONTROL, CONTROL_CLEARFAULT);
		device_invalidate_register(motor, REG_ERROR);   // force re-issue
		device_get_register(motor, REG_ERROR, &error);  // we won't read it but eposmon will pick up the response.
                device_invalidate_register(motor, REG_STATUS);
                return DEFUNCT;
        }

        r = device_set_register(motor, REG_OPMODE, OPMODE_PPM);

        r &= device_set_register(motor, REGISTER(0x2080, 0), 3000); // current_threshold       User specific [500 mA]
        r &= device_set_register(motor, REGISTER(0x6065, 0), 0xffffffff); // max_following_error User specific [2000 qc]
        r &= device_set_register(motor, REGISTER(0x6067, 0), 2000);   // position window [qc], see 14.66
        r &= device_set_register(motor, REGISTER(0x6068, 0), 50);      // position time window [ms], see 14.66
        r &= device_set_register(motor, REGISTER(0x607D, 1), 0x80000000); // min_position_limit [-2147483648 qc]
        r &= device_set_register(motor, REGISTER(0x607D, 2), 0x7fffffff); // max_position_limit  [2147483647 qc]
        r &= device_set_register(motor, REGISTER(0x607F, 0), 15000); // max_profile_velocity  Motor specific [25000 rpm]
        r &= device_set_register(motor, REGISTER(0x6081, 0), 5000);  // profile_velocity Desired Velocity [1000 rpm]
        r &= device_set_register(motor, REGISTER(0x6083, 0), 10000);  // profile_acceleration User specific [10000 rpm/s]
        r &= device_set_register(motor, REGISTER(0x6084, 0), 10000);  // profile_deceleration User specific [10000 rpm/s]
        r &= device_set_register(motor, REGISTER(0x6085, 0), 10000);  // quickstop_deceleration User specific [10000 rpm/s]
        r &= device_set_register(motor, REGISTER(0x6086, 0), 0);      // motion_profile_type  User specific [0]
        // brake config
        r &= device_set_register(motor, REGISTER(0x2078, 2), (1<<12));  // output mask bit for brake
        r &= device_set_register(motor, REGISTER(0x2078, 3), 0);        // output polarity bits
        r &= device_set_register(motor, REGISTER(0x2079, 4), 12);       // output 12 -> signal 4

        if (!r) {
                device_invalidate_register(motor, REG_CONTROL);   // force re-issue
                device_set_register(motor, REG_CONTROL, CONTROL_SHUTDOWN);
                return DEFUNCT;
        }

	return TARGETTING;
}

int sail_control(double* actual_angle_deg) {

        int r;
        uint32_t status;
        uint32_t error;
        uint32_t control;
        uint32_t opmode;
        int32_t curr_pos_qc;
        int32_t curr_targ_qc;
        int32_t curr_abspos_qc;

        VLOGF("sail_control(%f)", target_angle_deg);

        if (!device_get_register(motor, REG_STATUS, &status))
                return DEFUNCT;

	VLOGF("sail_control(%f) status 0x%x", target_angle_deg, status);

        if (status & STATUS_FAULT) {
                VLOGF("sail_control: clearing fault");
                device_invalidate_register(motor, REG_CONTROL);   // force re-issue
                device_set_register(motor, REG_CONTROL, CONTROL_CLEARFAULT);
		device_invalidate_register(motor, REG_ERROR);   // force re-issue
		device_get_register(motor, REG_ERROR, &error);  // we won't read it but eposmon will pick up the response.
                device_invalidate_register(motor, REG_STATUS);
                return HOMING;
        }

        r  = device_get_register(motor, REG_CONTROL, &control);
        r &= device_get_register(motor, REG_OPMODE,  &opmode);
        r &= device_get_register(motor, REG_CURRPOS, (uint32_t*)&curr_pos_qc);
        r &= device_get_register(motor, REG_TARGPOS, (uint32_t*)&curr_targ_qc);
        {
                // bmmh position is 30 bit signed,0 .. 0x4000 0000 -> 0x2000 0000 => -0x2000 0000
                r &= device_get_register(bmmh, REG_BMMHPOS, (uint32_t*)&curr_abspos_qc);
                if (curr_abspos_qc >= (1<<29)) curr_abspos_qc -= (1<<30);
                curr_abspos_qc = normalize_qc(&motor_params[BMMH], curr_abspos_qc);
        }

        if (!r) {
                VLOGF("sail control: incomplete status\n");
                return DEFUNCT;
        }

	if (opmode != OPMODE_PPM)
		return HOMING;

        *actual_angle_deg = qc_to_angle(&motor_params[BMMH], curr_abspos_qc);

        VLOGF("sail_control(%f <- %f)", target_angle_deg, *actual_angle_deg);

	if (isnan(target_angle_deg)) return REACHED;

        VLOGF("sail_control: configuration ok target %sreached",
              (status & STATUS_TARGETREACHED) ? "NOT ": "" );

        // we should operate the motor to make this angle zero
        double target_delta_deg = target_angle_deg - *actual_angle_deg;
        while (target_delta_deg < -180.0) target_delta_deg += 360.0;
        while (target_delta_deg >  180.0) target_delta_deg -= 360.0;

        VLOGF("sail_control: target delta: %f", target_delta_deg);

        int32_t new_targ_qc = curr_pos_qc + angle_to_qc(&motor_params[SAIL], target_delta_deg);
        int32_t delta_targ_qc = new_targ_qc - curr_targ_qc;
        if ((delta_targ_qc < -1000) || (delta_targ_qc > 1000)) {
                // pretend we didn't arrive and that we're not moving
                // for the decision tree beow
                status &= ~STATUS_TARGETREACHED;
                if (control == CONTROL_START) {
                        device_invalidate_register(motor, REG_CONTROL);
                        control &= ~0x30;
                }
        }

        VLOGF("sail_control: curr_pos_qc: %d curr_targ_qc: %d new_targ_qc: %d delta_targ_qc: %d",
              curr_pos_qc, curr_targ_qc, new_targ_qc, delta_targ_qc);


        if (!(status & STATUS_TARGETREACHED)) {
                VLOGF("sail_control: Status not reached, going to %d", new_targ_qc);
                if (!device_set_register(motor, REGISTER(0x2078, 1), 0)) {
                        VLOGF("sail_control: wait for break off");
                        return TARGETTING;
                }
                VLOGF("sail_control: break off confirmed");

                switch(control) {
                case CONTROL_SHUTDOWN:
                        VLOGF("sail_control: shutdown->switchon");
                        device_set_register(motor, REG_CONTROL, CONTROL_SWITCHON);
                        break;
                case CONTROL_SWITCHON:
                        VLOGF("sail_control: switchon -> start");
                        device_invalidate_register(motor, REG_TARGPOS);
                        device_set_register(motor, REG_TARGPOS, new_targ_qc);
                        device_set_register(motor, REG_CONTROL, CONTROL_START);
                        break;
                case CONTROL_START:
                        VLOGF("sail_control: moving, patience");
                        break;
                default:  // weird, shutdown first
                        VLOGF("sail_control: ? (%x) -> shutdown", control);
                        device_set_register(motor, REG_CONTROL, CONTROL_SHUTDOWN);
                        break;
                }
        } else {
                VLOGF("sail_control: Status Reached");
                device_set_register(motor, REG_CONTROL, CONTROL_SHUTDOWN);
                if (device_set_register(motor, REGISTER(0x2078, 1), (1<<12)))  // brake on
                        VLOGF("sail_control: brake on");
        }

        device_invalidate_register(bmmh,  REG_BMMHPOS);
        device_invalidate_register(motor, REG_CURRPOS);
        device_invalidate_register(motor, REG_STATUS);
	device_invalidate_register(motor, REG_ERROR);
        return (status & STATUS_TARGETREACHED) ? REACHED : TARGETTING;
}

// -----------------------------------------------------------------------------

const int64_t WARN_INTERVAL = 5000;
const int64_t MAX_INTERVAL = 15*60*1000;

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

	argv += optind;	argc -= optind;
	if (argc != 0) usage();

	if (signal(SIGBUS, fault) == SIG_ERR)  crash("signal(SIGBUS)");
	if (signal(SIGSEGV, fault) == SIG_ERR)  crash("signal(SIGSEGV)");

	if (!debug) openlog(argv0, LOG_PERROR, LOG_DAEMON);

	setlinebuf(stdout);
	bus = bus_new(stdout);
	motor = bus_open_device(bus, motor_params[SAIL].serial_number);
	bmmh  = bus_open_device(bus, motor_params[BMMH].serial_number);

	int64_t last_reached = now_ms();
	int64_t warn_interval = WARN_INTERVAL;
	double angle_deg = NAN;
	int state = DEFUNCT;

	for(;;) {
		syslog(LOG_WARNING, "Initializing sail.");

		while (state != TARGETTING)
			if (processinput())
				state = sail_init();

		syslog(LOG_WARNING, "Done initalizing sail.");

		while (state != HOMING) {
			if (processinput())
				state = sail_control(&angle_deg);

			if (isnan(target_angle_deg))
			    continue;

			int64_t now = now_ms();
			
			if (state == REACHED  || (now < last_reached)) {
				last_reached = now;
				warn_interval = WARN_INTERVAL;
			}

			if (now - last_reached > warn_interval) {
				syslog(LOG_WARNING, "Unable to target sail.");
				last_reached = now;  // shut up warning
				if (warn_interval < MAX_INTERVAL) warn_interval *= 2;
			}
		}
	}

	crash("main loop exit");
	return 0;
}


