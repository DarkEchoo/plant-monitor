#include "sensors/bh1750.h"
#include "sensors/capture.h"
#include "sensors/i2c_mock.h"
#include "sensors/i2c_transport.h"
#include "sensors/seesaw_soil.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * SIMULATED: every test runs the capture core against the scripted I2C
 * mock, a fake clock that only moves when the mock waits or the core
 * sleeps, a fake stop flag and a recording sink. No test opens
 * /dev/i2c-*, reads a real clock or sleeps, and no value is a measurement.
 * The rates used are the PROPOSED ones, not agreed requirements.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_STEPS 160
#define MAX_CLOCK 400
#define MAX_SLEEPS 128
#define MAX_RECORDS 96
#define SEC INT64_C(1000000000)
#define MS INT64_C(1000000)
#define ANCHOR_NS (5 * SEC)              /* fake boot-relative start */
#define WALL_OFFSET_NS (INT64_C(1700000000) * SEC)

static const uint8_t cmd_power_on[] = {0x01};
static const uint8_t cmd_mtreg_high[] = {0x42};
static const uint8_t cmd_mtreg_low[] = {0x65};
static const uint8_t cmd_one_time_h[] = {0x20};
static const uint8_t light_raw_12[] = {0x00, 0x0c};  /* 12 counts: 10000 mlx */

static const uint8_t cmd_reset[] = {0x00, 0x7f, 0xff};
static const uint8_t cmd_hw_id[] = {0x00, 0x01};
static const uint8_t cmd_touch0[] = {0x0f, 0x10};
static const uint8_t id_samd[] = {0x55};
static const uint8_t id_other[] = {0x54};
static const uint8_t soil_500[] = {0x01, 0xf4};
static const uint8_t soil_5000[] = {0x13, 0x88};  /* above 4095: rejected */

/* The fake world behind every seam, for one sensor. */
typedef struct {
    i2c_mock_step steps[MAX_STEPS];
    size_t step_count;
    i2c_mock mock;
    int open_errno;
    int cleanup_errno;
    int close_errno;
    int device_live;              /* clear is_simulated on the mock device */
    unsigned opens;
    unsigned open_calls;          /* including failed opens */
    unsigned closes;
    i2c_status mock_close_status;  /* OK only if the script was consumed */
    unsigned bus_seen;
    uint8_t address_seen;

    int64_t now_ns;               /* fake CLOCK_MONOTONIC */
    unsigned clock_calls;
    unsigned fail_clock_call;     /* 1-based; 0 none */
    int fail_clock_errno;
    unsigned bad_value_call;      /* 1-based call returning a bad value */
    int bad_value_kind;           /* 1: nsec -1, 2: nsec 1e9, 3: sec -1 */
    capture_clock clock_kind[MAX_CLOCK + 1];
    size_t steps_at_clock[MAX_CLOCK + 1];

    unsigned waits;
    unsigned extra_wait_n;        /* 1-based mock wait that takes longer */
    int64_t extra_wait_ns;

    unsigned sleeps;
    int64_t sleep_deadline[MAX_SLEEPS + 1];
    unsigned eintr_sleep;         /* 1-based sleep that returns EINTR */
    int eintr_sets_stop;          /* ... and a stop arrives with it */
    unsigned fail_sleep;
    int fail_sleep_errno;

    capture_clock_trust trust;
    int trust_errno;
    unsigned trust_calls;

    unsigned stop_after_heartbeats;  /* 0: never by count */
    int stop;

    capture_sample samples[MAX_RECORDS];
    unsigned n_samples;
    capture_heartbeat heartbeats[MAX_RECORDS];
    unsigned opens_at_heartbeat[MAX_RECORDS];
    unsigned n_heartbeats;
    char events[2 * MAX_RECORDS + 1];  /* 'H' or 'S', in emit order */
    unsigned n_events;
    unsigned fail_heartbeat_n;    /* 1-based; 0 none */
    unsigned fail_sample_n;
} fake;

static const i2c_device_ops *mock_ops;

/* add: append one scripted step. */
static void add(fake *f, i2c_mock_step step)
{
    assert(f->step_count < MAX_STEPS);
    f->steps[f->step_count++] = step;
}

/* write_step: a scripted write of len bytes that succeeds. */
static i2c_mock_step write_step(const uint8_t *bytes, size_t len)
{
    i2c_mock_step step = {I2C_MOCK_WRITE, bytes, len, len, 0, 0};
    return step;
}

/* wait_step: a scripted wait of at least min_wait_us. */
static i2c_mock_step wait_step(uint32_t min_wait_us)
{
    i2c_mock_step step = {I2C_MOCK_WAIT, NULL, 0, 0, 0, min_wait_us};
    return step;
}

/* read_step: a scripted read of len bytes returning bytes. */
static i2c_mock_step read_step(const uint8_t *bytes, size_t len)
{
    i2c_mock_step step = {I2C_MOCK_READ, bytes, len, len, 0, 0};
    return step;
}

/* failing: the same step, failing with err. */
static i2c_mock_step failing(i2c_mock_step step, int err)
{
    step.fail_errno = err;
    return step;
}

/* script_light_init: Power On, MTreg high, MTreg low. */
static void script_light_init(fake *f)
{
    add(f, write_step(cmd_power_on, 1));
    add(f, write_step(cmd_mtreg_high, 1));
    add(f, write_step(cmd_mtreg_low, 1));
}

/* script_light_init_fails_power_on: the first init step fails, alone. */
static void script_light_init_fails_power_on(fake *f)
{
    add(f, failing(write_step(cmd_power_on, 1), EREMOTEIO));
}

/* script_light_measure: One Time H-Resolution, 180 ms, two bytes. */
static void script_light_measure(fake *f)
{
    add(f, write_step(cmd_one_time_h, 1));
    add(f, wait_step(180000));
    add(f, read_step(light_raw_12, 2));
}

/* script_light_measure_read_fails: the measurement's read fails with err. */
static void script_light_measure_read_fails(fake *f, int err)
{
    add(f, write_step(cmd_one_time_h, 1));
    add(f, wait_step(180000));
    add(f, failing(read_step(light_raw_12, 2), err));
}

/* script_soil_init: reset, 500 ms, select HW_ID, 8 ms, read the ID. */
static void script_soil_init(fake *f, const uint8_t *id)
{
    add(f, write_step(cmd_reset, sizeof cmd_reset));
    add(f, wait_step(500000));
    add(f, write_step(cmd_hw_id, sizeof cmd_hw_id));
    add(f, wait_step(8000));
    add(f, read_step(id, 1));
}

