#define _POSIX_C_SOURCE 200809L

#include "sensors/i2c_linux.h"
#include "sensors/sensor_diag.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

/*
 * sensor-diag program: the real seams around sensor_diag_run. Only this
 * file reaches /dev/i2c-N and the system clocks, so the test suite never
 * builds or runs it.
 */

/* linux_open: open the device through the Linux /dev/i2c-N backend. */
static i2c_result linux_open(void *ctx, i2c_device *dev,
                             unsigned bus_number, uint8_t address_7bit,
                             int *cleanup_errno)
{
    (void)ctx;
    return i2c_linux_open(dev, bus_number, address_7bit, cleanup_errno);
}

/* system_clock: CLOCK_REALTIME or CLOCK_MONOTONIC; 0 or the errno. */
static int system_clock(void *ctx, sensor_diag_clock clock,
                        struct timespec *out)
{
    clockid_t id = clock == SENSOR_DIAG_CLOCK_WALL ? CLOCK_REALTIME
                                                   : CLOCK_MONOTONIC;

    (void)ctx;
    if (clock_gettime(id, out) != 0) {
        return errno != 0 ? errno : EIO;
    }
    return 0;
}

/* main: parse the arguments, then check both sensors on the given bus. */
int main(int argc, char **argv)
{
    sensor_diag_env env = {linux_open, system_clock, NULL, 0};
    sensor_diag_config cfg;
    const char *program =
        argc > 0 && argv[0] != NULL ? argv[0] : "sensor-diag";
    int rc;

    /*
     * Line-buffer stdout so each line reaches a redirected log as it is
     * printed: if a transfer hangs and the run is killed, the lines so far
     * show which step it reached. Must precede any output.
     */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "%s: warning: cannot line-buffer stdout; output "
                        "may be lost if the run is killed\n", program);
    }

    switch (sensor_diag_parse_args(argc, argv, &cfg, stderr)) {
    case SENSOR_DIAG_ARGS_HELP:
        sensor_diag_usage(stdout, program);
        if (fflush(stdout) != 0 || ferror(stdout)) {
            return SENSOR_DIAG_EXIT_FAILURE;
        }
        return SENSOR_DIAG_EXIT_OK;
    case SENSOR_DIAG_ARGS_ERROR:
        fprintf(stderr, "Try '%s --help'.\n", program);
        return SENSOR_DIAG_EXIT_USAGE;
    case SENSOR_DIAG_ARGS_RUN:
        break;
    }

    rc = sensor_diag_run(&cfg, &env, stdout);
    if (ferror(stdout)) {
        fprintf(stderr, "%s: writing the output failed\n", program);
    }
    return rc;
}
