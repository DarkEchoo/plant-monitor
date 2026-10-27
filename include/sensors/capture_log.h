#ifndef SENSORS_CAPTURE_LOG_H
#define SENSORS_CAPTURE_LOG_H

#include "sensors/capture.h"

#include <stdio.h>

/*
 * Text log lines for a capture process: one line per sample, heartbeat
 * and exit, for people reading the log and for evidence. This is NOT the
 * capture-to-hub message format and NOT the heartbeat protocol; neither
 * is agreed yet.
 *
 * Every line starts with SIMULATED or LIVE (from the sample's, the
 * heartbeat's or the caller's is_simulated) and is key=value text:
 * - sample: sequence, tick, phase, validity, value with an explicit unit
 *   (value=none when invalid, never 0), light raw_count, driver stage,
 *   transport status and errno (with errno_text), the requested and
 *   transferred lengths for a short transfer (docs/design.md keeps those
 *   out of the record), the rejected value, both timestamps (none plus
 *   the errno when unavailable) and the clock trust.
 * - heartbeat: the capture_heartbeat fields.
 * - exit: the exit reason and errno (with errno_text), and the open and
 *   close results, or not_attempted.
 *
 * Out-of-range enums print UNKNOWN (phase, unit) or INVALID (clock
 * trust), never a real value; a missing stage name prints stage=none.
 *
 * Each function writes and flushes one line, then returns 0, or EIO if
 * the flush fails or the stream's error indicator is set. A stream that
 * was already in error makes every later call return EIO. The pointers
 * must not be NULL.
 */

int capture_log_sample(FILE *out, const capture_sample *sample);

int capture_log_heartbeat(FILE *out, const capture_heartbeat *heartbeat);

int capture_log_exit(FILE *out, capture_sensor sensor, int is_simulated,
                     const capture_outcome *outcome);

/* Stable text: "light" or "soil" ("unknown" otherwise). */
const char *capture_sensor_name(capture_sensor sensor);

#endif