/* script_soil_measure: select touch channel 0, 5 ms, read two bytes. */
static void script_soil_measure(fake *f, const uint8_t *bytes)
{
    add(f, write_step(cmd_touch0, sizeof cmd_touch0));
    add(f, wait_step(5000));
    add(f, read_step(bytes, 2));
}

/* wrap_write: forward to the mock. */
static i2c_result wrap_write(void *ctx, const uint8_t *buf, size_t len)
{
    fake *f = ctx;
    return mock_ops->write(&f->mock, buf, len);
}

/* wrap_read: forward to the mock. */
static i2c_result wrap_read(void *ctx, uint8_t *buf, size_t len)
{
    fake *f = ctx;
    return mock_ops->read(&f->mock, buf, len);
}

/*
 * wrap_wait_us: forward to the mock, then advance the fake clock by the
 * wait, plus extra_wait_ns on the chosen wait, to simulate time passing.
 */
static i2c_result wrap_wait_us(void *ctx, uint32_t duration_us)
{
    fake *f = ctx;
    i2c_result r = mock_ops->wait_us(&f->mock, duration_us);

    f->waits++;
    f->now_ns += (int64_t)duration_us * 1000;
    if (f->waits == f->extra_wait_n) {
        f->now_ns += f->extra_wait_ns;
    }
    return r;
}

/* wrap_close: close the mock, record it, apply any injected failure. */
static i2c_result wrap_close(void *ctx)
{
    fake *f = ctx;
    i2c_result r = mock_ops->close(&f->mock);

    f->closes++;
    f->mock_close_status = r.status;
    if (f->close_errno != 0) {
        r.status = I2C_STATUS_OS_ERROR;
        r.os_errno = f->close_errno;
    }
    return r;
}

static const i2c_device_ops wrap_ops = {
    wrap_write, wrap_read, wrap_wait_us, wrap_close
};

/* fake_open: open the mock, following i2c_linux_open's contract. */
static i2c_result fake_open(void *ctx, i2c_device *dev, unsigned bus_number,
                            uint8_t address_7bit, int *cleanup_errno)
{
    fake *f = ctx;
    i2c_result r = {I2C_STATUS_OK, 0, 0, 0};

    assert(dev != NULL && dev->ops == NULL && cleanup_errno != NULL);
    f->open_calls++;
    f->bus_seen = bus_number;
    f->address_seen = address_7bit;
    *cleanup_errno = 0;
    if (f->open_errno != 0) {
        r.status = I2C_STATUS_OS_ERROR;
        r.os_errno = f->open_errno;
        *cleanup_errno = f->cleanup_errno;
        return r;
    }
    assert(i2c_mock_open(&f->mock, dev, address_7bit, f->steps,
                         f->step_count) == I2C_STATUS_OK);
    mock_ops = dev->ops;
    dev->ops = &wrap_ops;
    dev->ctx = f;
    if (f->device_live) {
        dev->is_simulated = 0;
    }
    f->opens++;
    return r;
}

/*
 * fake_read_clock: wall = WALL_OFFSET + monotonic. Records which clock was
 * read and how many script steps had run, so tests can check order.
 */
static int fake_read_clock(void *ctx, capture_clock clock,
                           struct timespec *out)
{
    fake *f = ctx;
    unsigned n = ++f->clock_calls;
    int64_t ns = f->now_ns;

    assert(n <= MAX_CLOCK);
    f->clock_kind[n] = clock;
    f->steps_at_clock[n] = f->mock.next_step;
    if (n == f->fail_clock_call) {
        return f->fail_clock_errno;
    }
    if (clock == CAPTURE_CLOCK_WALL && ns <= INT64_MAX - WALL_OFFSET_NS) {
        ns += WALL_OFFSET_NS;
    }
    out->tv_sec = (time_t)(ns / SEC);
    out->tv_nsec = (long)(ns % SEC);
    if (n == f->bad_value_call) {
        if (f->bad_value_kind == 1) {
            out->tv_nsec = -1;
        } else if (f->bad_value_kind == 2) {
            out->tv_nsec = 1000000000L;
        } else {
            out->tv_sec = -1;
        }
    }
    return 0;
}

/* fake_sleep_until: jump the fake clock to the deadline, or fail. */
static int fake_sleep_until(void *ctx, int64_t deadline_mono_ns)
{
    fake *f = ctx;
    unsigned n = ++f->sleeps;

    assert(n <= MAX_SLEEPS);
    f->sleep_deadline[n] = deadline_mono_ns;
    if (n == f->eintr_sleep) {
        if (f->eintr_sets_stop) {
            f->stop = 1;
        }
        return EINTR;
    }
    if (n == f->fail_sleep) {
        return f->fail_sleep_errno;
    }
    if (deadline_mono_ns > f->now_ns) {
        f->now_ns = deadline_mono_ns;
    }
    return 0;
}

/*
 * fake_trust: the configured trust answer. It always writes *err, as a
 * careless seam might, so the core must report it only for UNKNOWN.
 */
static capture_clock_trust fake_trust(void *ctx, int *err)
{
    fake *f = ctx;

    f->trust_calls++;
    *err = f->trust_errno;
    return f->trust;
}

/*
 * fake_stop: stop once f->stop is set (a stop that may already be pending
 * at startup), or, to run for N ticks, after N heartbeats once the run is
 * under way (the open has been tried).
 */
static int fake_stop(void *ctx)
{
    fake *f = ctx;

    return f->stop ||
           (f->stop_after_heartbeats != 0 && f->open_calls > 0 &&
            f->n_heartbeats >= f->stop_after_heartbeats);
}

/* record_sample: keep a copy, or fail when told to. */
static int record_sample(void *ctx, const capture_sample *s)
{
    fake *f = ctx;

    assert(f->n_samples < MAX_RECORDS);
    if (f->n_samples + 1 == f->fail_sample_n) {
        return EPIPE;
    }
    f->samples[f->n_samples++] = *s;
    f->events[f->n_events++] = 'S';
    f->events[f->n_events] = '\0';
    return 0;
}

/* record_heartbeat: keep a copy and the open count, or fail. */
static int record_heartbeat(void *ctx, const capture_heartbeat *h)
{
    fake *f = ctx;

    assert(f->n_heartbeats < MAX_RECORDS);
    if (f->n_heartbeats + 1 == f->fail_heartbeat_n) {
        return EPIPE;
    }
    f->opens_at_heartbeat[f->n_heartbeats] = f->opens;
    f->heartbeats[f->n_heartbeats++] = *h;
    f->events[f->n_events++] = 'H';
    f->events[f->n_events] = '\0';
    return 0;
}

