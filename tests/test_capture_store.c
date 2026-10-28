#define _POSIX_C_SOURCE 200809L
/* macOS hides mkdtemp in strict POSIX mode; Linux ignores this. */
#define _DARWIN_C_SOURCE

#include "capture_store.h"
#include "sensors/capture.h"
#include "sensors/i2c_mock.h"
#include "storage/capture_record.h"
#include "storage/record.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Tests for capture-to-storage. Every sample is SIMULATED: either built by
 * the test, or produced by capture_run from a scripted i2c_mock device and
 * a fake clock. No sensor, /dev/i2c device or real clock is used. The
 * end-to-end tests write real files in a fresh mkdtemp directory and read
 * them back through storage_record_decode.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define PATH_CAP 512
#define MAX_STEPS 64
#define SEC INT64_C(1000000000)
#define ANCHOR_NS (5 * SEC)
#define WALL_OFFSET_NS (INT64_C(1700000000) * SEC)
#define MAX_FILE_RECORDS 16
#define CLOCK_STEP_NS INT64_C(1000000)   /* 1 ms per clock read */
#define LOG_CAP 16384

static const uint8_t cmd_power_on[] = {0x01};
static const uint8_t cmd_mtreg_high[] = {0x42};
static const uint8_t cmd_mtreg_low[] = {0x65};
static const uint8_t cmd_one_time_h[] = {0x20};
static const uint8_t light_raw_12[] = {0x00, 0x0c};  /* 12 counts: 10000 mlx */
static const uint8_t cmd_reset[] = {0x00, 0x7f, 0xff};
static const uint8_t cmd_hw_id[] = {0x00, 0x01};
static const uint8_t cmd_touch0[] = {0x0f, 0x10};
static const uint8_t id_samd[] = {0x55};
static const uint8_t soil_500[] = {0x01, 0xf4};

/* ---- record conversion ---- */

/* base_sample: a valid SIMULATED light sample with trusted clocks. */
static capture_sample base_sample(void)
{
    capture_sample s;

    memset(&s, 0, sizeof s);
    s.sensor = CAPTURE_SENSOR_LIGHT;
    s.sequence = 7;
    s.is_simulated = 1;
    s.phase = CAPTURE_PHASE_MEASURE;
    s.valid = 1;
    s.unit = CAPTURE_UNIT_MILLILUX;
    s.value = 12500;
    s.has_raw_count = 1;
    s.raw_count = 15;
    s.io.status = I2C_STATUS_OK;
    s.wall_ns = WALL_OFFSET_NS + 3 * SEC;
    s.mono_ns = 3 * SEC;
    s.clock_trust = CAPTURE_CLOCK_TRUSTED;
    return s;
}

/* test_valid_light: value stored, bytes 36-39 zero, flags VALID|SIM. */
static void test_valid_light(void)
{
    capture_sample s = base_sample();
    storage_record r;

    storage_record_from_sample(&s, 42, &r);
    assert(r.record_type == STORAGE_RECORD_TYPE_LIGHT);
    assert(r.sequence == 42 && r.source_sequence == 7);
    assert(r.flags == (STORAGE_FLAG_VALID | STORAGE_FLAG_SIMULATED));
    assert(r.wall_clock_ns == (uint64_t)(WALL_OFFSET_NS + 3 * SEC));
    assert(r.monotonic_ns == (uint64_t)(3 * SEC));
    assert(r.sensor_value == 12500);
    assert(r.i2c_status == 0 && r.failure_stage == 0 && r.os_errno == 0);
}

/* test_live_valid_soil: LIVE has no SIMULATED flag; soil counts stored. */
static void test_live_valid_soil(void)
{
    capture_sample s = base_sample();
    storage_record r;

    s.sensor = CAPTURE_SENSOR_SOIL;
    s.is_simulated = 0;
    s.unit = CAPTURE_UNIT_RAW_COUNTS;
    s.value = 802;
    s.has_raw_count = 0;
    storage_record_from_sample(&s, 2, &r);
    assert(r.record_type == STORAGE_RECORD_TYPE_SOIL);
    assert(r.flags == STORAGE_FLAG_VALID);
    assert(r.sensor_value == 802);
}

/*
 * test_invalid_soil_rejected: a RANGE rejection has an OK transport but
 * is still invalid, and keeps its stage and rejected value.
 */
