#include "sensors/capture.h"

#include "sensors/bh1750.h"
#include "sensors/seesaw_soil.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define NSEC_PER_SEC INT64_C(1000000000)

/* What one driver call reported, in a form shared by both sensors. */
typedef struct {
    i2c_result io;
    int stage;
    const char *stage_name;
    int ok;                   /* the driver reported overall success */
    int has_rejected_value;
    uint16_t rejected_value;
} driver_outcome;

/* One successful reading, in a form shared by both sensors. */
typedef struct {
    uint32_t value;
    int has_raw_count;
    uint16_t raw_count;
} driver_value;

/* How the core drives one kind of sensor. */
typedef struct {
    capture_unit unit;
    driver_outcome (*init)(i2c_device *dev);
    driver_outcome (*measure)(i2c_device *dev, driver_value *out);
} sensor_ops;

/* State of one run. */
typedef struct {
    const capture_config *cfg;
    const capture_env *env;
    const capture_sink *sink;
    const sensor_ops *ops;
    i2c_device dev;
    int opened;
    int initialized;
    int simulated;                 /* environment or device */
    int64_t anchor_ns;             /* CLOCK_MONOTONIC at the start */
    uint64_t tick;
    int64_t tick_deadline_ns;
    uint64_t next_attempt_tick;
    uint64_t next_sequence;        /* wider than u32 to detect exhaustion */
    uint32_t last_sequence;
    uint32_t consecutive_failures;
    uint32_t consecutive_rejections;
    uint64_t attempts;
    uint64_t failures;
    uint64_t missed_ticks;
} run_state;

/*
 * from_bh1750: success needs stage NONE and an OK transport result. The
 * BH1750 driver has no rejected value.
 */
static driver_outcome from_bh1750(bh1750_status s)
{
    driver_outcome o;

    o.io = s.io;
    o.stage = (int)s.stage;
    o.stage_name = bh1750_stage_name(s.stage);
    o.ok = s.stage == BH1750_STAGE_NONE && s.io.status == I2C_STATUS_OK;
    o.has_rejected_value = 0;
    o.rejected_value = 0;
    return o;
}

/*
 * from_soil: as from_bh1750. HW_ID and RANGE rejections carry the value
 * the driver refused, with an OK transport result.
 */
static driver_outcome from_soil(soil_status s)
{
    driver_outcome o;

    o.io = s.io;
    o.stage = (int)s.stage;
    o.stage_name = soil_stage_name(s.stage);
    o.ok = s.stage == SOIL_STAGE_NONE && s.io.status == I2C_STATUS_OK;
    o.has_rejected_value =
        s.stage == SOIL_STAGE_HW_ID || s.stage == SOIL_STAGE_RANGE;
    o.rejected_value = s.rejected_value;
    return o;
}

/* light_init_op: Power On and set MTreg. */
static driver_outcome light_init_op(i2c_device *dev)
{
    return from_bh1750(bh1750_init(dev));
}

/* light_measure_op: one H-resolution reading in integer millilux. */
static driver_outcome light_measure_op(i2c_device *dev, driver_value *out)
{
    bh1750_reading reading;
    driver_outcome o = from_bh1750(bh1750_measure_once_h(dev, &reading));

    if (o.ok) {
        out->value = reading.illuminance_mlx;
        out->has_raw_count = 1;
        out->raw_count = reading.raw_count;
    }
    return o;
}

/* soil_init_op: software reset and hardware-ID check. */
static driver_outcome soil_init_op(i2c_device *dev)
{
    return from_soil(soil_init(dev));
}

/* soil_measure_op: one raw capacitance reading, uncalibrated. */
static driver_outcome soil_measure_op(i2c_device *dev, driver_value *out)
{
    soil_reading reading;
    driver_outcome o = from_soil(soil_measure_capacitance(dev, &reading));

    if (o.ok) {
        out->value = reading.capacitance_counts;
        out->has_raw_count = 0;
    }
    return o;
}