/* fake_init: a fresh world at the fake start time; trust untrusted. */
static void fake_init(fake *f, unsigned stop_after_heartbeats)
{
    memset(f, 0, sizeof *f);
    f->now_ns = ANCHOR_NS;
    f->trust = CAPTURE_CLOCK_UNTRUSTED;
    f->stop_after_heartbeats = stop_after_heartbeats;
}

/* light_config / soil_config: the proposed values on bus 1. */
static capture_config light_config(void)
{
    capture_config cfg;

    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    return cfg;
}

static capture_config soil_config(void)
{
    capture_config cfg;

    capture_config_proposed(&cfg, CAPTURE_SENSOR_SOIL, 1, 0x36);
    return cfg;
}

/* run: run the core in the fake world. */
static capture_exit run(fake *f, const capture_config *cfg,
                        int env_simulated, capture_outcome *out)
{
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 0};
    capture_sink sink = {record_sample, record_heartbeat, NULL};
    capture_exit e;

    env.ctx = f;
    env.is_simulated = env_simulated;
    sink.ctx = f;
    e = capture_run(cfg, &env, &sink, out);
    assert(out == NULL || out->exit == e);
    return e;
}

/*
 * assert_closed_once: the device, if opened, was closed exactly once and
 * its script consumed exactly, so nothing ran after a failure and there
 * were no hidden retries.
 */
static void assert_closed_once(const fake *f)
{
    assert(f->closes == f->opens);
    if (f->opens > 0) {
        assert(f->mock_close_status == I2C_STATUS_OK);
    }
}

/* tick_ns: the deadline of tick k. */
static int64_t tick_ns(uint64_t k)
{
    return ANCHOR_NS + (int64_t)k * CAPTURE_PROPOSED_TICK_PERIOD_NS;
}

/* test_config_proposed: the proposed values, per sensor. */
static void test_config_proposed(void)
{
    capture_config l = light_config();
    capture_config s = soil_config();

    assert(l.sensor == CAPTURE_SENSOR_LIGHT && l.bus_number == 1 &&
           l.address_7bit == 0x23);
    assert(l.tick_period_ns == INT64_C(10000000000));
    assert(l.sample_every_ticks == 1 && s.sample_every_ticks == 6);
    assert(l.backoff_cap_periods == 8 && s.backoff_cap_periods == 8);
    assert(l.reinit_after_transport_failure == 1);
    assert(l.reinit_after_rejections == 3);
    assert(l.first_sequence == 1 && s.first_sequence == 1);
    assert(s.sensor == CAPTURE_SENSOR_SOIL && s.address_7bit == 0x36);
}

/*
 * test_unusable_arguments: bad config, env or sink is refused before any
 * open, clock read or emit, and *out still says why.
 */
static void test_unusable_arguments(void)
{
    capture_config bad[10];
    capture_config good = light_config();
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    capture_sink sink = {record_sample, record_heartbeat, NULL};
    capture_env env_missing[5];
    capture_sink sink_missing[2];
    capture_outcome out;
    fake f;
    size_t i;

    fake_init(&f, 1);
    env.ctx = &f;
    sink.ctx = &f;
    for (i = 0; i < COUNT_OF(bad); i++) {
        bad[i] = good;
    }
    bad[0].tick_period_ns = 0;
    bad[1].tick_period_ns = -1;
    bad[2].sample_every_ticks = 0;
    bad[3].backoff_cap_periods = 0;
    bad[4].first_sequence = 0;
    bad[5].address_7bit = 0x36;           /* soil address for light */
    bad[6].sensor = CAPTURE_SENSOR_SOIL;  /* light address for soil */
    bad[7].sensor = (capture_sensor)7;
    bad[8].address_7bit = 0x24;
    bad[9].backoff_cap_periods = CAPTURE_BACKOFF_CAP_MAX + 1;
    for (i = 0; i < COUNT_OF(bad); i++) {
        memset(&out, 0xa5, sizeof out);
        assert(capture_run(&bad[i], &env, &sink, &out) == CAPTURE_EXIT_USAGE);
        assert(out.exit == CAPTURE_EXIT_USAGE && out.close_attempted == 0);
    }
    for (i = 0; i < COUNT_OF(env_missing); i++) {
        env_missing[i] = env;
    }
    env_missing[0].open_device = NULL;
    env_missing[1].read_clock = NULL;
    env_missing[2].sleep_until = NULL;
    env_missing[3].check_clock_trust = NULL;
    env_missing[4].stop_requested = NULL;
    for (i = 0; i < COUNT_OF(env_missing); i++) {
        assert(capture_run(&good, &env_missing[i], &sink, &out) ==
               CAPTURE_EXIT_USAGE);
    }
    sink_missing[0] = sink;
    sink_missing[0].emit_sample = NULL;
    sink_missing[1] = sink;
    sink_missing[1].emit_heartbeat = NULL;
    for (i = 0; i < COUNT_OF(sink_missing); i++) {
        assert(capture_run(&good, &env, &sink_missing[i], &out) ==
               CAPTURE_EXIT_USAGE);
    }
    assert(capture_run(NULL, &env, &sink, &out) == CAPTURE_EXIT_USAGE);
    assert(capture_run(&good, NULL, &sink, &out) == CAPTURE_EXIT_USAGE);
    assert(capture_run(&good, &env, NULL, &out) == CAPTURE_EXIT_USAGE);
    assert(capture_run(&bad[0], &env, &sink, NULL) == CAPTURE_EXIT_USAGE);
    assert(f.opens == 0 && f.clock_calls == 0 && f.n_events == 0);

    good.backoff_cap_periods = CAPTURE_BACKOFF_CAP_MAX;  /* the largest */
    f.stop = 1;
    assert(capture_run(&good, &env, &sink, &out) == CAPTURE_EXIT_STOPPED);
}

/*
 * test_startup_heartbeat: the first heartbeat comes before the device is
 * opened, with the defined startup fields, and is labelled from the
 * environment alone; the first attempt follows on the same tick.
 */
