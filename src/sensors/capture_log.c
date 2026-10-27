#include "sensors/capture_log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

/* label: the prefix every log line starts with. */
static const char *label(int simulated)
{
    return simulated ? "SIMULATED" : "LIVE";
}

/* unit_name: the explicit unit of a sample's value; UNKNOWN if invalid. */
static const char *unit_name(capture_unit unit)
{
    switch (unit) {
    case CAPTURE_UNIT_MILLILUX:
        return "millilux";
    case CAPTURE_UNIT_RAW_COUNTS:
        return "raw_counts";
    default:
        return "UNKNOWN";
    }
}

/* phase_name: which driver call gave a sample's stage; UNKNOWN if invalid. */
static const char *phase_name(capture_phase phase)
{
    switch (phase) {
    case CAPTURE_PHASE_INIT:
        return "INIT";
    case CAPTURE_PHASE_MEASURE:
        return "MEASURE";
    default:
        return "UNKNOWN";
    }
}

/*
 * trust_name: stable text for a clock-trust result. UNKNOWN means the
 * trust check failed; INVALID means an out-of-range value.
 */
static const char *trust_name(capture_clock_trust trust)
{
    switch (trust) {
    case CAPTURE_CLOCK_TRUSTED:
        return "TRUSTED";
    case CAPTURE_CLOCK_UNTRUSTED:
        return "UNTRUSTED";
    case CAPTURE_CLOCK_TRUST_UNKNOWN:
        return "UNKNOWN";
    default:
        return "INVALID";
    }
}

/*
 * finish: flush the line, then 0, or EIO if any write to out failed. The
 * flush matters on a buffered stream, where a write error only appears
 * when the buffer is written.
 */
static int finish(FILE *out)
{
    if (fflush(out) != 0 || ferror(out)) {
        return EIO;
    }
    return 0;
}

/* print_errno: errno=N, with errno_text when N is nonzero. */
static void print_errno(FILE *out, const char *key, int err)
{
    fprintf(out, " %s=%d", key, err);
    if (err != 0) {
        fprintf(out, " %s_text=\"%s\"", key, strerror(err));
    }
}

/* print_ns: a timestamp, or none if its clock read failed. */
static void print_ns(FILE *out, const char *key, int clock_errno, int64_t ns)
{
    if (clock_errno == 0) {
        fprintf(out, " %s=%" PRId64, key, ns);
    } else {
        fprintf(out, " %s=none", key);
    }
}

/* capture_sensor_name: see capture_log.h. */
const char *capture_sensor_name(capture_sensor sensor)
{
    switch (sensor) {
    case CAPTURE_SENSOR_LIGHT:
        return "light";
    case CAPTURE_SENSOR_SOIL:
        return "soil";
    default:
        return "unknown";
    }
}

/*
 * capture_log_sample: one line per attempt. An invalid sample prints
 * value=none, never its (meaningless) value field.
 */
int capture_log_sample(FILE *out, const capture_sample *s)
{
    fprintf(out, "%s sensor=%s event=sample seq=%" PRIu32 " tick=%" PRIu64
                 " phase=%s init=%d valid=%d",
            label(s->is_simulated), capture_sensor_name(s->sensor),
            s->sequence, s->tick, phase_name(s->phase),
            s->included_init != 0, s->valid != 0);
    if (s->valid) {
        fprintf(out, " value=%" PRIu32 " unit=%s", s->value,
                unit_name(s->unit));
    } else {
        fprintf(out, " value=none unit=%s", unit_name(s->unit));
    }
    if (s->unit == CAPTURE_UNIT_MILLILUX) {
        if (s->has_raw_count) {
            fprintf(out, " raw_count=%u", (unsigned)s->raw_count);
        } else {
            fputs(" raw_count=none", out);
        }
    }
    fprintf(out, " stage=%s i2c=%s",
            s->stage_name != NULL ? s->stage_name : "none",
            i2c_status_name(s->io.status));
    print_errno(out, "errno", s->io.os_errno);
    if (s->io.status == I2C_STATUS_SHORT_TRANSFER) {
        fprintf(out, " requested_len=%zu transferred_len=%zu",
                s->io.requested_len, s->io.transferred_len);
    }
    if (s->has_rejected_value) {
        fprintf(out, " rejected_value=%u", (unsigned)s->rejected_value);
    } else {
        fputs(" rejected_value=none", out);
    }
    print_ns(out, "wall_ns", s->wall_errno, s->wall_ns);
    print_ns(out, "mono_ns", s->mono_errno, s->mono_ns);
    fprintf(out, " clock_trust=%s reinit_scheduled=%d",
            trust_name(s->clock_trust), s->reinit_scheduled != 0);
    if (s->wall_errno != 0) {
        fprintf(out, " wall_errno=%d", s->wall_errno);
    }
    if (s->mono_errno != 0) {
        fprintf(out, " mono_errno=%d", s->mono_errno);
    }
    if (s->clock_trust == CAPTURE_CLOCK_TRUST_UNKNOWN) {
        fprintf(out, " trust_errno=%d", s->clock_trust_errno);
    }
    fputc('\n', out);
    return finish(out);
}

/* capture_log_heartbeat: one line per heartbeat (a log, not a protocol). */
int capture_log_heartbeat(FILE *out, const capture_heartbeat *h)
{
    fprintf(out, "%s sensor=%s event=heartbeat tick=%" PRIu64
                 " scheduled_mono_ns=%" PRId64 " state=%s last_seq=%" PRIu32
                 " consecutive_failures=%" PRIu32 " attempts=%" PRIu64
                 " failures=%" PRIu64 " missed_ticks=%" PRIu64
                 " next_attempt_tick=%" PRIu64 " initialized=%d\n",
            label(h->is_simulated), capture_sensor_name(h->sensor), h->tick,
            h->scheduled_mono_ns, capture_state_name(h->state),
            h->last_sequence, h->consecutive_failures, h->attempts,
            h->failures, h->missed_ticks, h->next_attempt_tick,
            h->sensor_initialized != 0);
    return finish(out);
}

/*
 * capture_log_exit: how the run ended, with the open and close results.
 * capture_run zeroes the outcome, so a zero open result alone does not
 * mean the open succeeded. A successful open is always followed by a
 * close attempt; a failed one leaves a non-OK result; otherwise no open
 * was attempted (USAGE, or a clock, sink or stop exit before the open).
 */
int capture_log_exit(FILE *out, capture_sensor sensor, int is_simulated,
                     const capture_outcome *o)
{
    fprintf(out, "%s sensor=%s event=exit result=%s",
            label(is_simulated), capture_sensor_name(sensor),
            capture_exit_name(o->exit));
    print_errno(out, "errno", o->exit_errno);
    if (o->close_attempted || o->open_io.status != I2C_STATUS_OK) {
        fprintf(out, " open=%s open_errno=%d cleanup_errno=%d",
                i2c_status_name(o->open_io.status), o->open_io.os_errno,
                o->open_cleanup_errno);
    } else {
        fputs(" open=not_attempted open_errno=none cleanup_errno=none", out);
    }
    if (o->close_attempted) {
        fprintf(out, " close=%s close_errno=%d\n",
                i2c_status_name(o->close_io.status), o->close_io.os_errno);
    } else {
        fputs(" close=not_attempted close_errno=none\n", out);
    }
    return finish(out);
}