static void test_invalid_soil_rejected(void)
{
    capture_sample s = base_sample();
    storage_record r;

    s.sensor = CAPTURE_SENSOR_SOIL;
    s.valid = 0;
    s.value = 999;              /* meaningless when invalid; not stored */
    s.has_rejected_value = 1;
    s.rejected_value = 5000;
    s.stage = 6;
    s.io.status = I2C_STATUS_OK;
    storage_record_from_sample(&s, 3, &r);
    assert((r.flags & STORAGE_FLAG_VALID) == 0);
    assert(r.sensor_value == 5000);
    assert(r.failure_stage == 6 && r.i2c_status == I2C_STATUS_OK);
}

/* test_invalid_light: value 0, transport status, stage and errno kept. */
static void test_invalid_light(void)
{
    capture_sample s = base_sample();
    storage_record r;

    s.valid = 0;
    s.value = 12500;
    s.has_rejected_value = 1;   /* light never stores a rejected value */
    s.rejected_value = 77;
    s.stage = 7;
    s.io.status = I2C_STATUS_OS_ERROR;
    s.io.os_errno = EIO;
    storage_record_from_sample(&s, 4, &r);
    assert(r.flags == STORAGE_FLAG_SIMULATED);
    assert(r.sensor_value == 0);
    assert(r.i2c_status == I2C_STATUS_OS_ERROR && r.failure_stage == 7);
    assert(r.os_errno == EIO);
}

/* test_errno_clamped: an errno outside 0-65535 is stored as 0xFFFF. */
static void test_errno_clamped(void)
{
    capture_sample s = base_sample();
    storage_record r;

    s.valid = 0;
    s.io.status = I2C_STATUS_OS_ERROR;
    s.io.os_errno = 70000;
    storage_record_from_sample(&s, 5, &r);
    assert(r.os_errno == STORAGE_ERRNO_UNREPRESENTABLE);
    s.io.os_errno = -1;
    storage_record_from_sample(&s, 5, &r);
    assert(r.os_errno == STORAGE_ERRNO_UNREPRESENTABLE);
}

/*
 * test_time_flags: an unreadable or negative timestamp is stored as 0 with
 * TIME_UNAVAILABLE; UNTRUSTED and UNKNOWN trust both set CLOCK_UNTRUSTED.
 */
static void test_time_flags(void)
{
    capture_sample s = base_sample();
    storage_record r;

    s.wall_errno = EINVAL;
    storage_record_from_sample(&s, 6, &r);
    assert(r.wall_clock_ns == 0 && r.monotonic_ns == (uint64_t)(3 * SEC));
    assert(r.flags & STORAGE_FLAG_TIME_UNAVAILABLE);

    s = base_sample();
    s.mono_ns = -1;
    storage_record_from_sample(&s, 6, &r);
    assert(r.monotonic_ns == 0 && (r.flags & STORAGE_FLAG_TIME_UNAVAILABLE));

    s = base_sample();
    s.clock_trust = CAPTURE_CLOCK_UNTRUSTED;
    storage_record_from_sample(&s, 6, &r);
    assert(r.flags & STORAGE_FLAG_CLOCK_UNTRUSTED);
    assert((r.flags & STORAGE_FLAG_TIME_UNAVAILABLE) == 0);

    s.clock_trust = CAPTURE_CLOCK_TRUST_UNKNOWN;
    storage_record_from_sample(&s, 6, &r);
    assert(r.flags & STORAGE_FLAG_CLOCK_UNTRUSTED);
}

/* test_start_record: type START, bytes 32-39 zero, flags from inputs. */
static void test_start_record(void)
{
    storage_record r;

    storage_record_start(1, 0, 11, 0, 22, 0, CAPTURE_CLOCK_TRUSTED, &r);
    assert(r.record_type == STORAGE_RECORD_TYPE_START && r.sequence == 1);
    assert(r.flags == 0 && r.wall_clock_ns == 11 && r.monotonic_ns == 22);
    assert(r.sensor_value == 0 && r.i2c_status == 0 &&
           r.failure_stage == 0 && r.os_errno == 0 &&
           r.source_sequence == 0);

    storage_record_start(1, 1, 11, EINVAL, 22, 0,
                         CAPTURE_CLOCK_UNTRUSTED, &r);
    assert(r.flags == (STORAGE_FLAG_SIMULATED | STORAGE_FLAG_CLOCK_UNTRUSTED |
                       STORAGE_FLAG_TIME_UNAVAILABLE));
    assert(r.wall_clock_ns == 0);
}