static void test_startup_heartbeat(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    const capture_heartbeat *h;
    fake f;

    fake_init(&f, 1);
    script_light_init(&f);
    script_light_measure(&f);
    assert(run(&f, &cfg, 0, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_heartbeats == 1 && f.opens_at_heartbeat[0] == 0);
    h = &f.heartbeats[0];
    assert(h->sensor == CAPTURE_SENSOR_LIGHT && h->tick == 0);
    assert(h->state == CAPTURE_STATE_STARTING && h->last_sequence == 0);
    assert(h->consecutive_failures == 0 && h->attempts == 0 &&
           h->failures == 0 && h->missed_ticks == 0);
    assert(h->next_attempt_tick == 0 && h->sensor_initialized == 0);
    assert(h->is_simulated == 0);              /* env says live; not open */
    assert(h->scheduled_mono_ns == ANCHOR_NS);
    assert(strcmp(f.events, "HS") == 0);
    assert(f.samples[0].is_simulated == 1);    /* the mock device */
    assert(f.bus_seen == 1 && f.address_seen == 0x23);
    assert(out.close_attempted == 1 &&
           out.close_io.status == I2C_STATUS_OK);
    assert_closed_once(&f);
}

/*
 * test_stop_before_open: a stop already pending at startup ends the run
 * after the startup heartbeat, without opening the device.
 */
static void test_stop_before_open(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 0);
    f.stop = 1;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(strcmp(f.events, "H") == 0 && f.opens == 0);
    assert(out.close_attempted == 0);
}

/*
 * test_light_schedule_no_drift: heartbeat then sample on every tick, on
 * absolute deadlines that do not drift although each reading takes
 * 180 ms; sequences 1..5; both timestamps after the attempt.
 */
static void test_light_schedule_no_drift(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;
    unsigned k;

    fake_init(&f, 5);
    script_light_init(&f);
    for (k = 0; k < 5; k++) {
        script_light_measure(&f);
    }
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(strcmp(f.events, "HSHSHSHSHS") == 0);
    assert(f.sleeps == 4);
    for (k = 1; k <= 4; k++) {
        assert(f.sleep_deadline[k] == tick_ns(k));
    }
    for (k = 0; k < 5; k++) {
        const capture_sample *s = &f.samples[k];

        assert(s->sequence == k + 1 && s->tick == k);
        assert(s->valid == 1 && s->phase == CAPTURE_PHASE_MEASURE);
        assert(s->included_init == (k == 0));
        assert(s->unit == CAPTURE_UNIT_MILLILUX && s->value == 10000);
        assert(s->has_raw_count == 1 && s->raw_count == 12);
        assert(s->has_rejected_value == 0);
        assert(s->stage == BH1750_STAGE_NONE &&
               strcmp(s->stage_name, "NONE") == 0);
        assert(s->io.status == I2C_STATUS_OK);
        assert(s->mono_ns == tick_ns(k) + 180 * MS && s->mono_errno == 0);
        assert(s->wall_ns == WALL_OFFSET_NS + s->mono_ns &&
               s->wall_errno == 0);
        assert(s->clock_trust == CAPTURE_CLOCK_UNTRUSTED);
        assert(s->is_simulated == 1 && s->reinit_scheduled == 0);
        assert(f.heartbeats[k].tick == k &&
               f.heartbeats[k].scheduled_mono_ns == tick_ns(k));
        assert(f.heartbeats[k].last_sequence == k);
    }
    assert(f.heartbeats[1].state == CAPTURE_STATE_OK);
    assert(f.heartbeats[4].attempts == 4 && f.heartbeats[4].failures == 0);
    assert(f.trust_calls == 5);
    assert_closed_once(&f);
}

/*
 * test_clocks_follow_attempt: per attempt the clocks are read wall, then
 * monotonic, only after the attempt's last step ran; then one scheduling
 * read. The anchor read is first, before anything else.
 */
static void test_clocks_follow_attempt(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 2);
    script_light_init(&f);
    script_light_measure(&f);
    script_light_measure_read_fails(&f, EREMOTEIO);
    /* the reinit it schedules would run on tick 2, after the stop */
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.clock_kind[1] == CAPTURE_CLOCK_MONOTONIC &&
           f.steps_at_clock[1] == 0);
    /* tick 0: init (3) + measure (3) ran before both sample clocks */
    assert(f.clock_kind[2] == CAPTURE_CLOCK_WALL && f.steps_at_clock[2] == 6);
    assert(f.clock_kind[3] == CAPTURE_CLOCK_MONOTONIC &&
           f.steps_at_clock[3] == 6);
    assert(f.clock_kind[4] == CAPTURE_CLOCK_MONOTONIC);
    /* tick 1: the failed read is the 9th step; clocks come after it */
    assert(f.clock_kind[5] == CAPTURE_CLOCK_WALL && f.steps_at_clock[5] == 9);
    assert(f.clock_kind[6] == CAPTURE_CLOCK_MONOTONIC &&
           f.steps_at_clock[6] == 9);
    assert(f.samples[1].valid == 0 &&
           f.samples[1].mono_ns == tick_ns(1) + 180 * MS);
    assert_closed_once(&f);
}

/*
 * test_soil_every_sixth_tick: soil samples on ticks 0, 6 and 12, with a
 * heartbeat on every tick between.
 */
static void test_soil_every_sixth_tick(void)
{
    capture_config cfg = soil_config();
    capture_outcome out;
    fake f;
    unsigned k;

    fake_init(&f, 13);
    script_soil_init(&f, id_samd);
    for (k = 0; k < 3; k++) {
        script_soil_measure(&f, soil_500);
    }
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_heartbeats == 13 && f.n_samples == 3);
    assert(strcmp(f.events, "HSHHHHHHSHHHHHHS") == 0);
    for (k = 0; k < 3; k++) {
        assert(f.samples[k].tick == 6 * k && f.samples[k].sequence == k + 1);
        assert(f.samples[k].valid && f.samples[k].value == 500 &&
               f.samples[k].unit == CAPTURE_UNIT_RAW_COUNTS &&
               f.samples[k].has_raw_count == 0);
    }
    /* soil init waits 508 ms, then the 5 ms measurement */
    assert(f.samples[0].mono_ns == tick_ns(0) + 513 * MS);
    assert(f.samples[1].mono_ns == tick_ns(6) + 5 * MS);
    assert(f.heartbeats[3].next_attempt_tick == 6);
    assert(f.heartbeats[12].sensor_initialized == 1);
    for (k = 1; k <= 12; k++) {
        assert(f.sleep_deadline[k] == tick_ns(k));
    }
    assert_closed_once(&f);
}

/*
 * test_overrun_skips_ticks: an attempt that runs 25 s past its tick
 * skips the two ticks already past, counts them, and does not burst.
 */