static const sensor_ops light_ops = {
    CAPTURE_UNIT_MILLILUX, light_init_op, light_measure_op
};

static const sensor_ops soil_ops = {
    CAPTURE_UNIT_RAW_COUNTS, soil_init_op, soil_measure_op
};

/* address_valid: an address the sensor's driver accepts. */
static int address_valid(capture_sensor sensor, uint8_t address)
{
    if (sensor == CAPTURE_SENSOR_LIGHT) {
        return address == BH1750_ADDRESS_ADDR_LOW ||
               address == BH1750_ADDRESS_ADDR_HIGH;
    }
    if (sensor == CAPTURE_SENSOR_SOIL) {
        return address >= SOIL_ADDRESS_MIN && address <= SOIL_ADDRESS_MAX;
    }
    return 0;
}

/* config_usable: every field is in range for its sensor. */
static int config_usable(const capture_config *cfg)
{
    return cfg != NULL && address_valid(cfg->sensor, cfg->address_7bit) &&
           cfg->tick_period_ns > 0 && cfg->sample_every_ticks >= 1 &&
           cfg->backoff_cap_periods >= 1 &&
           cfg->backoff_cap_periods <= CAPTURE_BACKOFF_CAP_MAX &&
           cfg->first_sequence >= 1;
}

/* env_usable: every seam is present. */
static int env_usable(const capture_env *env)
{
    return env != NULL && env->open_device != NULL &&
           env->read_clock != NULL && env->sleep_until != NULL &&
           env->check_clock_trust != NULL && env->stop_requested != NULL;
}

/* sink_usable: both callbacks are present. */
static int sink_usable(const capture_sink *sink)
{
    return sink != NULL && sink->emit_sample != NULL &&
           sink->emit_heartbeat != NULL;
}

/*
 * read_ns: read one clock in nanoseconds; 0, the read's errno, EINVAL for
 * a tv_nsec out of range, or ERANGE for a negative or too large value.
 */
static int read_ns(const capture_env *env, capture_clock clock,
                   int64_t *out_ns)
{
    struct timespec ts;
    intmax_t sec;
    int e = env->read_clock(env->ctx, clock, &ts);

    if (e != 0) {
        return e;
    }
    if (ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC) {
        return EINVAL;
    }
    sec = (intmax_t)ts.tv_sec;
    if (sec < 0 || sec > INT64_MAX / NSEC_PER_SEC ||
        (sec == INT64_MAX / NSEC_PER_SEC &&
         ts.tv_nsec > INT64_MAX % NSEC_PER_SEC)) {
        return ERANGE;
    }
    *out_ns = (int64_t)sec * NSEC_PER_SEC + ts.tv_nsec;
    return 0;
}

/* deadline_of: anchor + tick * period, or ERANGE if it would overflow. */
static int deadline_of(const run_state *st, uint64_t tick, int64_t *out_ns)
{
    int64_t period = st->cfg->tick_period_ns;

    if (tick > (uint64_t)((INT64_MAX - st->anchor_ns) / period)) {
        return ERANGE;
    }
    *out_ns = st->anchor_ns + (int64_t)tick * period;
    return 0;
}

/*
 * backoff_periods: min(2^(n-1), cap) for n >= 1 consecutive failures.
 * Once doubling would pass the cap it takes the cap instead, so it never
 * overflows.
 */
static uint32_t backoff_periods(uint32_t failures, uint32_t cap)
{
    uint32_t periods = 1;
    uint32_t i;

    for (i = 1; i < failures; i++) {
        if (periods > cap / 2) {
            periods = cap;
            break;
        }
        periods *= 2;
    }
    return periods < cap ? periods : cap;
}

/* current_state: STARTING until an attempt completes, then OK/DEGRADED. */
static capture_state current_state(const run_state *st)
{
    if (st->attempts == 0) {
        return CAPTURE_STATE_STARTING;
    }
    return st->consecutive_failures == 0 ? CAPTURE_STATE_OK
                                         : CAPTURE_STATE_DEGRADED;
}