/* ---- end to end: capture_run -> capture_store -> file ---- */

/* The fake world: a scripted mock device, a fake clock, a stop count. */
typedef struct {
    i2c_mock_step steps[MAX_STEPS];
    size_t step_count;
    i2c_mock mock;
    int64_t now_ns;
    unsigned heartbeats;
    unsigned stop_after_heartbeats;
    capture_store *store;
    long file_bytes_at_heartbeat[32];  /* file size seen at each one */
    int64_t now_at_heartbeat[32];      /* fake time after each one */
    long file_bytes_after_sample[32];  /* file size after each sample */
    unsigned samples;
} fake;

static char temp_dir[PATH_CAP];

/* add: append one scripted step. */
static void add(fake *f, i2c_mock_op op, const uint8_t *bytes, size_t len,
                uint32_t min_wait_us)
{
    i2c_mock_step step = {op, bytes, len, len, 0, min_wait_us};

    assert(f->step_count < MAX_STEPS);
    f->steps[f->step_count++] = step;
}

/* script_light: init, then n measurements of 12 counts. */
static void script_light(fake *f, unsigned n)
{
    add(f, I2C_MOCK_WRITE, cmd_power_on, 1, 0);
    add(f, I2C_MOCK_WRITE, cmd_mtreg_high, 1, 0);
    add(f, I2C_MOCK_WRITE, cmd_mtreg_low, 1, 0);
    while (n-- > 0) {
        add(f, I2C_MOCK_WRITE, cmd_one_time_h, 1, 0);
        add(f, I2C_MOCK_WAIT, NULL, 0, 180000);
        add(f, I2C_MOCK_READ, light_raw_12, 2, 0);
    }
}

/* script_soil: init, then n measurements of 500 counts. */
static void script_soil(fake *f, unsigned n)
{
    add(f, I2C_MOCK_WRITE, cmd_reset, sizeof cmd_reset, 0);
    add(f, I2C_MOCK_WAIT, NULL, 0, 500000);
    add(f, I2C_MOCK_WRITE, cmd_hw_id, sizeof cmd_hw_id, 0);
    add(f, I2C_MOCK_WAIT, NULL, 0, 8000);
    add(f, I2C_MOCK_READ, id_samd, 1, 0);
    while (n-- > 0) {
        add(f, I2C_MOCK_WRITE, cmd_touch0, sizeof cmd_touch0, 0);
        add(f, I2C_MOCK_WAIT, NULL, 0, 5000);
        add(f, I2C_MOCK_READ, soil_500, 2, 0);
    }
}

/* fake_open: open the scripted mock (always SIMULATED). */
static i2c_result fake_open(void *ctx, i2c_device *dev, unsigned bus_number,
                            uint8_t address_7bit, int *cleanup_errno)
{
    fake *f = ctx;
    i2c_result r = {I2C_STATUS_OK, 0, 0, 0};

    (void)bus_number;
    *cleanup_errno = 0;
    assert(i2c_mock_open(&f->mock, dev, address_7bit, f->steps,
                         f->step_count) == I2C_STATUS_OK);
    return r;
}

/*
 * fake_read_clock: wall = WALL_OFFSET + monotonic. Every read advances
 * the clock by CLOCK_STEP_NS, so work takes time as on real hardware: an
 * attempt (several reads) ends later than the heartbeat before it.
 */
static int fake_read_clock(void *ctx, capture_clock clock,
                           struct timespec *out)
{
    fake *f = ctx;
    int64_t ns = f->now_ns;

    f->now_ns += CLOCK_STEP_NS;
    if (clock == CAPTURE_CLOCK_WALL) {
        ns += WALL_OFFSET_NS;
    }
    out->tv_sec = (time_t)(ns / SEC);
    out->tv_nsec = (long)(ns % SEC);
    return 0;
}

/* fake_sleep_until: jump the fake clock to the deadline. */
static int fake_sleep_until(void *ctx, int64_t deadline_mono_ns)
{
    fake *f = ctx;

    if (deadline_mono_ns > f->now_ns) {
        f->now_ns = deadline_mono_ns;
    }
    return 0;
}

/* fake_trust: always untrusted, as on the Pi with no network. */
static capture_clock_trust fake_trust(void *ctx, int *err)
{
    (void)ctx;
    *err = 0;
    return CAPTURE_CLOCK_UNTRUSTED;
}