static void test_overrun_skips_ticks(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 2);
    script_light_init(&f);
    script_light_measure(&f);
    script_light_measure(&f);
    f.extra_wait_n = 1;
    f.extra_wait_ns = 25 * SEC;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(strcmp(f.events, "HSHS") == 0);
    assert(f.sleeps == 1 && f.sleep_deadline[1] == tick_ns(3));
    assert(f.heartbeats[1].tick == 3 && f.heartbeats[1].missed_ticks == 2);
    assert(f.heartbeats[1].next_attempt_tick == 3);   /* not a stale 1 */
    assert(f.samples[1].tick == 3 && f.samples[1].sequence == 2);
    assert_closed_once(&f);
}

/*
 * test_overrun_exact_deadline: finishing exactly on the next tick's
 * deadline is not a miss; that tick runs on time.
 */
static void test_overrun_exact_deadline(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 2);
    script_light_init(&f);
    script_light_measure(&f);
    script_light_measure(&f);
    f.extra_wait_n = 1;
    f.extra_wait_ns = 10 * SEC - 180 * MS;   /* ends at tick 1's deadline */
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.heartbeats[1].tick == 1 && f.heartbeats[1].missed_ticks == 0);
    assert(f.samples[1].tick == 1);
    assert_closed_once(&f);
}

/*
 * test_soil_overrun_moves_due_attempt: a 65 s overrun at tick 0 skips
 * ticks 1-6, including tick 6 where the next soil attempt was due. That
 * attempt runs on tick 7, the heartbeat says so, and the 6-tick spacing
 * continues from tick 7.
 */
static void test_soil_overrun_moves_due_attempt(void)
{
    capture_config cfg = soil_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 8);                    /* ticks 0, then 7 to 13 */
    script_soil_init(&f, id_samd);
    script_soil_measure(&f, soil_500);   /* tick 0, overruns */
    script_soil_measure(&f, soil_500);   /* due 6, runs on tick 7 */
    script_soil_measure(&f, soil_500);   /* tick 13 */
    f.extra_wait_n = 3;                  /* the tick-0 measurement's wait */
    f.extra_wait_ns = 65 * SEC;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_samples == 3);
    assert(f.heartbeats[1].tick == 7 && f.heartbeats[1].missed_ticks == 6);
    assert(f.heartbeats[1].next_attempt_tick == 7);
    assert(f.samples[1].tick == 7 && f.samples[2].tick == 13);
    assert_closed_once(&f);
}

/*
 * test_backoff_keeps_heartbeats: a light sensor whose init always fails
 * is attempted on ticks 0, 1, 3, 7, 15, 23, 31, 39 (1, 2, 4, 8, then 8
 * periods), while a heartbeat goes out on every one of the 40 ticks.
 * Sequences count attempts only.
 */
static void test_backoff_keeps_heartbeats(void)
{
    static const uint64_t attempt_ticks[] = {0, 1, 3, 7, 15, 23, 31, 39};
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;
    size_t i;
    unsigned k;

    fake_init(&f, 40);
    for (i = 0; i < COUNT_OF(attempt_ticks); i++) {
        script_light_init_fails_power_on(&f);
    }
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_heartbeats == 40 && f.n_samples == COUNT_OF(attempt_ticks));
    for (k = 1; k < 40; k++) {
        assert(f.heartbeats[k].tick == k);
        assert(f.sleep_deadline[k] == tick_ns(k));
        assert(f.heartbeats[k].state == CAPTURE_STATE_DEGRADED);
        assert(f.heartbeats[k].sensor_initialized == 0);
    }
    for (i = 0; i < COUNT_OF(attempt_ticks); i++) {
        const capture_sample *s = &f.samples[i];

        assert(s->tick == attempt_ticks[i] && s->sequence == i + 1);
        assert(s->phase == CAPTURE_PHASE_INIT && s->valid == 0);
        assert(s->stage == BH1750_STAGE_POWER_ON);
        assert(s->io.status == I2C_STATUS_OS_ERROR &&
               s->io.os_errno == EREMOTEIO);
        assert(s->included_init == 0 && s->has_raw_count == 0);
    }
    assert(f.heartbeats[2].next_attempt_tick == 3 &&
           f.heartbeats[2].consecutive_failures == 2);
    assert(f.heartbeats[8].next_attempt_tick == 15 &&
           f.heartbeats[8].consecutive_failures == 4);
    assert(f.heartbeats[39].last_sequence == 7 &&
           f.heartbeats[39].attempts == 7 && f.heartbeats[39].failures == 7);
    assert_closed_once(&f);
}

/*
 * test_backoff_cap_not_power_of_two: with a cap of 3 periods the spacing
 * is 1, 2, 3, 3: the cap itself, not the next power of two.
 */
static void test_backoff_cap_not_power_of_two(void)
{
    static const uint64_t attempt_ticks[] = {0, 1, 3, 6, 9, 12};
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;
    size_t i;

    cfg.backoff_cap_periods = 3;
    fake_init(&f, 13);
    for (i = 0; i < COUNT_OF(attempt_ticks); i++) {
        script_light_init_fails_power_on(&f);
    }
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_samples == COUNT_OF(attempt_ticks));
    for (i = 0; i < COUNT_OF(attempt_ticks); i++) {
        assert(f.samples[i].tick == attempt_ticks[i]);
    }
    assert_closed_once(&f);
}

/*
 * test_recovery_resets_backoff: two failed inits, then a successful one
 * on tick 3 measures in the same attempt; the next attempt is the next
 * tick and the state is OK again.
 */
static void test_recovery_resets_backoff(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 5);
    script_light_init_fails_power_on(&f);
    script_light_init_fails_power_on(&f);
    script_light_init(&f);
    script_light_measure(&f);
    script_light_measure(&f);
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.n_samples == 4);
    assert(f.samples[2].tick == 3 && f.samples[2].valid &&
           f.samples[2].included_init == 1 && f.samples[2].sequence == 3);
    assert(f.samples[3].tick == 4 && f.samples[3].included_init == 0);
    assert(f.heartbeats[4].state == CAPTURE_STATE_OK &&
           f.heartbeats[4].consecutive_failures == 0 &&
           f.heartbeats[4].failures == 2);
    assert_closed_once(&f);
}

/*
 * test_transport_failure_trigger: with the trigger on, a failed read
 * makes the next attempt initialize first; with it off, the next attempt
 * measures straight away. The exact scripts prove which happened.
 */
