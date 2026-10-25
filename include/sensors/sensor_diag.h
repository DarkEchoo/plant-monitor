#ifndef SENSORS_SENSOR_DIAG_H
#define SENSORS_SENSOR_DIAG_H

#include "sensors/i2c_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/*
 * sensor-diag: a one-shot hardware check that opens the BH1750 and the
 * seesaw soil sensor, initializes each once, takes a few measurements and
 * prints one line per step. It connects the drivers to a bus; it does no
 * analysis, calibration, trends, fusion or plant-care advice, and it never
 * retries or re-initializes.
 *
 * The output is a DIAGNOSTIC format for people reading a terminal. It is not
 * the storage record format, which PR #5's design still defines.
 *
 * Output rules:
 * - Every line starts with SIMULATED or LIVE. A line is SIMULATED if the
 *   environment or the device it describes is simulated.
 * - A reading is valid only when the driver reports overall success (stage
 *   NONE). A failed or rejected reading prints valid=0 and value=none, never
 *   a zero value.
 * - Units: light is integer millilux (unit=millilux), soil is the raw seesaw
 *   capacitance count (unit=raw_counts, uncalibrated).
 * - Both clocks are read immediately after every measurement attempt,
 *   successful or not, wall clock first. They mark when the attempt
 *   completed, not when it started. clock_sync is always "unknown": the tool
 *   does not judge whether the wall clock is right.
 *
 * The checks run in order: light, then soil. A failure on one sensor never
 * stops the other from being checked. Any failed operation (open, init,
 * measurement, clock read, close or writing the output) makes the run fail.
 */

#define SENSOR_DIAG_EXIT_OK      0  /* every operation succeeded */
#define SENSOR_DIAG_EXIT_FAILURE 1  /* at least one operation failed */
#define SENSOR_DIAG_EXIT_USAGE   2  /* bad arguments; nothing was opened */

#define SENSOR_DIAG_SAMPLES_DEFAULT 1u
#define SENSOR_DIAG_SAMPLES_MAX     100u

typedef struct {
    unsigned bus_number;          /* /dev/i2c-<bus_number>; no default */
    uint8_t light_address_7bit;   /* 0x23 (default) or 0x5c */
    uint8_t soil_address_7bit;    /* 0x36 (default) to 0x39 */
    unsigned samples;             /* measurements per sensor, 1 to 100 */
} sensor_diag_config;

typedef enum {
    SENSOR_DIAG_ARGS_RUN,    /* *cfg is filled in */
    SENSOR_DIAG_ARGS_HELP,   /* --help was given */
    SENSOR_DIAG_ARGS_ERROR   /* an error was printed to err */
} sensor_diag_args;

typedef enum {
    SENSOR_DIAG_CLOCK_WALL,       /* CLOCK_REALTIME */
    SENSOR_DIAG_CLOCK_MONOTONIC   /* CLOCK_MONOTONIC */
} sensor_diag_clock;

/*
 * Where devices and time come from. The program uses the Linux backend and
 * clock_gettime; tests use the scripted mock and a fake clock.
 * - open_device follows i2c_linux_open's contract, including cleanup_errno.
 * - read_clock returns 0, or the errno of the failed clock read.
 * - is_simulated labels lines printed before a device is open.
 */
typedef struct {
    i2c_result (*open_device)(void *ctx, i2c_device *dev,
                              unsigned bus_number, uint8_t address_7bit,
                              int *cleanup_errno);
    int (*read_clock)(void *ctx, sensor_diag_clock clock,
                      struct timespec *out);
    void *ctx;
    int is_simulated;
} sensor_diag_env;

/*
 * Parse argv. --bus is required. Numbers are decimal, addresses may also be
 * hex (0x23). Options may not repeat. On RUN *cfg is written; otherwise it
 * is untouched. Errors are reported to err.
 */
sensor_diag_args sensor_diag_parse_args(int argc, char *const argv[],
                                        sensor_diag_config *cfg, FILE *err);

/* Usage text, including what a short check can and cannot show. */
void sensor_diag_usage(FILE *out, const char *program);

/*
 * Convert a clock reading to nanoseconds. Returns 0, EINVAL if tv_nsec is
 * outside 0-999999999, or ERANGE if tv_sec is negative or the result does
 * not fit in int64_t. *out_ns is written only on success.
 */
int sensor_diag_timespec_to_ns(const struct timespec *ts, int64_t *out_ns);

/*
 * Run the check and print to out. Returns a SENSOR_DIAG_EXIT_ value; USAGE
 * if cfg, env or out is unusable, in which case nothing is opened or
 * printed. Every device opened here is closed before returning.
 */
int sensor_diag_run(const sensor_diag_config *cfg,
                    const sensor_diag_env *env, FILE *out);

#endif
