#define _POSIX_C_SOURCE 200809L

#include "sensors/capture_linux.h"
#include "capture_linux_sys.h"

#include "sensors/i2c_linux.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/timex.h>
#include <time.h>

#define NSEC_PER_SEC INT64_C(1000000000)

/* Largest time_t; Linux time_t is a signed 32- or 64-bit integer. */
#define CAPTURE_TIME_T_MAX \
    ((time_t)(((uintmax_t)1 << (sizeof(time_t) * CHAR_BIT - 1)) - 1))

/* Set only by stop_handler; read by linux_stop_requested. */
static volatile sig_atomic_t stop_flag;

/* errno_or_eio: errno, or EIO if a failed call left it 0. */
static int errno_or_eio(void)
{
    return errno != 0 ? errno : EIO;
}

/* real_clock_now: clock_gettime. */
static int real_clock_now(clockid_t clock, struct timespec *now)
{
    return clock_gettime(clock, now);
}

/* real_clock_sleep: clock_nanosleep; returns 0 or an error number. */
static int real_clock_sleep(clockid_t clock, int flags,
                            const struct timespec *deadline)
{
    return clock_nanosleep(clock, flags, deadline, NULL);
}

/* real_adjtimex: adjtimex; the clock state, or -1 and errno. */
static int real_adjtimex(struct timex *tx)
{
    return adjtimex(tx);
}

/* real_sigaction: sigaction without reading the old action. */
static int real_sigaction(int sig, const struct sigaction *act)
{
    return sigaction(sig, act, NULL);
}

/* capture_linux_real_sys: the seam table of real calls used in product. */
const capture_linux_sys capture_linux_real_sys = {
    real_clock_now, real_clock_sleep, real_adjtimex, real_sigaction
};

/* sys_of: the syscall table kept in the env's ctx. */
static const capture_linux_sys *sys_of(void *ctx)
{
    return (const capture_linux_sys *)ctx;
}

/* linux_open: the Linux /dev/i2c-N backend. */
static i2c_result linux_open(void *ctx, i2c_device *dev,
                             unsigned bus_number, uint8_t address_7bit,
                             int *cleanup_errno)
{
    (void)ctx;
    return i2c_linux_open(dev, bus_number, address_7bit, cleanup_errno);
}

/* linux_read_clock: CLOCK_REALTIME or CLOCK_MONOTONIC; 0 or the errno. */
static int linux_read_clock(void *ctx, capture_clock clock,
                            struct timespec *out)
{
    clockid_t id = clock == CAPTURE_CLOCK_WALL ? CLOCK_REALTIME
                                               : CLOCK_MONOTONIC;

    errno = 0;
    if (sys_of(ctx)->clock_now(id, out) != 0) {
        return errno_or_eio();
    }
    return 0;
}

/*
 * linux_sleep_until: absolute CLOCK_MONOTONIC sleep; 0, EINTR, or another
 * error number. A negative deadline is EINVAL, and one a 32-bit time_t
 * cannot hold is EOVERFLOW, both before any sleep. (The EOVERFLOW path
 * cannot occur with a 64-bit time_t, so it is untested on x86-64.)
 */
static int linux_sleep_until(void *ctx, int64_t deadline_mono_ns)
{
    struct timespec deadline;
    int64_t sec;

    if (deadline_mono_ns < 0) {
        return EINVAL;
    }
    sec = deadline_mono_ns / NSEC_PER_SEC;
    if ((uintmax_t)sec > (uintmax_t)CAPTURE_TIME_T_MAX) {
        return EOVERFLOW;
    }
    deadline.tv_sec = (time_t)sec;
    deadline.tv_nsec = (long)(deadline_mono_ns % NSEC_PER_SEC);
    return sys_of(ctx)->clock_sleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                    &deadline);
}

/*
 * linux_clock_trust: docs/design.md "Clock trust". Read-only adjtimex
 * (modes = 0); trusted only without TIME_ERROR and with STA_UNSYNC clear.
 */
static capture_clock_trust linux_clock_trust(void *ctx, int *err)
{
    struct timex tx;
    int state;

    memset(&tx, 0, sizeof tx);
    tx.modes = 0;
    errno = 0;
    state = sys_of(ctx)->adjtimex(&tx);
    if (state == -1) {
        *err = errno_or_eio();
        return CAPTURE_CLOCK_TRUST_UNKNOWN;
    }
    if (state == TIME_ERROR || (tx.status & STA_UNSYNC) != 0) {
        return CAPTURE_CLOCK_UNTRUSTED;
    }
    return CAPTURE_CLOCK_TRUSTED;
}

/* linux_stop_requested: nonzero once SIGTERM or SIGINT arrived. */
static int linux_stop_requested(void *ctx)
{
    (void)ctx;
    return stop_flag != 0;
}

/* stop_handler: async-signal-safe; it only sets the flag. */
static void stop_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* capture_linux_env_with: see capture_linux_sys.h. */
void capture_linux_env_with(capture_env *env, const capture_linux_sys *sys)
{
    env->open_device = linux_open;
    env->read_clock = linux_read_clock;
    env->sleep_until = linux_sleep_until;
    env->check_clock_trust = linux_clock_trust;
    env->stop_requested = linux_stop_requested;
    env->ctx = (void *)sys;   /* only ever read back as const */
    env->is_simulated = 0;
}

/* capture_linux_env: the real seams. */
void capture_linux_env(capture_env *env)
{
    capture_linux_env_with(env, &capture_linux_real_sys);
}

/*
 * capture_linux_install_stop_handlers_with: SIGTERM, then SIGINT, with no
 * flags. clock_nanosleep returns EINTR after a handler whatever the
 * flags; leaving SA_RESTART off also keeps any future blocking read or
 * write from being silently restarted, and leaving SA_RESETHAND off keeps
 * the handler for a second signal.
 */
int capture_linux_install_stop_handlers_with(const capture_linux_sys *sys)
{
    static const int sigs[] = {SIGTERM, SIGINT};
    struct sigaction act;
    size_t i;

    memset(&act, 0, sizeof act);
    act.sa_handler = stop_handler;
    act.sa_flags = 0;
    errno = 0;
    if (sigemptyset(&act.sa_mask) != 0) {
        return errno_or_eio();
    }
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        errno = 0;
        if (sys->sigaction(sigs[i], &act) != 0) {
            return errno_or_eio();
        }
    }
    return 0;
}

/* capture_linux_install_stop_handlers: with the real sigaction. */
int capture_linux_install_stop_handlers(void)
{
    return capture_linux_install_stop_handlers_with(&capture_linux_real_sys);
}

/* capture_linux_clear_stop: see capture_linux_sys.h (tests only). */
void capture_linux_clear_stop(void)
{
    stop_flag = 0;
}