static void test_transport_failure_trigger(void)
{
    int enabled;

    for (enabled = 0; enabled <= 1; enabled++) {
        capture_config cfg = light_config();
        capture_outcome out;
        fake f;

        cfg.reinit_after_transport_failure = enabled;
        fake_init(&f, 2);
        script_light_init(&f);
        script_light_measure_read_fails(&f, EREMOTEIO);
        if (enabled) {
            script_light_init(&f);
        }
        script_light_measure(&f);
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        assert(f.samples[0].valid == 0 &&
               f.samples[0].phase == CAPTURE_PHASE_MEASURE);
        assert(f.samples[0].stage == BH1750_STAGE_READ);
        assert(f.samples[0].io.os_errno == EREMOTEIO);
        assert(f.samples[0].reinit_scheduled == enabled);
        assert(f.samples[1].valid == 1 &&
               f.samples[1].included_init == enabled);
        assert(f.heartbeats[1].sensor_initialized == !enabled);
        assert_closed_once(&f);
    }
}

/*
 * test_soil_range_rejection: a RANGE rejection is invalid with an OK
 * transport and the refused counts; it is never a value.
 */
static void test_soil_range_rejection(void)
{
    capture_config cfg = soil_config();
    capture_outcome out;
    const capture_sample *s;
    fake f;

    cfg.sample_every_ticks = 1;
    fake_init(&f, 1);
    script_soil_init(&f, id_samd);
    script_soil_measure(&f, soil_5000);
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    s = &f.samples[0];
    assert(s->valid == 0 && s->phase == CAPTURE_PHASE_MEASURE);
    assert(s->stage == SOIL_STAGE_RANGE &&
           strcmp(s->stage_name, "RANGE") == 0);
    assert(s->io.status == I2C_STATUS_OK);
    assert(s->has_rejected_value == 1 && s->rejected_value == 5000);
    assert(s->unit == CAPTURE_UNIT_RAW_COUNTS && s->reinit_scheduled == 0);
    assert_closed_once(&f);
}

/*
 * test_rejection_trigger: three consecutive RANGE rejections (ticks 0, 1,
 * 3 under backoff) schedule a reinit; with the trigger at 0 they do not;
 * a valid reading in between resets the count.
 */
static void test_rejection_trigger(void)
{
    int variant;

    for (variant = 0; variant < 3; variant++) {
        capture_config cfg = soil_config();
        capture_outcome out;
        fake f;

        cfg.sample_every_ticks = 1;
        if (variant == 1) {
            cfg.reinit_after_rejections = 0;
        }
        fake_init(&f, variant == 2 ? 6 : 8);
        script_soil_init(&f, id_samd);
        if (variant == 2) {
            script_soil_measure(&f, soil_5000);   /* tick 0 */
            script_soil_measure(&f, soil_5000);   /* tick 1 */
            script_soil_measure(&f, soil_500);    /* tick 3 */
            script_soil_measure(&f, soil_5000);   /* tick 4 */
            script_soil_measure(&f, soil_5000);   /* tick 5 */
        } else {
            script_soil_measure(&f, soil_5000);   /* tick 0 */
            script_soil_measure(&f, soil_5000);   /* tick 1 */
            script_soil_measure(&f, soil_5000);   /* tick 3 */
            if (variant == 0) {
                script_soil_init(&f, id_samd);
            }
            script_soil_measure(&f, soil_500);    /* tick 7 */
        }
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        if (variant == 2) {
            unsigned i;

            assert(f.n_samples == 5 && f.samples[2].valid);
            for (i = 0; i < 5; i++) {
                assert(f.samples[i].reinit_scheduled == 0);
            }
        } else {
            assert(f.n_samples == 4);
            assert(f.samples[2].tick == 3);
            assert(f.samples[0].reinit_scheduled == 0 &&
                   f.samples[1].reinit_scheduled == 0);
            assert(f.samples[2].reinit_scheduled == (variant == 0));
            assert(f.samples[3].tick == 7 && f.samples[3].valid &&
                   f.samples[3].included_init == (variant == 0));
        }
        assert_closed_once(&f);
    }
}

/*
 * test_rejection_count_resets: the rejection count restarts after the
 * trigger fires and after a transport failure, so rejections after a
 * reinit, or split by a transport failure, do not reinitialize early.
 * (The reset after a failed init is unreachable on its own: a failed
 * init only follows a reinit, and both triggers already reset the count.)
 */
static void test_rejection_count_resets(void)
{
    int variant;

    for (variant = 0; variant < 2; variant++) {
        capture_config cfg = soil_config();
        capture_outcome out;
        fake f;
        unsigned i;

        cfg.sample_every_ticks = 1;
        cfg.backoff_cap_periods = 1;    /* one attempt every tick */
        fake_init(&f, 5);
        script_soil_init(&f, id_samd);
        script_soil_measure(&f, soil_5000);          /* tick 0 */
        script_soil_measure(&f, soil_5000);          /* tick 1 */
        if (variant == 0) {
            script_soil_measure(&f, soil_5000);      /* tick 2: 3rd, fires */
        } else {
            add(&f, write_step(cmd_touch0, sizeof cmd_touch0));
            add(&f, wait_step(5000));
            add(&f, failing(read_step(soil_500, 2), EREMOTEIO)); /* tick 2 */
        }
        script_soil_init(&f, id_samd);               /* tick 3 */
        script_soil_measure(&f, soil_5000);
        script_soil_measure(&f, soil_5000);          /* tick 4 */
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        assert(f.n_samples == 5);
        for (i = 0; i < 5; i++) {
            /* only tick 2 fires: rejections (0) or transport (1) */
            assert(f.samples[i].reinit_scheduled == (i == 2));
        }
        assert(f.samples[3].included_init == 1 &&
               f.samples[4].included_init == 0);
        assert_closed_once(&f);
    }
}

/*
 * test_soil_hw_id_rejected: a wrong hardware ID is a failed init with an
 * OK transport and the ID as the rejected value; the next attempt
 * initializes again.
 */
static void test_soil_hw_id_rejected(void)
{
    capture_config cfg = soil_config();
    capture_outcome out;
    fake f;

    cfg.sample_every_ticks = 1;
    fake_init(&f, 2);
    script_soil_init(&f, id_other);
    script_soil_init(&f, id_samd);
    script_soil_measure(&f, soil_500);
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.samples[0].phase == CAPTURE_PHASE_INIT &&
           f.samples[0].valid == 0);
    assert(f.samples[0].stage == SOIL_STAGE_HW_ID &&
           f.samples[0].io.status == I2C_STATUS_OK);
    assert(f.samples[0].has_rejected_value &&
           f.samples[0].rejected_value == 0x54);
    assert(f.samples[0].mono_ns == tick_ns(0) + 508 * MS);
    assert(f.samples[1].valid && f.samples[1].included_init == 1);
    assert_closed_once(&f);
}