/* emit_heartbeat: report this tick's state; 0 or the sink's errno. */
static int emit_heartbeat(const run_state *st)
{
    capture_heartbeat h;

    memset(&h, 0, sizeof h);
    h.sensor = st->cfg->sensor;
    h.is_simulated = st->simulated;
    h.state = current_state(st);
    h.tick = st->tick;
    h.scheduled_mono_ns = st->tick_deadline_ns;
    h.last_sequence = st->last_sequence;
    h.consecutive_failures = st->consecutive_failures;
    h.attempts = st->attempts;
    h.failures = st->failures;
    h.missed_ticks = st->missed_ticks;
    h.next_attempt_tick = st->next_attempt_tick;
    h.sensor_initialized = st->initialized;
    return st->sink->emit_heartbeat(st->sink->ctx, &h);
}

/* fill_from: copy a driver outcome into the sample. */
static void fill_from(capture_sample *s, const driver_outcome *o,
                      capture_phase phase)
{
    s->phase = phase;
    s->valid = o->ok;
    s->stage = o->stage;
    s->stage_name = o->stage_name;
    s->io = o->io;
    s->has_rejected_value = o->has_rejected_value;
    s->rejected_value = o->rejected_value;
}

/*
 * record_result: update the counters, the backoff and the
 * reinitialization state from one attempt's sample.
 */
static void record_result(run_state *st, capture_sample *s)
{
    const capture_config *cfg = st->cfg;
    uint32_t delay_periods = 1;

    st->attempts++;
    st->last_sequence = s->sequence;
    if (s->valid) {
        st->consecutive_failures = 0;
        st->consecutive_rejections = 0;
    } else {
        st->failures++;
        if (st->consecutive_failures < UINT32_MAX) {
            st->consecutive_failures++;
        }
        if (s->phase == CAPTURE_PHASE_INIT) {
            st->consecutive_rejections = 0;
        } else if (s->io.status != I2C_STATUS_OK) {
            st->consecutive_rejections = 0;
            if (cfg->reinit_after_transport_failure) {
                s->reinit_scheduled = 1;
            }
        } else {
            st->consecutive_rejections++;
            if (cfg->reinit_after_rejections != 0 &&
                st->consecutive_rejections >= cfg->reinit_after_rejections) {
                s->reinit_scheduled = 1;
                st->consecutive_rejections = 0;
            }
        }
        if (s->reinit_scheduled) {
            st->initialized = 0;
        }
        delay_periods = backoff_periods(st->consecutive_failures,
                                        cfg->backoff_cap_periods);
    }
    st->next_attempt_tick =
        s->tick + (uint64_t)cfg->sample_every_ticks * delay_periods;
}

/*
 * attempt: initialize if needed, then measure if initialized; then read
 * the wall clock, the monotonic clock and the clock trust, in that order,
 * so the timestamps mark when the attempt completed.
 */
static void attempt(run_state *st, capture_sample *s)
{
    driver_outcome o;
    driver_value v = {0, 0, 0};
    int trust_errno = 0;

    memset(s, 0, sizeof *s);
    s->sensor = st->cfg->sensor;
    s->sequence = (uint32_t)st->next_sequence;
    s->unit = st->ops->unit;
    s->tick = st->tick;
    st->next_sequence++;

    if (!st->initialized) {
        o = st->ops->init(&st->dev);
        if (o.ok) {
            st->initialized = 1;
            s->included_init = 1;
        } else {
            fill_from(s, &o, CAPTURE_PHASE_INIT);
        }
    }
    if (st->initialized) {
        o = st->ops->measure(&st->dev, &v);
        fill_from(s, &o, CAPTURE_PHASE_MEASURE);
        if (o.ok) {
            s->value = v.value;
            s->has_raw_count = v.has_raw_count;
            s->raw_count = v.raw_count;
        }
    }

    s->wall_errno = read_ns(st->env, CAPTURE_CLOCK_WALL, &s->wall_ns);
    s->mono_errno = read_ns(st->env, CAPTURE_CLOCK_MONOTONIC, &s->mono_ns);
    s->clock_trust = st->env->check_clock_trust(st->env->ctx, &trust_errno);
    s->clock_trust_errno =
        s->clock_trust == CAPTURE_CLOCK_TRUST_UNKNOWN ? trust_errno : 0;
    s->is_simulated = st->simulated;
    record_result(st, s);
}