/* fake_stop: stop after the configured number of heartbeats. */
static int fake_stop(void *ctx)
{
    fake *f = ctx;

    return f->heartbeats >= f->stop_after_heartbeats;
}

/* file_size: bytes in temp_dir/name, or -1 if it does not exist. */
static long file_size(const char *name)
{
    char path[PATH_CAP];
    FILE *fp;
    long n;

    assert(snprintf(path, sizeof path, "%s/%s", temp_dir, name) > 0);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    assert(fseek(fp, 0, SEEK_END) == 0);
    n = ftell(fp);
    assert(fclose(fp) == 0);
    return n;
}

/* log_name: the SIMULATED log file name for sensor. */
static const char *log_name(capture_sensor sensor)
{
    return capture_store_file_name(sensor, 1);
}

/* count_heartbeat: the store's heartbeat sink, plus a count and size. */
static int count_heartbeat(void *ctx, const capture_heartbeat *h)
{
    fake *f = ctx;
    int err = capture_store_emit_heartbeat(f->store, h);

    assert(f->heartbeats < COUNT_OF(f->file_bytes_at_heartbeat));
    f->file_bytes_at_heartbeat[f->heartbeats] =
        file_size(log_name(f->store->sensor));
    f->now_at_heartbeat[f->heartbeats] = f->now_ns;
    f->heartbeats++;
    return err;
}

/* forward_sample: the store's sample sink, plus the size after it. */
static int forward_sample(void *ctx, const capture_sample *s)
{
    fake *f = ctx;
    int err = capture_store_emit_sample(f->store, s);

    assert(f->samples < COUNT_OF(f->file_bytes_after_sample));
    f->file_bytes_after_sample[f->samples++] =
        file_size(log_name(f->store->sensor));
    return err;
}

/* make_temp_dir: a fresh, empty directory for one test. */
static void make_temp_dir(void)
{
    const char *base = getenv("TMPDIR");
    int n;

    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    n = snprintf(temp_dir, sizeof temp_dir, "%s/capture-store-test.XXXXXX",
                 base);
    assert(n > 0 && (size_t)n < sizeof temp_dir);
    assert(mkdtemp(temp_dir) != NULL);
}

/* remove_temp: delete the log files (if any) and the directory. */
static void remove_temp(void)
{
    char path[PATH_CAP];

    assert(snprintf(path, sizeof path, "%s/%s", temp_dir,
                    log_name(CAPTURE_SENSOR_LIGHT)) > 0);
    (void)unlink(path);
    assert(snprintf(path, sizeof path, "%s/%s", temp_dir,
                    log_name(CAPTURE_SENSOR_SOIL)) > 0);
    (void)unlink(path);
    assert(rmdir(temp_dir) == 0);
}

/* read_records: decode every record of the log; return the count. */
static size_t read_records(capture_sensor sensor, storage_record *out,
                           size_t cap)
{
    char path[PATH_CAP];
    uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
    FILE *fp;
    size_t n = 0;
    size_t got;

    assert(snprintf(path, sizeof path, "%s/%s", temp_dir,
                    log_name(sensor)) > 0);
    fp = fopen(path, "rb");
    assert(fp != NULL);
    while ((got = fread(buf, 1, sizeof buf, fp)) == sizeof buf) {
        assert(n < cap);
        assert(storage_record_decode(buf, &out[n]) == STORAGE_RECORD_OK);
        n++;
    }
    assert(got == 0 && !ferror(fp));   /* whole records only */
    assert(fclose(fp) == 0);
    return n;
}

/* read_log: the text log written so far. */
static void read_log(FILE *log, char *out, size_t cap)
{
    size_t n;

    assert(fflush(log) == 0);
    rewind(log);
    n = fread(out, 1, cap - 1, log);
    assert(!ferror(log) && n < cap - 1);
    out[n] = '\0';
}

/* every_line_simulated: every text line is labelled SIMULATED. */
static void assert_every_line_simulated(const char *text)
{
    const char *line = text;

    while (*line != '\0') {
        assert(strncmp(line, "SIMULATED ", 10) == 0);
        line = strchr(line, '\n');
        assert(line != NULL);
        line++;
    }
}