/*
 * test_sample_clock_failures: a failed wall or monotonic sample read marks
 * only that timestamp unavailable; validity is the driver's alone, and
 * the run goes on.
 */
static void test_sample_clock_failures(void)
{
    unsigned which;

    for (which = 2; which <= 3; which++) {
        capture_config cfg = light_config();
        capture_outcome out;
        fake f;

        fake_init(&f, 2);
        script_light_init(&f);
        script_light_measure(&f);
        script_light_measure(&f);
        f.fail_clock_call = which;
        f.fail_clock_errno = EINVAL;
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        assert(f.samples[0].valid == 1);
        if (which == 2) {
            assert(f.samples[0].wall_errno == EINVAL &&
                   f.samples[0].wall_ns == 0);
            assert(f.samples[0].mono_errno == 0);
        } else {
            assert(f.samples[0].mono_errno == EINVAL &&
                   f.samples[0].mono_ns == 0);
            assert(f.samples[0].wall_errno == 0);
        }
        assert(f.samples[1].wall_errno == 0 && f.samples[1].mono_errno == 0);
        assert_closed_once(&f);
    }
}

/*
 * test_bad_clock_values: a sample clock returning tv_nsec -1 or 1e9 is
 * EINVAL, a negative tv_sec is ERANGE, and only that timestamp is
 * unavailable; the same values on the scheduling read are fatal.
 */
static void test_bad_clock_values(void)
{
    static const struct {
        int kind;
        int err;
    } kinds[] = {{1, EINVAL}, {2, EINVAL}, {3, ERANGE}};
    size_t i;

    for (i = 0; i < COUNT_OF(kinds); i++) {
        capture_config cfg = light_config();
        capture_outcome out;
        fake f;

        fake_init(&f, 1);
        script_light_init(&f);
        script_light_measure(&f);
        f.bad_value_call = 2;              /* the sample's wall read */
        f.bad_value_kind = kinds[i].kind;
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        assert(f.samples[0].wall_errno == kinds[i].err &&
               f.samples[0].wall_ns == 0 && f.samples[0].mono_errno == 0);
        assert_closed_once(&f);

        fake_init(&f, 3);
        script_light_init(&f);
        script_light_measure(&f);
        f.bad_value_call = 4;              /* the scheduling read */
        f.bad_value_kind = kinds[i].kind;
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_CLOCK_FAILED);
        assert(out.exit_errno == kinds[i].err);
        assert_closed_once(&f);
    }
}

/*
 * test_deadline_overflow: with a monotonic clock so late that the next
 * tick's deadline would overflow int64_t, the run stops with
 * CLOCK_FAILED / ERANGE instead of wrapping. Needs a 64-bit time_t to
 * represent that clock value.
 */
static void test_deadline_overflow(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    if (sizeof(time_t) < 8) {
        return;
    }
    fake_init(&f, 3);
    f.now_ns = INT64_MAX - 5 * SEC;
    script_light_init(&f);
    script_light_measure(&f);
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_CLOCK_FAILED);
    assert(out.exit_errno == ERANGE && strcmp(f.events, "HS") == 0);
    assert_closed_once(&f);
}

/* test_clock_trust_reported: trusted, untrusted and unknown with errno. */
static void test_clock_trust_reported(void)
{
    static const capture_clock_trust cases[] = {
        CAPTURE_CLOCK_TRUSTED, CAPTURE_CLOCK_UNTRUSTED,
        CAPTURE_CLOCK_TRUST_UNKNOWN
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        capture_config cfg = light_config();
        capture_outcome out;
        fake f;

        fake_init(&f, 1);
        script_light_init(&f);
        script_light_measure(&f);
        f.trust = cases[i];
        f.trust_errno = EPERM;
        assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
        assert(f.samples[0].clock_trust == cases[i]);
        assert(f.samples[0].clock_trust_errno ==
               (cases[i] == CAPTURE_CLOCK_TRUST_UNKNOWN ? EPERM : 0));
        assert_closed_once(&f);
    }
}

/*
 * test_fatal_clock_failures: the anchor read failing stops before any
 * heartbeat or open; the scheduling read failing after an attempt stops
 * with the device closed.
 */
static void test_fatal_clock_failures(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 3);
    f.fail_clock_call = 1;
    f.fail_clock_errno = EINVAL;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_CLOCK_FAILED);
    assert(out.exit_errno == EINVAL && out.close_attempted == 0);
    assert(f.n_events == 0 && f.opens == 0);

    fake_init(&f, 3);
    script_light_init(&f);
    script_light_measure(&f);
    f.fail_clock_call = 4;
    f.fail_clock_errno = EOVERFLOW;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_CLOCK_FAILED);
    assert(out.exit_errno == EOVERFLOW && out.close_attempted == 1);
    assert(strcmp(f.events, "HS") == 0);
    assert_closed_once(&f);
}

/*
 * test_simulated_labels: before the open the label comes from the
 * environment; afterwards from the environment or the device.
 */
static void test_simulated_labels(void)
{
    int env_sim;
    int live;

    for (env_sim = 0; env_sim <= 1; env_sim++) {
        for (live = 0; live <= 1; live++) {
            capture_config cfg = light_config();
            capture_outcome out;
            fake f;
            int expect = env_sim || !live;

            fake_init(&f, 2);
            script_light_init(&f);
            script_light_measure(&f);
            script_light_measure(&f);
            f.device_live = live;
            assert(run(&f, &cfg, env_sim, &out) == CAPTURE_EXIT_STOPPED);
            assert(f.heartbeats[0].is_simulated == env_sim);
            assert(f.heartbeats[1].is_simulated == expect);
            assert(f.samples[0].is_simulated == expect &&
                   f.samples[1].is_simulated == expect);
            assert_closed_once(&f);
        }
    }
}

/*
 * test_open_failure: a failed initial open returns OPEN_FAILED with both
 * errnos, after the startup heartbeat only; nothing is left open.
 */
static void test_open_failure(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 3);
    f.open_errno = ENOENT;
    f.cleanup_errno = EIO;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_OPEN_FAILED);
    assert(out.open_io.status == I2C_STATUS_OS_ERROR &&
           out.open_io.os_errno == ENOENT);
    assert(out.open_cleanup_errno == EIO && out.exit_errno == ENOENT);
    assert(out.close_attempted == 0);
    assert(strcmp(f.events, "H") == 0 && f.opens == 0 && f.closes == 0);
}

