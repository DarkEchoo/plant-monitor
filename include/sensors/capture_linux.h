#ifndef SENSORS_CAPTURE_LINUX_H
#define SENSORS_CAPTURE_LINUX_H

#include "sensors/capture.h"

/*
 * Real Linux seams for capture_run (sensors/capture.h):
 * - read_clock: clock_gettime(CLOCK_REALTIME or CLOCK_MONOTONIC).
 * - sleep_until: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME), so an
 *   interrupted sleep resumes toward the same deadline. EINTR is passed
 *   back for capture_run to check the stop flag.
 * - check_clock_trust: docs/design.md "Clock trust". adjtimex() with
 *   modes = 0 (read-only); the clock is trusted only when the call does
 *   not return TIME_ERROR and STA_UNSYNC is clear. A failed call is
 *   CAPTURE_CLOCK_TRUST_UNKNOWN with its errno, which the design treats as
 *   untrusted. Checked fresh on every call, never cached.
 * - open_device: i2c_linux_open (opens a real /dev/i2c-N).
 * - stop_requested: a flag set by the SIGTERM/SIGINT handler below.
 *
 * capture_linux_env does not install the handlers; a capture program calls
 * capture_linux_install_stop_handlers first. Only a capture program uses
 * these seams; there is no such program yet.
 */

/*
 * Install a handler for SIGTERM and SIGINT that only sets the stop flag,
 * with no sigaction flags: a blocked sleep returns EINTR, and the handler
 * stays installed for a second signal. Returns 0 or the errno of the
 * failed sigaction; if SIGINT fails, the SIGTERM handler stays installed
 * (the caller should exit).
 */
int capture_linux_install_stop_handlers(void);

/* Fill *env with the real seams; is_simulated is 0 and ctx is internal. */
void capture_linux_env(capture_env *env);

#endif
