#define _POSIX_C_SOURCE 200809L

#include "sensors/capture.h"
#include "sensors/capture_linux.h"
#include "sensors/i2c_mock.h"
#include "../src/sensors/capture_linux_sys.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <time.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Two kinds of tests:
 * - SIMULATED: fake syscalls drive every error path, and one run of
 *   capture_run uses the real clocks and sleep with the scripted I2C mock
 *   as its device.
 * - OS PLUMBING: the real clock_gettime, clock_nanosleep, adjtimex (read
 *   only, modes = 0) and sigaction are called. Signals are raised only at
 *   this test process.
 * No test opens /dev/i2c-* except the absent bus UINT_MAX, after stat()
 * confirms it does not exist. No value is a measurement.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define NSEC_PER_SEC INT64_C(1000000000)

/* What the fake syscalls saw and should return. */
static struct {
    clockid_t clock_seen;
    int clock_ret;
    int clock_errno;
    clockid_t sleep_clock;
    int sleep_flags;
    struct timespec sleep_deadline;
    unsigned sleep_calls;
    int sleep_ret;
    int timex_modes;
    int timex_ret;
    int timex_status;
    int timex_errno;
    unsigned sigaction_calls;
    int sigaction_fail_call;   /* 1-based; 0 none */
    int sigaction_errno;
    int signals[4];
    struct sigaction last_act;
} fk;

/* fake_clock_now: record the clock, return 12.5 s or fail. */
static int fake_clock_now(clockid_t clock, struct timespec *now)
{
    fk.clock_seen = clock;
    if (fk.clock_ret != 0) {
        errno = fk.clock_errno;
        return fk.clock_ret;
    }
    now->tv_sec = 12;
    now->tv_nsec = 500000000L;
    return 0;
}

/* fake_clock_sleep: record the call and return the configured error. */
static int fake_clock_sleep(clockid_t clock, int flags,
                            const struct timespec *deadline)
{
    fk.sleep_calls++;
    fk.sleep_clock = clock;
    fk.sleep_flags = flags;
    fk.sleep_deadline = *deadline;
    return fk.sleep_ret;
}

/* fake_adjtimex: record modes, report the configured state. */
static int fake_adjtimex(struct timex *tx)
{
    fk.timex_modes = (int)tx->modes;
    if (fk.timex_ret == -1) {
        errno = fk.timex_errno;
        return -1;
    }
    tx->status = fk.timex_status;
    return fk.timex_ret;
}

/* fake_sigaction: record the signal and action, or fail on one call. */
static int fake_sigaction(int sig, const struct sigaction *act)
{
    unsigned n = ++fk.sigaction_calls;

    assert(n <= COUNT_OF(fk.signals));
    fk.signals[n - 1] = sig;
    fk.last_act = *act;
    if ((int)n == fk.sigaction_fail_call) {
        errno = fk.sigaction_errno;
        return -1;
    }
    return 0;
}

static const capture_linux_sys fake_sys = {
    fake_clock_now, fake_clock_sleep, fake_adjtimex, fake_sigaction
};

/* fake_env: the seams over the fake syscalls, with fk cleared. */
static capture_env fake_env(void)
{
    capture_env env;

    memset(&fk, 0, sizeof fk);
    capture_linux_env_with(&env, &fake_sys);
    return env;
}

/* test_env_filled: every seam is set; output is labelled live. */
static void test_env_filled(void)
{
    capture_env env;

    memset(&env, 0xa5, sizeof env);
    capture_linux_env(&env);
    assert(env.open_device != NULL && env.read_clock != NULL &&
           env.sleep_until != NULL && env.check_clock_trust != NULL &&
           env.stop_requested != NULL);
    assert(env.is_simulated == 0);
}

/* test_read_clock_maps_clocks: wall is REALTIME, monotonic is MONOTONIC. */
static void test_read_clock_maps_clocks(void)
{
    capture_env env = fake_env();
    struct timespec ts;

    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_WALL, &ts) == 0);
    assert(fk.clock_seen == CLOCK_REALTIME);
    assert(ts.tv_sec == 12 && ts.tv_nsec == 500000000L);
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_MONOTONIC, &ts) == 0);
    assert(fk.clock_seen == CLOCK_MONOTONIC);
}

/* test_read_clock_failure: the errno comes back; errno 0 becomes EIO. */
static void test_read_clock_failure(void)
{
    capture_env env = fake_env();
    struct timespec ts;

    fk.clock_ret = -1;
    fk.clock_errno = EINVAL;
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_WALL, &ts) == EINVAL);
    fk.clock_errno = 0;
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_MONOTONIC, &ts) == EIO);
}