/*
 * test_stop_during_sleep: an EINTR without a stop sleeps again to the
 * same deadline; an EINTR that brings a stop ends cleanly.
 */
static void test_stop_during_sleep(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 2);
    script_light_init(&f);
    script_light_measure(&f);
    script_light_measure(&f);
    f.eintr_sleep = 1;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.sleeps == 2 && f.sleep_deadline[1] == tick_ns(1) &&
           f.sleep_deadline[2] == tick_ns(1));
    assert(strcmp(f.events, "HSHS") == 0);
    assert_closed_once(&f);

    fake_init(&f, 0);
    script_light_init(&f);
    script_light_measure(&f);
    f.eintr_sleep = 1;
    f.eintr_sets_stop = 1;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_STOPPED);
    assert(f.sleeps == 1 && strcmp(f.events, "HS") == 0);
    assert(out.close_attempted == 1 && out.exit_errno == 0);
    assert_closed_once(&f);
}

/* test_sleep_failure: any other sleep error is fatal; the device closes. */
static void test_sleep_failure(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 0);
    script_light_init(&f);
    script_light_measure(&f);
    f.fail_sleep = 1;
    f.fail_sleep_errno = EINVAL;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SLEEP_FAILED);
    assert(out.exit_errno == EINVAL && out.close_attempted == 1);
    assert_closed_once(&f);
}

/*
 * test_sink_failures: a failed startup heartbeat stops before the open; a
 * failed later heartbeat or sample stops with the device closed.
 */
static void test_sink_failures(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 3);
    f.fail_heartbeat_n = 1;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SINK_FAILED);
    assert(out.exit_errno == EPIPE && f.opens == 0 &&
           out.close_attempted == 0);

    fake_init(&f, 3);
    script_light_init(&f);
    script_light_measure(&f);
    f.fail_heartbeat_n = 2;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SINK_FAILED);
    assert(strcmp(f.events, "HS") == 0 && out.close_attempted == 1);
    assert_closed_once(&f);

    fake_init(&f, 3);
    script_light_init(&f);
    script_light_measure(&f);
    f.fail_sample_n = 1;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SINK_FAILED);
    assert(strcmp(f.events, "H") == 0 && out.exit_errno == EPIPE);
    assert_closed_once(&f);
}

/*
 * test_close_failure_precedence: a failed close turns a clean stop into
 * CLOSE_FAILED, but never replaces an earlier failure; the close result
 * is kept either way.
 */
static void test_close_failure_precedence(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    fake_init(&f, 1);
    script_light_init(&f);
    script_light_measure(&f);
    f.close_errno = EIO;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_CLOSE_FAILED);
    assert(out.exit_errno == EIO && out.close_attempted == 1);
    assert(out.close_io.status == I2C_STATUS_OS_ERROR &&
           out.close_io.os_errno == EIO);

    fake_init(&f, 3);
    script_light_init(&f);
    script_light_measure(&f);
    f.fail_heartbeat_n = 2;
    f.close_errno = EIO;
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SINK_FAILED);
    assert(out.exit_errno == EPIPE);
    assert(out.close_attempted == 1 && out.close_io.os_errno == EIO);
    assert(f.closes == 1);
}

/*
 * test_sequence_exhausted: starting at UINT32_MAX, the first attempt uses
 * it and the next one stops the run instead of wrapping to 0.
 */
static void test_sequence_exhausted(void)
{
    capture_config cfg = light_config();
    capture_outcome out;
    fake f;

    cfg.first_sequence = UINT32_MAX;
    fake_init(&f, 5);
    script_light_init(&f);
    script_light_measure(&f);
    assert(run(&f, &cfg, 1, &out) == CAPTURE_EXIT_SEQUENCE_EXHAUSTED);
    assert(f.n_samples == 1 && f.samples[0].sequence == UINT32_MAX);
    assert(strcmp(f.events, "HSH") == 0);
    assert(f.heartbeats[1].last_sequence == UINT32_MAX);
    assert(out.close_attempted == 1);
    assert_closed_once(&f);
}

/* test_names: stable text for states and exits. */
static void test_names(void)
{
    assert(strcmp(capture_state_name(CAPTURE_STATE_STARTING),
                  "STARTING") == 0);
    assert(strcmp(capture_state_name(CAPTURE_STATE_OK), "OK") == 0);
    assert(strcmp(capture_state_name(CAPTURE_STATE_DEGRADED),
                  "DEGRADED") == 0);
    assert(strcmp(capture_state_name((capture_state)9), "UNKNOWN") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_STOPPED), "STOPPED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_USAGE), "USAGE") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_OPEN_FAILED),
                  "OPEN_FAILED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_CLOCK_FAILED),
                  "CLOCK_FAILED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_SLEEP_FAILED),
                  "SLEEP_FAILED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_SINK_FAILED),
                  "SINK_FAILED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_SEQUENCE_EXHAUSTED),
                  "SEQUENCE_EXHAUSTED") == 0);
    assert(strcmp(capture_exit_name(CAPTURE_EXIT_CLOSE_FAILED),
                  "CLOSE_FAILED") == 0);
    assert(strcmp(capture_exit_name((capture_exit)99), "UNKNOWN") == 0);
}

/* main: run every SIMULATED capture-core test; abort on the first failure. */
int main(void)
{
    test_config_proposed();
    test_unusable_arguments();
    test_startup_heartbeat();
    test_stop_before_open();
    test_light_schedule_no_drift();
    test_clocks_follow_attempt();
    test_soil_every_sixth_tick();
    test_overrun_skips_ticks();
    test_overrun_exact_deadline();
    test_soil_overrun_moves_due_attempt();
    test_backoff_keeps_heartbeats();
    test_backoff_cap_not_power_of_two();
    test_recovery_resets_backoff();
    test_transport_failure_trigger();
    test_soil_range_rejection();
    test_rejection_trigger();
    test_rejection_count_resets();
    test_soil_hw_id_rejected();
    test_sample_clock_failures();
    test_bad_clock_values();
    test_deadline_overflow();
    test_clock_trust_reported();
    test_fatal_clock_failures();
    test_simulated_labels();
    test_open_failure();
    test_stop_during_sleep();
    test_sleep_failure();
    test_sink_failures();
    test_close_failure_precedence();
    test_sequence_exhausted();
    test_names();

    puts("SIMULATED capture core tests passed (scripted mock, fake clock "
         "and sink; no sensor hardware or /dev/i2c device used; proposed "
         "rates only)");
    return 0;
}
