// Copyright 2011 The Avalon Project Authors. All rights reserved.
// Use of this source code is governed by the Apache License 2.0
// that can be found in the LICENSE file.
//
// Decode status registers on ebus and output ruddersts messages on lbus.
// Use plug -o and plug -i to connect to the busses.
// 

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../proto/rudder.h"
#include "actuator.h"

// -----------------------------------------------------------------------------
//   Together with getopt in main, this is our minimalistic UI
// -----------------------------------------------------------------------------
// static const char* version = "$Id: $";
static const char* argv0;
static int verbose = 0;
static int debug = 0;

static void
crash(const char* fmt, ...)
{
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

static void
usage(void)
{
	fprintf(stderr,
		"usage: plug -i /path/to/ebus | %s [options] | plug -o /path/to/lbus \n"
		"options:\n"
		"\t-n miNimum time [ms] between reports (default 250ms)\n"
		"\t-x maXimum time [ms] between reports (default 1s)\n"
		, argv0);
	exit(2);
}

static int64_t
now_us() 
{
        struct timeval tv;
        if (gettimeofday(&tv, NULL) < 0) crash("no working clock");

        int64_t ms1 = tv.tv_sec;  ms1 *= 1000000;
        int64_t ms2 = tv.tv_usec;
        return ms1 + ms2;
}

// update x and return true if abs(difference) > .1
int upd(double* x, double y) {
	double r = y - *x;
	*x = y;
	return r*r > .01;
}

// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {

	int ch;
	int64_t min_us = 125 *1000;
	int64_t max_us = 1000 *1000;

	argv0 = strrchr(argv[0], '/');
	if (argv0) ++argv0; else argv0 = argv[0];

	while ((ch = getopt(argc, argv, "dhn:vx:")) != -1){
		switch (ch) {
		case 'd': ++debug; break;
		case 'n': min_us = 1000*atoi(optarg); break;
		case 'x': max_us = 1000*atoi(optarg); break;
		case 'v': ++verbose; break;
		case 'h': 
		default:
			usage();
		}
	}

	argv += optind;
	argc -= optind;

	if (argc != 0) usage();

	if (signal(SIGBUS, fault) == SIG_ERR)  crash("signal(SIGBUS)");
	if (signal(SIGSEGV, fault) == SIG_ERR)  crash("signal(SIGSEGV)");

	if (!debug) openlog(argv0, LOG_PERROR, LOG_DAEMON);

	setlinebuf(stdout);

	struct RudderProto sts = INIT_RUDDERPROTO;

	int64_t last_us = now_us();

	int homed[2] = { 0, 0 };

	while(!feof(stdin)) {
		char line[1024];
		if (!fgets(line, sizeof(line), stdin))
			crash("reading stdin");

		 if (line[0] == '#') continue;

		int64_t now = now_us();

                uint32_t serial = 0;
		int index       = 0;
		int subindex    = 0;
		int64_t value_l = 0;  // fumble signedness

		int n = sscanf(line, "%i:%i[%i] = %lli", &serial, &index, &subindex, &value_l);
		if (n != 4) continue;
		
		uint32_t value = value_l;
		int reg = REGISTER(index,subindex);
		int changed = 0;

		if (serial == motor_params[BMMH].serial_number && reg == REG_BMMHPOS) {
			changed = upd(&sts.sail_deg, qc_to_angle(motor_params[BMMH], value));
		} else if (serial == motor_params[LEFT].serial_number && reg == REG_STATUS) {
			homed[LEFT] = value&STATUS_HOMEREF;
			changed = (!homed[LEFT] && !isnan(sts.rudder_l_deg));
			if (changed) sts.ruder_l_deg = NAN;
		} else if (serial == motor_params[RIGHT].serial_number && reg == REG_STATUS) {
			homed[RIGHT] = value&STATUS_HOMEREF;
			changed = (!homed[RIGHT] && !isnan(sts.rudder_r_deg));
			if (changed) sts.ruder_r_deg = NAN;
		} else if (serial == motor_params[LEFT].serial_number && reg == REG_CURRPOS && homed[LEFT]) {
			changed = upd(&sts.rudder_l_deg, qc_to_angle(motor_params[LEFT], value));
		} else if (serial == motor_params[RIGHT].serial_number && reg == REG_CURRPOS && homed[RIGHT]) {
			changed = upd(&sts.rudder_r_deg, qc_to_angle(motor_params[RIGHT], value));
		}

		if (!changed && (last_us + max_us > now)) {
			continue;
		}

		if (last_us + min_us > now)
			continue;

		int dum;
		printf(OFMT_RUDDERPROTO_STS(sts, &dum));
		last_us = now_us();
	}

	crash("main loop exit");
	return 0;
}