/*
 * test_sleep_until_absolute: an absolute CLOCK_MONOTONIC sleep with the
 * deadline split into seconds and nanoseconds; the result is passed back
 * unchanged, including EINTR.
 */
static void test_sleep_until_absolute(void)
{
    static const int results[] = {0, EINTR, EINVAL};
    size_t i;

    for (i = 0; i < COUNT_OF(results); i++) {
        capture_env env = fake_env();

        fk.sleep_ret = results[i];
        assert(env.sleep_until(env.ctx, INT64_C(15000000123)) == results[i]);
        assert(fk.sleep_calls == 1 && fk.sleep_clock == CLOCK_MONOTONIC);
        assert(fk.sleep_flags == TIMER_ABSTIME);
        assert(fk.sleep_deadline.tv_sec == 15 &&
               fk.sleep_deadline.tv_nsec == 123);
    }
}

/* test_sleep_until_negative: refused before any sleep. */
static void test_sleep_until_negative(void)
{
    capture_env env = fake_env();

    assert(env.sleep_until(env.ctx, -1) == EINVAL);
    assert(fk.sleep_calls == 0);
}

/*
 * test_clock_trust: modes = 0 (read only); trusted only without
 * TIME_ERROR and with STA_UNSYNC clear; a failed call is UNKNOWN with its
 * errno, and errno 0 becomes EIO.
 */
static void test_clock_trust(void)
{
    static const struct {
        int ret;
        int status;
        int err;
        capture_clock_trust want;
        int want_err;
    } cases[] = {
        {TIME_OK, 0, 0, CAPTURE_CLOCK_TRUSTED, 0},
        {TIME_INS, 0, 0, CAPTURE_CLOCK_TRUSTED, 0},
        {TIME_OK, STA_UNSYNC, 0, CAPTURE_CLOCK_UNTRUSTED, 0},
        {TIME_ERROR, 0, 0, CAPTURE_CLOCK_UNTRUSTED, 0},
        {TIME_ERROR, STA_UNSYNC, 0, CAPTURE_CLOCK_UNTRUSTED, 0},
        {-1, 0, EPERM, CAPTURE_CLOCK_TRUST_UNKNOWN, EPERM},
        {-1, 0, 0, CAPTURE_CLOCK_TRUST_UNKNOWN, EIO},
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        capture_env env = fake_env();
        int err = 777;

        fk.timex_modes = -1;
        fk.timex_ret = cases[i].ret;
        fk.timex_status = cases[i].status;
        fk.timex_errno = cases[i].err;
        assert(env.check_clock_trust(env.ctx, &err) == cases[i].want);
        assert(fk.timex_modes == 0);
        if (cases[i].want == CAPTURE_CLOCK_TRUST_UNKNOWN) {
            assert(err == cases[i].want_err);
        }
    }
}

/*
 * test_install_handlers_fake: SIGTERM then SIGINT, a handler (not
 * SIG_IGN/SIG_DFL) and no flags at all: not SA_RESTART, SA_SIGINFO, or
 * SA_RESETHAND (which would let a second SIGTERM kill the process
 * mid-close). A failure returns its errno and stops there.
 */
static void test_install_handlers_fake(void)
{
    memset(&fk, 0, sizeof fk);
    assert(capture_linux_install_stop_handlers_with(&fake_sys) == 0);
    assert(fk.sigaction_calls == 2);
    assert(fk.signals[0] == SIGTERM && fk.signals[1] == SIGINT);
    assert(fk.last_act.sa_handler != SIG_DFL &&
           fk.last_act.sa_handler != SIG_IGN);
    assert(fk.last_act.sa_flags == 0);

    memset(&fk, 0, sizeof fk);
    fk.sigaction_fail_call = 1;
    fk.sigaction_errno = EINVAL;
    assert(capture_linux_install_stop_handlers_with(&fake_sys) == EINVAL);
    assert(fk.sigaction_calls == 1);

    memset(&fk, 0, sizeof fk);
    fk.sigaction_fail_call = 2;
    fk.sigaction_errno = 0;
    assert(capture_linux_install_stop_handlers_with(&fake_sys) == EIO);
}

/*
 * test_real_clocks: OS PLUMBING. Both clocks read; the monotonic clock
 * does not go backwards.
 */
