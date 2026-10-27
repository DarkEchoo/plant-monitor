#ifndef SENSORS_CAPTURE_LINUX_SYS_H
#define SENSORS_CAPTURE_LINUX_SYS_H

/*
 * Internal syscall seam for the capture Linux seams. Only capture_linux.c
 * and its tests use it. Including files must define _POSIX_C_SOURCE
 * 200809L first (for struct sigaction and clockid_t).
 *
 * A replacement function that fails must report its error exactly as the
 * real call does: set errno, or for clock_sleep return the error number.
 */

#include "sensors/capture.h"

#include <signal.h>
#include <sys/timex.h>
#include <time.h>

typedef struct {
    /* clock_gettime(clock, now); -1 and errno on failure. */
    int (*clock_now)(clockid_t clock, struct timespec *now);
    /* clock_nanosleep(clock, flags, deadline, NULL): returns 0 or an error
     * number directly, not through errno. */
    int (*clock_sleep)(clockid_t clock, int flags,
                       const struct timespec *deadline);
    /* adjtimex(tx): the clock state, or -1 and errno on failure. */
    int (*adjtimex)(struct timex *tx);
    /* sigaction(sig, act, NULL); -1 and errno on failure. */
    int (*sigaction)(int sig, const struct sigaction *act);
} capture_linux_sys;

extern const capture_linux_sys capture_linux_real_sys;

/* capture_linux_env with an explicit syscall table; sys must outlive env. */
void capture_linux_env_with(capture_env *env, const capture_linux_sys *sys);

/* capture_linux_install_stop_handlers with an explicit syscall table. */
int capture_linux_install_stop_handlers_with(const capture_linux_sys *sys);

/* Clear the stop flag (tests only). */
void capture_linux_clear_stop(void);

#endif
