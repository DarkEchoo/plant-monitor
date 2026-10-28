#include "capture_store.h"

#include "sensors/capture_log.h"
#include "storage/capture_record.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#define NSEC_PER_SEC INT64_C(1000000000)

/* label: the prefix every log line starts with. */
static const char *label(int simulated)
{
    return simulated ? "SIMULATED" : "LIVE";
}

/* status_name: stable text for a storage_log_status. */
static const char *status_name(storage_log_status status)
{
    switch (status) {
    case STORAGE_LOG_OK: return "OK";
    case STORAGE_LOG_ARGUMENT: return "ARGUMENT";
    case STORAGE_LOG_NOT_OPEN: return "NOT_OPEN";
    case STORAGE_LOG_MISALIGNED: return "MISALIGNED";
    case STORAGE_LOG_BAD_RECORD: return "BAD_RECORD";
    case STORAGE_LOG_SIMULATED_MISMATCH: return "SIMULATED_MISMATCH";
    case STORAGE_LOG_SEQUENCE: return "SEQUENCE";
    case STORAGE_LOG_OS_ERROR: return "OS_ERROR";
    case STORAGE_LOG_FAILED: return "FAILED";
    case STORAGE_LOG_ALREADY_OPEN: return "ALREADY_OPEN";
    case STORAGE_LOG_BAD_TAIL: return "BAD_TAIL";
    default: return "UNKNOWN";
    }
}

/* stage_name: stable text for a storage_log_stage. */
static const char *stage_name(storage_log_stage stage)
{
    switch (stage) {
    case STORAGE_LOG_STAGE_NONE: return "NONE";
    case STORAGE_LOG_STAGE_OPEN_DIR: return "OPEN_DIR";
    case STORAGE_LOG_STAGE_OPEN_FILE: return "OPEN_FILE";
    case STORAGE_LOG_STAGE_STAT: return "STAT";
    case STORAGE_LOG_STAGE_SYNC_DIR: return "SYNC_DIR";
    case STORAGE_LOG_STAGE_CLOSE_DIR: return "CLOSE_DIR";
    case STORAGE_LOG_STAGE_WRITE: return "WRITE";
    case STORAGE_LOG_STAGE_SYNC: return "SYNC";
    case STORAGE_LOG_STAGE_CLOSE_FILE: return "CLOSE_FILE";
    case STORAGE_LOG_STAGE_READ_TAIL: return "READ_TAIL";
    default: return "UNKNOWN";
    }
}

/* type_name: a record type as text. */
static const char *type_name(uint8_t record_type)
{
    switch (record_type) {
    case STORAGE_RECORD_TYPE_LIGHT: return "light";
    case STORAGE_RECORD_TYPE_SOIL: return "soil";
    case STORAGE_RECORD_TYPE_START: return "START";
    default: return "UNKNOWN";
    }
}

/* finish: flush one log line; 0 or EIO, as capture_log does. */
static int finish(FILE *out)
{
    if (fflush(out) != 0 || ferror(out)) {
        return EIO;
    }
    return 0;
}

/* line_start: the common prefix of every storage line. */
static void line_start(const capture_store *st, const char *event)
{
    fprintf(st->log, "%s sensor=%s event=%s storage=temporary-per-sensor",
            label(st->simulated), capture_sensor_name(st->sensor), event);
}

/*
 * log_store_error: one line for a failed storage operation. Returns the
 * errno to hand back to capture_run: the OS errno, else EIO.
 */
static int log_store_error(capture_store *st, const char *op,
                           storage_log_result r)
{
    line_start(st, "store_error");
    fprintf(st->log, " op=%s result=%s stage=%s errno=%d", op,
            status_name(r.status), stage_name(r.stage), r.os_errno);
    if (r.os_errno != 0) {
        fprintf(st->log, " errno_text=\"%s\"", strerror(r.os_errno));
    }
    fputc('\n', st->log);
    (void)finish(st->log);
    return r.os_errno != 0 ? r.os_errno : EIO;
}