/* run_capture: open the store, run the core, return the exit. */
static capture_exit run_capture(fake *f, const capture_config *cfg,
                                FILE *log, capture_store *st)
{
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    capture_sink sink = {forward_sample, count_heartbeat, NULL};
    capture_outcome out;
    capture_exit e;

    env.ctx = f;
    sink.ctx = f;
    f->store = st;
    assert(capture_store_open(st, log, temp_dir, cfg, &env) == 0);
    e = capture_run(cfg, &env, &sink, &out);
    return e;
}

/*
 * test_light_end_to_end: START plus three light records reach the file in
 * order, gapless from 1, each SIMULATED, VALID and CLOCK_UNTRUSTED, with
 * the capture counter in source_sequence.
 */
static void test_light_end_to_end(void)
{
    static fake f;
    static capture_store st;
    storage_record recs[MAX_FILE_RECORDS];
    char text[LOG_CAP];
    capture_config cfg;
    FILE *log = tmpfile();
    size_t n;
    size_t i;

    assert(log != NULL);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    f.stop_after_heartbeats = 3;
    script_light(&f, 3);
    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);

    assert(run_capture(&f, &cfg, log, &st) == CAPTURE_EXIT_STOPPED);
    assert(f.mock.next_step == f.step_count && !f.mock.mismatch);
    assert(capture_store_close(&st) == 0);

    n = read_records(CAPTURE_SENSOR_LIGHT, recs, COUNT_OF(recs));
    assert(n == 4);
    assert(recs[0].record_type == STORAGE_RECORD_TYPE_START);
    for (i = 0; i < n; i++) {
        assert(recs[i].sequence == i + 1);
        assert(recs[i].flags & STORAGE_FLAG_SIMULATED);
        assert(recs[i].flags & STORAGE_FLAG_CLOCK_UNTRUSTED);
    }
    for (i = 1; i < n; i++) {
        assert(recs[i].record_type == STORAGE_RECORD_TYPE_LIGHT);
        assert(recs[i].flags & STORAGE_FLAG_VALID);
        assert(recs[i].sensor_value == 10000);
        assert(recs[i].source_sequence == i);
        /* One sample per 10 s tick, taken within 1 s of its deadline. */
        assert(recs[i].monotonic_ns > recs[i - 1].monotonic_ns);
        assert(recs[i].monotonic_ns >=
               (uint64_t)(ANCHOR_NS + (int64_t)(i - 1) *
                                          CAPTURE_PROPOSED_TICK_PERIOD_NS));
        assert(recs[i].monotonic_ns <
               (uint64_t)(ANCHOR_NS + SEC + (int64_t)(i - 1) *
                                          CAPTURE_PROPOSED_TICK_PERIOD_NS));
    }

    read_log(log, text, sizeof text);
    assert_every_line_simulated(text);
    assert(strstr(text, "event=store_open storage=temporary-per-sensor") !=
           NULL);
    assert(strstr(text, "sequence_scope=this_process_only") != NULL);
    assert(strstr(text, "event=stored storage=temporary-per-sensor "
                        "type=light sequence=4 source_sequence=3 valid=1")
           != NULL);
    assert(strstr(text, "event=store_close") != NULL);
    assert(fclose(log) == 0);
    remove_temp();
}

/*
 * test_soil_flushed_within_deadline: a soil record (every 6th tick) is on
 * disk before it is 60 s old, without waiting for close, even though the
 * attempt ends later than its tick's heartbeat (each fake clock read takes
 * 1 ms). Without the flush margin it would wait for the tick-6 heartbeat
 * and reach the disk only just under, or over, 60 s.
 */
