#include "storage/capture_record.h"

#include <string.h>

/* stored_time: a timestamp as stored, or 0 (and *unavailable set). */
static uint64_t stored_time(int64_t ns, int err, int *unavailable)
{
    if (err != 0 || ns < 0) {
        *unavailable = 1;
        return 0;
    }
    return (uint64_t)ns;
}

/* time_flags: CLOCK_UNTRUSTED and TIME_UNAVAILABLE for one attempt. */
static uint16_t time_flags(capture_clock_trust trust, int unavailable)
{
    uint16_t flags = 0;

    if (trust != CAPTURE_CLOCK_TRUSTED) {
        flags |= STORAGE_FLAG_CLOCK_UNTRUSTED;
    }
    if (unavailable) {
        flags |= STORAGE_FLAG_TIME_UNAVAILABLE;
    }
    return flags;
}

/* stored_errno: errno as u16, or 0xFFFF when it does not fit. */
static uint16_t stored_errno(int err)
{
    if (err < 0 || err > 0xFFFF) {
        return STORAGE_ERRNO_UNREPRESENTABLE;
    }
    return (uint16_t)err;
}

/* storage_record_from_sample: see capture_record.h. */
void storage_record_from_sample(const capture_sample *s, uint64_t sequence,
                                storage_record *rec)
{
    int unavailable = 0;

    memset(rec, 0, sizeof *rec);
    rec->record_type = s->sensor == CAPTURE_SENSOR_LIGHT
                           ? STORAGE_RECORD_TYPE_LIGHT
                           : STORAGE_RECORD_TYPE_SOIL;
    rec->sequence = sequence;
    rec->wall_clock_ns = stored_time(s->wall_ns, s->wall_errno, &unavailable);
    rec->monotonic_ns = stored_time(s->mono_ns, s->mono_errno, &unavailable);
    rec->flags = time_flags(s->clock_trust, unavailable);
    if (s->is_simulated) {
        rec->flags |= STORAGE_FLAG_SIMULATED;
    }
    rec->source_sequence = s->sequence;
    if (s->valid) {
        rec->flags |= STORAGE_FLAG_VALID;
        rec->sensor_value = s->value;
        return;
    }
    if (s->sensor == CAPTURE_SENSOR_SOIL && s->has_rejected_value) {
        rec->sensor_value = s->rejected_value;
    }
    rec->i2c_status = (uint8_t)s->io.status;
    rec->failure_stage = (uint8_t)s->stage;
    rec->os_errno = stored_errno(s->io.os_errno);
}

/* storage_record_start: see capture_record.h. */
void storage_record_start(uint64_t sequence, int simulated,
                          int64_t wall_ns, int wall_errno,
                          int64_t mono_ns, int mono_errno,
                          capture_clock_trust trust, storage_record *rec)
{
    int unavailable = 0;

    memset(rec, 0, sizeof *rec);
    rec->record_type = STORAGE_RECORD_TYPE_START;
    rec->sequence = sequence;
    rec->wall_clock_ns = stored_time(wall_ns, wall_errno, &unavailable);
    rec->monotonic_ns = stored_time(mono_ns, mono_errno, &unavailable);
    rec->flags = time_flags(trust, unavailable);
    if (simulated) {
        rec->flags |= STORAGE_FLAG_SIMULATED;
    }
}