/*
 * advance_tick: move to the first later tick whose deadline has not
 * passed (a deadline equal to now still counts as on time), counting the
 * ticks whose deadlines have passed as missed. An attempt that was due on
 * a missed tick runs on the new tick. 0, or an errno.
 */
static int advance_tick(run_state *st, int64_t now_ns)
{
    uint64_t next = st->tick + 1;
    int64_t deadline;
    int e = deadline_of(st, next, &deadline);

    if (e != 0) {
        return e;
    }
    if (now_ns > deadline) {
        int64_t elapsed = now_ns - st->anchor_ns;
        int64_t period = st->cfg->tick_period_ns;
        uint64_t first_on_time = (uint64_t)(elapsed / period) +
                                 (elapsed % period != 0 ? 1 : 0);

        st->missed_ticks += first_on_time - next;
        next = first_on_time;
        e = deadline_of(st, next, &deadline);
        if (e != 0) {
            return e;
        }
    }
    if (st->next_attempt_tick < next) {
        st->next_attempt_tick = next;
    }
    st->tick = next;
    st->tick_deadline_ns = deadline;
    return 0;
}

/* fail: record the first fatal failure. */
static void fail(capture_outcome *res, capture_exit exit, int err)
{
    res->exit = exit;
    res->exit_errno = err;
}

/* capture_run: see capture.h. */
capture_exit capture_run(const capture_config *cfg, const capture_env *env,
                         const capture_sink *sink, capture_outcome *out)
{
    capture_outcome local;
    capture_outcome *res = out != NULL ? out : &local;
    run_state st;
    capture_sample sample;
    int64_t now_ns;
    int e;

    memset(res, 0, sizeof *res);
    res->exit = CAPTURE_EXIT_USAGE;
    if (!config_usable(cfg) || !env_usable(env) || !sink_usable(sink)) {
        return res->exit;
    }

    memset(&st, 0, sizeof st);
    st.cfg = cfg;
    st.env = env;
    st.sink = sink;
    st.ops = cfg->sensor == CAPTURE_SENSOR_LIGHT ? &light_ops : &soil_ops;
    st.simulated = env->is_simulated != 0;
    st.next_sequence = cfg->first_sequence;
    res->exit = CAPTURE_EXIT_STOPPED;

    e = read_ns(env, CAPTURE_CLOCK_MONOTONIC, &st.anchor_ns);
    if (e != 0) {
        fail(res, CAPTURE_EXIT_CLOCK_FAILED, e);
        return res->exit;
    }
    st.tick_deadline_ns = st.anchor_ns;

    for (;;) {
        e = emit_heartbeat(&st);
        if (e != 0) {
            fail(res, CAPTURE_EXIT_SINK_FAILED, e);
            goto close_device;
        }

        if (!st.opened) {
            int cleanup_errno = 0;

            if (env->stop_requested(env->ctx)) {
                goto close_device;   /* nothing is open yet */
            }

            res->open_io = env->open_device(env->ctx, &st.dev,
                                            cfg->bus_number,
                                            cfg->address_7bit,
                                            &cleanup_errno);
            res->open_cleanup_errno = cleanup_errno;
            if (res->open_io.status != I2C_STATUS_OK) {
                fail(res, CAPTURE_EXIT_OPEN_FAILED, res->open_io.os_errno);
                return res->exit;
            }
            st.opened = 1;
            st.simulated = st.simulated || st.dev.is_simulated;
        }

        if (st.tick >= st.next_attempt_tick) {
            if (st.next_sequence > UINT32_MAX) {
                fail(res, CAPTURE_EXIT_SEQUENCE_EXHAUSTED, 0);
                goto close_device;
            }
            attempt(&st, &sample);
            e = sink->emit_sample(sink->ctx, &sample);
            if (e != 0) {
                fail(res, CAPTURE_EXIT_SINK_FAILED, e);
                goto close_device;
            }
        }

        e = read_ns(env, CAPTURE_CLOCK_MONOTONIC, &now_ns);
        if (e == 0) {
            e = advance_tick(&st, now_ns);
        }
        if (e != 0) {
            fail(res, CAPTURE_EXIT_CLOCK_FAILED, e);
            goto close_device;
        }

        for (;;) {
            if (env->stop_requested(env->ctx)) {
                goto close_device;
            }
            e = env->sleep_until(env->ctx, st.tick_deadline_ns);
            if (e == 0) {
                break;
            }
            if (e != EINTR) {
                fail(res, CAPTURE_EXIT_SLEEP_FAILED, e);
                goto close_device;
            }
        }
    }

close_device:
    if (st.opened) {
        res->close_attempted = 1;
        res->close_io = i2c_device_close(&st.dev);
        if (res->close_io.status != I2C_STATUS_OK &&
            res->exit == CAPTURE_EXIT_STOPPED) {
            fail(res, CAPTURE_EXIT_CLOSE_FAILED, res->close_io.os_errno);
        }
    }
    return res->exit;
}