static void test_soil_flushed_within_deadline(void)
{
    static fake f;
    static capture_store st;
    storage_record recs[MAX_FILE_RECORDS];
    char text[LOG_CAP];
    capture_config cfg;
    FILE *log = tmpfile();
    unsigned i;

    assert(log != NULL);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    f.stop_after_heartbeats = 7;
    script_soil(&f, 2);
    capture_config_proposed(&cfg, CAPTURE_SENSOR_SOIL, 1, 0x36);

    assert(run_capture(&f, &cfg, log, &st) == CAPTURE_EXIT_STOPPED);
    assert(f.mock.next_step == f.step_count && !f.mock.mismatch);

    /*
     * START alone until the sample's batch is due: at the tick-5 heartbeat
     * it would be within the margin of 60 s by tick 6, so it is flushed
     * then, about 50 s after it was taken.
     */
    for (i = 0; i < 5; i++) {
        assert(f.file_bytes_at_heartbeat[i] == STORAGE_RECORD_SIZE_BYTES);
    }
    assert(f.file_bytes_at_heartbeat[5] == 2 * STORAGE_RECORD_SIZE_BYTES);
    assert(f.file_bytes_at_heartbeat[6] == 2 * STORAGE_RECORD_SIZE_BYTES);

    assert(capture_store_close(&st) == 0);
    assert(read_records(CAPTURE_SENSOR_SOIL, recs, COUNT_OF(recs)) == 3);
    assert(recs[1].record_type == STORAGE_RECORD_TYPE_SOIL &&
           recs[1].sensor_value == 500 && recs[1].source_sequence == 1);
    /* Taken after tick 0's heartbeat; on disk well under 60 s later. */
    assert(recs[1].monotonic_ns > (uint64_t)f.now_at_heartbeat[0]);
    assert(f.now_at_heartbeat[5] - (int64_t)recs[1].monotonic_ns <
           60 * SEC - CAPTURE_STORE_FLUSH_MARGIN_NS);
    assert(recs[2].record_type == STORAGE_RECORD_TYPE_SOIL &&
           recs[2].sequence == 3 && recs[2].source_sequence == 2);

    read_log(log, text, sizeof text);
    assert_every_line_simulated(text);
    assert(strstr(text, "event=store_flush storage=temporary-per-sensor "
                        "records=1 reason=deadline") != NULL);
    assert(fclose(log) == 0);
    remove_temp();
}

/*
 * test_previous_log_refused: a second run into the same directory is
 * refused (no recovery yet), logged, and leaves the old file unchanged.
 */
static void test_previous_log_refused(void)
{
    static fake f;
    static capture_store st;
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    char text[LOG_CAP];
    capture_config cfg;
    FILE *log = tmpfile();
    long before;

    assert(log != NULL);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    env.ctx = &f;
    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);

    assert(capture_store_open(&st, log, temp_dir, &cfg, &env) == 0);
    assert(capture_store_close(&st) == 0);
    before = file_size(log_name(CAPTURE_SENSOR_LIGHT));
    assert(before == STORAGE_RECORD_SIZE_BYTES);

    assert(capture_store_open(&st, log, temp_dir, &cfg, &env) != 0);
    assert(!st.writer.is_open);
    assert(file_size(log_name(CAPTURE_SENSOR_LIGHT)) == before);
    read_log(log, text, sizeof text);
    assert(strstr(text, "event=store_error storage=temporary-per-sensor "
                        "op=open result=SEQUENCE") != NULL);
    assert(fclose(log) == 0);
    remove_temp();
}

/*
 * test_long_tick_flushes_each_record: with a tick of 60 s or more there is
 * no heartbeat before the deadline, so each record is flushed right after
 * it is appended instead of waiting a whole tick.
 */
static void test_long_tick_flushes_each_record(void)
{
    static fake f;
    static capture_store st;
    storage_record recs[MAX_FILE_RECORDS];
    capture_config cfg;
    FILE *log = tmpfile();

    assert(log != NULL);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    f.stop_after_heartbeats = 2;
    script_light(&f, 2);
    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    cfg.tick_period_ns = 120 * SEC;

    assert(run_capture(&f, &cfg, log, &st) == CAPTURE_EXIT_STOPPED);
    assert(f.samples == 2);
    assert(f.file_bytes_after_sample[0] == 2 * STORAGE_RECORD_SIZE_BYTES);
    assert(f.file_bytes_after_sample[1] == 3 * STORAGE_RECORD_SIZE_BYTES);
    assert(capture_store_close(&st) == 0);
    assert(read_records(CAPTURE_SENSOR_LIGHT, recs, COUNT_OF(recs)) == 3);
    assert(fclose(log) == 0);
    remove_temp();
}

/*
 * test_shared_dir_separate_files: light and soil given the same directory
 * by mistake still write separate files, so neither is mixed or refused.
 */