static void test_real_clocks(void)
{
    capture_env env;
    struct timespec a;
    struct timespec b;

    capture_linux_env(&env);
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_WALL, &a) == 0);
    assert(a.tv_sec > 0);
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_MONOTONIC, &a) == 0);
    assert(env.read_clock(env.ctx, CAPTURE_CLOCK_MONOTONIC, &b) == 0);
    assert(b.tv_sec > a.tv_sec ||
           (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
}

/* mono_ns: the real monotonic clock in nanoseconds. */
static int64_t mono_ns(void)
{
    struct timespec ts;

    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

/*
 * test_real_sleep_until: OS PLUMBING. A 2 ms absolute sleep returns 0 at
 * or after its deadline; a deadline in the past returns at once.
 */
static void test_real_sleep_until(void)
{
    capture_env env;
    int64_t deadline;

    capture_linux_env(&env);
    deadline = mono_ns() + 2000000;
    assert(env.sleep_until(env.ctx, deadline) == 0);
    assert(mono_ns() >= deadline);
    assert(env.sleep_until(env.ctx, mono_ns() - 1000000) == 0);
}

/*
 * test_real_clock_trust: OS PLUMBING. The read-only adjtimex call returns
 * one of the three states. Which one depends on this machine, so it is
 * only printed, not checked.
 */
static void test_real_clock_trust(void)
{
    capture_env env;
    capture_clock_trust t;
    int err = 0;

    capture_linux_env(&env);
    t = env.check_clock_trust(env.ctx, &err);
    assert(t == CAPTURE_CLOCK_TRUSTED || t == CAPTURE_CLOCK_UNTRUSTED ||
           (t == CAPTURE_CLOCK_TRUST_UNKNOWN && err != 0));
    printf("OS PLUMBING clock trust on this machine: %s\n",
           t == CAPTURE_CLOCK_TRUSTED     ? "TRUSTED"
           : t == CAPTURE_CLOCK_UNTRUSTED ? "UNTRUSTED"
                                          : "UNKNOWN");
}

/*
 * test_real_stop_handlers: OS PLUMBING. After installing the handlers,
 * SIGTERM and SIGINT raised at this process set the stop flag instead of
 * ending it, and still do on a second signal (the handler stays
 * installed). It runs last, so the handlers it leaves installed affect no
 * other test.
 */
static void test_real_stop_handlers(void)
{
    static const int sigs[] = {SIGTERM, SIGINT};
    capture_env env;
    size_t i;

    capture_linux_env(&env);
    assert(capture_linux_install_stop_handlers() == 0);
    for (i = 0; i < 2 * COUNT_OF(sigs); i++) {
        capture_linux_clear_stop();
        assert(env.stop_requested(env.ctx) == 0);
        assert(raise(sigs[i % COUNT_OF(sigs)]) == 0);
        assert(env.stop_requested(env.ctx) != 0);
    }
    capture_linux_clear_stop();
}

/*
 * test_real_open_absent_bus: OS PLUMBING. open_device reaches the Linux
 * backend: the absent bus UINT_MAX fails with ENOENT. Skipped if that
 * node somehow exists.
 */
static void test_real_open_absent_bus(void)
{
    capture_env env;
    i2c_device dev = {0};
    struct stat st;
    int cleanup_errno = -1;
    i2c_result r;

    if (stat("/dev/i2c-4294967295", &st) == 0 || UINT_MAX != 4294967295u) {
        puts("OS PLUMBING absent-bus open skipped");
        return;
    }
    capture_linux_env(&env);
    r = env.open_device(env.ctx, &dev, UINT_MAX, 0x23, &cleanup_errno);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == ENOENT);
    assert(cleanup_errno == 0 && dev.ops == NULL);
}

/* The scripted light sensor for the real-time run, and its stop count. */
static const uint8_t cmd_power_on[] = {0x01};
static const uint8_t cmd_mtreg_high[] = {0x42};
static const uint8_t cmd_mtreg_low[] = {0x65};
static const uint8_t cmd_one_time_h[] = {0x20};
static const uint8_t light_raw_12[] = {0x00, 0x0c};

static struct {
    i2c_mock mock;
    i2c_mock_step steps[3 + 3 * 4];
    unsigned heartbeats;
    unsigned samples;
    capture_sample sample[4];
} rt;

/* rt_step: one scripted step: a write/read of len bytes, or a wait. */
static i2c_mock_step rt_step(i2c_mock_op op, const uint8_t *bytes,
                             size_t len, uint32_t min_wait_us)
{
    i2c_mock_step step = {op, bytes, len, len, 0, min_wait_us};
    return step;
}

/* rt_open: the scripted mock instead of a real bus. */
static i2c_result rt_open(void *ctx, i2c_device *dev, unsigned bus_number,
                          uint8_t address_7bit, int *cleanup_errno)
{
    i2c_result r = {I2C_STATUS_OK, 0, 0, 0};

    (void)ctx;
    (void)bus_number;
    *cleanup_errno = 0;
    assert(i2c_mock_open(&rt.mock, dev, address_7bit, rt.steps,
                         COUNT_OF(rt.steps)) == I2C_STATUS_OK);
    return r;
}

/* rt_stop: stop after four ticks. */
static int rt_stop(void *ctx)
{
    (void)ctx;
    return rt.heartbeats >= 4;
}

/* rt_sample: keep each sample. */
static int rt_sample(void *ctx, const capture_sample *s)
{
    (void)ctx;
    assert(rt.samples < COUNT_OF(rt.sample));
    rt.sample[rt.samples++] = *s;
    return 0;
}

/*
 * rt_heartbeat: count heartbeats; each must be emitted at or after its
 * tick's deadline on the real monotonic clock (the sleep really waited).
 */
static int rt_heartbeat(void *ctx, const capture_heartbeat *h)
{
    (void)ctx;
    assert(rt.heartbeats < 4);
    assert(mono_ns() >= h->scheduled_mono_ns);
    rt.heartbeats++;
    return 0;
}

/*
 * test_real_time_capture_run: SIMULATED device, real clocks, real sleep,
 * real clock-trust check. A 20 ms tick for four ticks: every heartbeat
 * comes at or after its deadline (checked in rt_heartbeat), every sample
 * is valid and SIMULATED,
 * and the run stops cleanly with the mock closed. Missed ticks are not
 * checked, because a loaded machine may legitimately miss one.
 */
static void test_real_time_capture_run(void)
{
    capture_config cfg;
    capture_env env;
    capture_sink sink = {rt_sample, rt_heartbeat, NULL};
    capture_outcome out;
    size_t n = 0;
    unsigned k;

    memset(&rt, 0, sizeof rt);
    rt.steps[n++] = rt_step(I2C_MOCK_WRITE, cmd_power_on, 1, 0);
    rt.steps[n++] = rt_step(I2C_MOCK_WRITE, cmd_mtreg_high, 1, 0);
    rt.steps[n++] = rt_step(I2C_MOCK_WRITE, cmd_mtreg_low, 1, 0);
    for (k = 0; k < 4; k++) {
        rt.steps[n++] = rt_step(I2C_MOCK_WRITE, cmd_one_time_h, 1, 0);
        rt.steps[n++] = rt_step(I2C_MOCK_WAIT, NULL, 0, 180000);
        rt.steps[n++] = rt_step(I2C_MOCK_READ, light_raw_12, 2, 0);
    }
    assert(n == COUNT_OF(rt.steps));

    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    cfg.tick_period_ns = 20000000;   /* 20 ms, for the test only */
    capture_linux_env(&env);
    env.open_device = rt_open;
    env.stop_requested = rt_stop;
    capture_linux_clear_stop();

    assert(capture_run(&cfg, &env, &sink, &out) == CAPTURE_EXIT_STOPPED);
    assert(out.close_attempted == 1 && out.close_io.status == I2C_STATUS_OK);
    assert(rt.heartbeats == 4 && rt.samples == rt.heartbeats);
    for (k = 0; k < rt.samples; k++) {
        assert(rt.sample[k].valid == 1 && rt.sample[k].value == 10000);
        assert(rt.sample[k].is_simulated == 1);
        assert(rt.sample[k].wall_errno == 0 && rt.sample[k].mono_errno == 0);
        assert(k == 0 || rt.sample[k].mono_ns > rt.sample[k - 1].mono_ns);
    }
}

/* main: run every test; abort on the first failure. */
int main(void)
{
    test_env_filled();
    test_read_clock_maps_clocks();
    test_read_clock_failure();
    test_sleep_until_absolute();
    test_sleep_until_negative();
    test_clock_trust();
    test_install_handlers_fake();
    test_real_clocks();
    test_real_sleep_until();
    test_real_clock_trust();
    test_real_open_absent_bus();
    test_real_time_capture_run();
    test_real_stop_handlers();   /* last: leaves the handlers installed */

    puts("SIMULATED + OS PLUMBING capture linux seam tests passed (fake "
         "syscalls; real clocks, sleep, adjtimex read-only and signals at "
         "this process; scripted mock device; no /dev/i2c bus opened)");
    return 0;
}
