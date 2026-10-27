#ifndef SENSORS_CAPTURE_ARGS_H
#define SENSORS_CAPTURE_ARGS_H

#include "sensors/capture.h"

#include <stdio.h>

/*
 * Command-line options for the future capture program: one program, run
 * as two processes (one per sensor). These options do not depend on the
 * capture-to-hub interface, which is not agreed yet, so there are no
 * output or socket options here.
 *
 *   --sensor light|soil  required
 *   --bus N              required; no default, because init writes to the
 *                        bus (BH1750 Power On, soil software reset)
 *   --addr ADDR          light 0x23 (default) or 0x5c; soil 0x36 (default)
 *                        to 0x39
 *   --tick-ms N          PROPOSED 10000; 100 to 3600000
 *   --every N            ticks between attempts; PROPOSED light 1, soil 6;
 *                        1 to 1000
 *   --help
 *
 * Numbers are decimal; addresses may also be hex (0x23). Options may not
 * repeat, and a value may not start with "--". Arguments are checked in
 * order, so --help wins only when no earlier argument is an error (as in
 * sensor-diag). Every other capture_config field keeps its proposed value
 * (capture_config_proposed).
 */

#define CAPTURE_ARGS_TICK_MS_MIN 100u
#define CAPTURE_ARGS_TICK_MS_MAX 3600000u
#define CAPTURE_ARGS_EVERY_MAX   1000u

typedef enum {
    CAPTURE_ARGS_RUN,    /* *cfg is filled in */
    CAPTURE_ARGS_HELP,   /* --help was given */
    CAPTURE_ARGS_ERROR   /* an error was printed to err */
} capture_args_result;

/* Parse argv. On RUN *cfg is written; otherwise it is untouched. */
capture_args_result capture_parse_args(int argc, char *const argv[],
                                       capture_config *cfg, FILE *err);

/* Usage text, marking the rates as proposals. */
void capture_usage(FILE *out, const char *program);

/*
 * Process exit status for a run's result: 0 for STOPPED, 2 for USAGE and
 * 1 for every failure. PROVISIONAL until the supervisor's needs are
 * agreed; the program also logs capture_exit_name.
 */
int capture_exit_status(capture_exit result);

#endif