static void test_shared_dir_separate_files(void)
{
    static fake f;
    static capture_store light;
    static capture_store soil;
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    capture_config light_cfg;
    capture_config soil_cfg;
    FILE *log = tmpfile();

    assert(log != NULL);
    assert(strcmp(log_name(CAPTURE_SENSOR_LIGHT),
                  "SIMULATED-plant-light-0000000000000001.log") == 0);
    assert(strcmp(capture_store_file_name(CAPTURE_SENSOR_SOIL, 0),
                  "plant-soil-0000000000000001.log") == 0);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    env.ctx = &f;
    capture_config_proposed(&light_cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    capture_config_proposed(&soil_cfg, CAPTURE_SENSOR_SOIL, 1, 0x36);

    /* Both open before either closes, as two processes started together. */
    assert(capture_store_open(&light, log, temp_dir, &light_cfg, &env) == 0);
    assert(capture_store_open(&soil, log, temp_dir, &soil_cfg, &env) == 0);
    assert(capture_store_close(&light) == 0);
    assert(capture_store_close(&soil) == 0);
    assert(file_size(log_name(CAPTURE_SENSOR_LIGHT)) ==
           STORAGE_RECORD_SIZE_BYTES);
    assert(file_size(log_name(CAPTURE_SENSOR_SOIL)) ==
           STORAGE_RECORD_SIZE_BYTES);
    assert(fclose(log) == 0);
    remove_temp();
}

/*
 * test_close_with_broken_log: when the text log can no longer be written
 * (as with a closed stdout pipe), close still flushes the buffered record
 * to storage, reports the log failure, and sets closed_ok.
 */
static void test_close_with_broken_log(void)
{
    static fake f;
    static capture_store st;
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    storage_record recs[MAX_FILE_RECORDS];
    capture_sample s = base_sample();
    capture_config cfg;
    FILE *log = tmpfile();
    FILE *broken;
    char path[PATH_CAP];

    assert(log != NULL);
    make_temp_dir();
    memset(&f, 0, sizeof f);
    f.now_ns = ANCHOR_NS;
    env.ctx = &f;
    capture_config_proposed(&cfg, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    assert(capture_store_open(&st, log, temp_dir, &cfg, &env) == 0);
    s.mono_ns = f.now_ns;
    assert(capture_store_emit_sample(&st, &s) == 0);
    assert(st.writer.buffered_records == 1);

    /* A read-only stream: every write to it fails. */
    assert(snprintf(path, sizeof path, "%s/%s", temp_dir,
                    log_name(CAPTURE_SENSOR_LIGHT)) > 0);
    broken = fopen(path, "r");
    assert(broken != NULL);
    st.log = broken;
    assert(capture_store_close(&st) == EIO);
    assert(st.closed_ok && !st.writer.is_open);
    assert(fclose(broken) == 0);

    assert(read_records(CAPTURE_SENSOR_LIGHT, recs, COUNT_OF(recs)) == 2);
    assert(recs[1].record_type == STORAGE_RECORD_TYPE_LIGHT &&
           recs[1].source_sequence == 7);
    assert(fclose(log) == 0);
    remove_temp();
}

/* test_missing_dir: a directory that does not exist is a logged error. */
static void test_missing_dir(void)
{
    static fake f;
    static capture_store st;
    capture_env env = {fake_open, fake_read_clock, fake_sleep_until,
                       fake_trust, fake_stop, NULL, 1};
    char text[LOG_CAP];
    char missing[PATH_CAP];
    capture_config cfg;
    FILE *log = tmpfile();

    assert(log != NULL);
    make_temp_dir();
    assert(snprintf(missing, sizeof missing, "%s/absent", temp_dir) > 0);
    memset(&f, 0, sizeof f);
    env.ctx = &f;
    capture_config_proposed(&cfg, CAPTURE_SENSOR_SOIL, 1, 0x36);

    assert(capture_store_open(&st, log, missing, &cfg, &env) == ENOENT);
    read_log(log, text, sizeof text);
    assert(strstr(text, "op=open result=OS_ERROR stage=OPEN_DIR errno=")
           != NULL);
    assert(fclose(log) == 0);
    remove_temp();
}

/* main: run every test; abort on the first failure. */
int main(void)
{
    test_valid_light();
    test_live_valid_soil();
    test_invalid_soil_rejected();
    test_invalid_light();
    test_errno_clamped();
    test_time_flags();
    test_start_record();
    test_light_end_to_end();
    test_soil_flushed_within_deadline();
    test_previous_log_refused();
    test_long_tick_flushes_each_record();
    test_shared_dir_separate_files();
    test_close_with_broken_log();
    test_missing_dir();

    puts("SIMULATED capture store tests passed (scripted mock, fake clock; "
         "real files in a temporary directory; no sensor hardware or "
         "/dev/i2c device used)");
    return 0;
}