/* log_clock_error: one line for a clock read the store needed. */
static int log_clock_error(capture_store *st, const char *op, int err)
{
    line_start(st, "store_error");
    fprintf(st->log, " op=%s result=CLOCK errno=%d errno_text=\"%s\"\n",
            op, err, strerror(err));
    (void)finish(st->log);
    return err != 0 ? err : EIO;
}

/* read_ns: one clock in nanoseconds; 0 or the errno (EOVERFLOW if bad). */
static int read_ns(const capture_env *env, capture_clock clock,
                   int64_t *out)
{
    struct timespec ts;
    int err = env->read_clock(env->ctx, clock, &ts);

    if (err != 0) {
        return err;
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC ||
        (int64_t)ts.tv_sec > (INT64_MAX - ts.tv_nsec) / NSEC_PER_SEC) {
        return EOVERFLOW;
    }
    *out = (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
    return 0;
}

/* log_flushed: a line when a batch of n records reached the file. */
static int log_flushed(capture_store *st, size_t n, const char *reason)
{
    line_start(st, "store_flush");
    fprintf(st->log, " records=%zu reason=%s file_bytes=%" PRIu64 "\n", n,
            reason, st->writer.file_size_bytes);
    return finish(st->log);
}

/*
 * append: append *rec, log it, and log the flush if the append wrote the
 * batch. now_ns is the monotonic time for the writer's flush deadline.
 */
static int append(capture_store *st, const storage_record *rec,
                  uint64_t now_ns)
{
    size_t before = st->writer.buffered_records;
    storage_log_result r = storage_log_append(&st->writer, rec, now_ns);
    int err;

    if (r.status != STORAGE_LOG_OK) {
        return log_store_error(st, "append", r);
    }
    st->records_appended++;
    line_start(st, "stored");
    fprintf(st->log, " type=%s sequence=%" PRIu64 " source_sequence=%"
            PRIu32 " valid=%d buffered=%zu\n", type_name(rec->record_type),
            rec->sequence, rec->source_sequence,
            (rec->flags & STORAGE_FLAG_VALID) != 0,
            st->writer.buffered_records);
    err = finish(st->log);
    if (err == 0 && st->writer.buffered_records == 0) {
        err = log_flushed(st, before + 1,
                          before + 1 == STORAGE_LOG_BATCH_RECORDS
                              ? "batch_full" : "deadline");
    }
    return err;
}

/* capture_store_open: see capture_store.h. */
int capture_store_open(capture_store *st, FILE *log, const char *dir_path,
                       const capture_config *cfg, const capture_env *env)
{
    const char *name;
    storage_log_result r;
    storage_record start;
    int64_t wall_ns = 0;
    int64_t mono_ns = 0;
    int wall_err;
    int mono_err;
    int trust_err = 0;
    capture_clock_trust trust;
    size_t before;
    int err;

    memset(st, 0, sizeof *st);
    st->log = log;
    st->env = env;
    st->sensor = cfg->sensor;
    st->simulated = env->is_simulated;
    st->tick_period_ns = cfg->tick_period_ns;
    name = st->simulated ? CAPTURE_STORE_SIMULATED_FILE_NAME
                         : CAPTURE_STORE_FILE_NAME;

    r = storage_log_open(&st->writer, dir_path, name, 1, st->simulated);
    if (r.status != STORAGE_LOG_OK) {
        return log_store_error(st, "open", r);
    }
    line_start(st, "store_open");
    fprintf(st->log, " dir=%s file=%s first_sequence=1 "
            "sequence_scope=this_process_only\n", dir_path, name);
    err = finish(st->log);
    if (err != 0) {
        goto fail;
    }

    wall_err = read_ns(env, CAPTURE_CLOCK_WALL, &wall_ns);
    mono_err = read_ns(env, CAPTURE_CLOCK_MONOTONIC, &mono_ns);
    trust = env->check_clock_trust(env->ctx, &trust_err);
    if (wall_err != 0 || mono_err != 0) {
        /* Logged, not fatal: START is stored with TIME_UNAVAILABLE. */
        (void)log_clock_error(st, "start_clock",
                              wall_err != 0 ? wall_err : mono_err);
    }
    if (trust == CAPTURE_CLOCK_TRUST_UNKNOWN) {
        (void)log_clock_error(st, "start_clock_trust", trust_err);
    }
    storage_record_start(st->writer.next_sequence, st->simulated, wall_ns,
                         wall_err, mono_ns, mono_err, trust, &start);
    err = append(st, &start, mono_err == 0 ? (uint64_t)mono_ns : 0);
    if (err != 0) {
        goto fail;
    }
    before = st->writer.buffered_records;
    r = storage_log_flush(&st->writer);
    if (r.status != STORAGE_LOG_OK) {
        err = log_store_error(st, "flush", r);
        goto fail;
    }
    err = log_flushed(st, before, "start");
    if (err != 0) {
        goto fail;
    }
    return 0;

fail:
    r = storage_log_close(&st->writer);
    if (r.status != STORAGE_LOG_OK && r.status != STORAGE_LOG_FAILED) {
        (void)log_store_error(st, "close", r);
    }
    return err;
}

/* capture_store_emit_sample: log the sample, then store it. */
int capture_store_emit_sample(void *ctx, const capture_sample *sample)
{
    capture_store *st = ctx;
    storage_record rec;
    int64_t now_ns;
    int err;

    err = capture_log_sample(st->log, sample);
    if (err != 0) {
        return err;
    }
    err = read_ns(st->env, CAPTURE_CLOCK_MONOTONIC, &now_ns);
    if (err != 0) {
        return log_clock_error(st, "append_clock", err);
    }
    storage_record_from_sample(sample, st->writer.next_sequence, &rec);
    return append(st, &rec, (uint64_t)now_ns);
}

/*
 * capture_store_emit_heartbeat: log the heartbeat, then flush the batch if
 * it would be overdue by the next tick's heartbeat.
 */
int capture_store_emit_heartbeat(void *ctx,
                                 const capture_heartbeat *heartbeat)
{
    capture_store *st = ctx;
    storage_log_result r;
    size_t before;
    int64_t now_ns;
    uint64_t next_tick_ns;
    int err;

    err = capture_log_heartbeat(st->log, heartbeat);
    if (err != 0) {
        return err;
    }
    before = st->writer.buffered_records;
    if (before == 0) {
        return 0;
    }
    err = read_ns(st->env, CAPTURE_CLOCK_MONOTONIC, &now_ns);
    if (err != 0) {
        return log_clock_error(st, "flush_clock", err);
    }
    next_tick_ns = (uint64_t)now_ns;
    if (UINT64_MAX - next_tick_ns >= (uint64_t)st->tick_period_ns) {
        next_tick_ns += (uint64_t)st->tick_period_ns;
    } else {
        next_tick_ns = UINT64_MAX;
    }
    r = storage_log_flush_if_due(&st->writer, next_tick_ns);
    if (r.status != STORAGE_LOG_OK) {
        return log_store_error(st, "flush_if_due", r);
    }
    if (st->writer.buffered_records == 0) {
        return log_flushed(st, before, "deadline");
    }
    return 0;
}

/* capture_store_close: see capture_store.h. */
int capture_store_close(capture_store *st)
{
    size_t before = st->writer.buffered_records;
    storage_log_result r = storage_log_close(&st->writer);

    if (r.status != STORAGE_LOG_OK) {
        return log_store_error(st, "close", r);
    }
    line_start(st, "store_close");
    fprintf(st->log, " flushed_records=%zu records_appended=%" PRIu64
            " file_bytes=%" PRIu64 " result=OK\n", before,
            st->records_appended, st->writer.file_size_bytes);
    return finish(st->log);
}