/* capture_config_proposed: see capture.h; every value is a proposal. */
void capture_config_proposed(capture_config *cfg, capture_sensor sensor,
                             unsigned bus_number, uint8_t address_7bit)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->sensor = sensor;
    cfg->bus_number = bus_number;
    cfg->address_7bit = address_7bit;
    cfg->tick_period_ns = CAPTURE_PROPOSED_TICK_PERIOD_NS;
    cfg->sample_every_ticks = sensor == CAPTURE_SENSOR_SOIL
                                  ? CAPTURE_PROPOSED_SOIL_EVERY_TICKS
                                  : CAPTURE_PROPOSED_LIGHT_EVERY_TICKS;
    cfg->backoff_cap_periods = CAPTURE_PROPOSED_BACKOFF_CAP_PERIODS;
    cfg->reinit_after_transport_failure =
        CAPTURE_PROPOSED_REINIT_AFTER_TRANSPORT_FAILURE;
    cfg->reinit_after_rejections = CAPTURE_PROPOSED_REINIT_AFTER_REJECTIONS;
    cfg->first_sequence = CAPTURE_DEFAULT_FIRST_SEQUENCE;
}

/* capture_state_name: stable text for logs. */
const char *capture_state_name(capture_state state)
{
    switch (state) {
    case CAPTURE_STATE_STARTING:
        return "STARTING";
    case CAPTURE_STATE_OK:
        return "OK";
    case CAPTURE_STATE_DEGRADED:
        return "DEGRADED";
    default:
        return "UNKNOWN";
    }
}

/* capture_exit_name: stable text for logs. */
const char *capture_exit_name(capture_exit exit)
{
    switch (exit) {
    case CAPTURE_EXIT_STOPPED:
        return "STOPPED";
    case CAPTURE_EXIT_USAGE:
        return "USAGE";
    case CAPTURE_EXIT_OPEN_FAILED:
        return "OPEN_FAILED";
    case CAPTURE_EXIT_CLOCK_FAILED:
        return "CLOCK_FAILED";
    case CAPTURE_EXIT_SLEEP_FAILED:
        return "SLEEP_FAILED";
    case CAPTURE_EXIT_SINK_FAILED:
        return "SINK_FAILED";
    case CAPTURE_EXIT_SEQUENCE_EXHAUSTED:
        return "SEQUENCE_EXHAUSTED";
    case CAPTURE_EXIT_CLOSE_FAILED:
        return "CLOSE_FAILED";
    default:
        return "UNKNOWN";
    }
}
