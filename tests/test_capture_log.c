#include "sensors/bh1750.h"
#include "sensors/capture.h"
#include "sensors/capture_log.h"
#include "sensors/seesaw_soil.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * SIMULATED: the samples, heartbeats and outcomes here are built by hand
 * to check the log format; none is a measurement. No device, clock or
 * bus is used. errno_text is compared with strerror() at run time, not
 * hard-coded.
 */

#define OUT_MAX 2048

/* capture: what writer() printed for one call, and its result. */
typedef struct {
    char text[OUT_MAX];
    int result;
} captured;

/* light_valid: a successful light measurement. */
static capture_sample light_valid(void)
{
    capture_sample s;

    memset(&s, 0, sizeof s);
    s.sensor = CAPTURE_SENSOR_LIGHT;
    s.sequence = 12;
    s.is_simulated = 1;
    s.phase = CAPTURE_PHASE_MEASURE;
    s.included_init = 1;
    s.valid = 1;
    s.unit = CAPTURE_UNIT_MILLILUX;
    s.value = 10000;
    s.has_raw_count = 1;
    s.raw_count = 12;
    s.stage = BH1750_STAGE_NONE;
    s.stage_name = "NONE";
    s.wall_ns = INT64_C(1700000000123456789);
    s.mono_ns = INT64_C(5180000000);
    s.clock_trust = CAPTURE_CLOCK_UNTRUSTED;
    s.tick = 34;
    return s;
}

/* open_tmp: a temporary stream for one call. */
static FILE *open_tmp(void)
{
    FILE *f = tmpfile();

    assert(f != NULL);
    return f;
}

/* read_back: everything written to f, as a string; closes f. */
static void read_back(FILE *f, char *buf, size_t size)
{
    size_t n;

    assert(fflush(f) == 0);
    rewind(f);
    n = fread(buf, 1, size - 1, f);
    assert(!ferror(f) && n < size - 1);
    buf[n] = '\0';
    assert(fclose(f) == 0);
}

/* log_sample: capture_log_sample's output and result. */
static captured log_sample(const capture_sample *s)
{
    captured c;
    FILE *f = open_tmp();

    c.result = capture_log_sample(f, s);
    read_back(f, c.text, sizeof c.text);
    return c;
}

/* test_valid_light_sample: the whole line for a valid light reading. */
static void test_valid_light_sample(void)
{
    static const char expected[] =
        "SIMULATED sensor=light event=sample seq=12 tick=34 phase=MEASURE "
        "init=1 valid=1 value=10000 unit=millilux raw_count=12 stage=NONE "
        "i2c=OK errno=0 rejected_value=none wall_ns=1700000000123456789 "
        "mono_ns=5180000000 clock_trust=UNTRUSTED reinit_scheduled=0\n";
    capture_sample s = light_valid();
    captured c = log_sample(&s);

    assert(c.result == 0);
    assert(strcmp(c.text, expected) == 0);
}

/*
 * test_rejected_soil_sample: a RANGE rejection prints valid=0 and
 * value=none (never a zero value) with the refused counts, and a live
 * sample is labelled LIVE.
 */
static void test_rejected_soil_sample(void)
{
    static const char expected[] =
        "LIVE sensor=soil event=sample seq=3 tick=12 phase=MEASURE init=0 "
        "valid=0 value=none unit=raw_counts stage=RANGE i2c=OK errno=0 "
        "rejected_value=5000 wall_ns=1700000000000000000 "
        "mono_ns=120000000000 clock_trust=TRUSTED reinit_scheduled=1\n";
    capture_sample s;
    captured c;

    memset(&s, 0, sizeof s);
    s.sensor = CAPTURE_SENSOR_SOIL;
    s.sequence = 3;
    s.phase = CAPTURE_PHASE_MEASURE;
    s.unit = CAPTURE_UNIT_RAW_COUNTS;
    s.value = 0;
    s.stage = SOIL_STAGE_RANGE;
    s.stage_name = "RANGE";
    s.has_rejected_value = 1;
    s.rejected_value = 5000;
    s.wall_ns = INT64_C(1700000000000000000);
    s.mono_ns = INT64_C(120000000000);
    s.clock_trust = CAPTURE_CLOCK_TRUSTED;
    s.reinit_scheduled = 1;
    s.tick = 12;
    c = log_sample(&s);
    assert(c.result == 0 && strcmp(c.text, expected) == 0);
    assert(strstr(c.text, " value=0 ") == NULL);
}

/*
 * test_failure_details: an init failure carries errno and its text; a
 * short transfer carries both lengths; unavailable clocks print none and
 * their errno; an unknown trust prints its errno.
 */
static void test_failure_details(void)
{
    capture_sample s = light_valid();
    captured c;
    char want[256];

    s.phase = CAPTURE_PHASE_INIT;
    s.included_init = 0;
    s.valid = 0;
    s.has_raw_count = 0;
    s.stage = BH1750_STAGE_POWER_ON;
    s.stage_name = "POWER_ON";
    s.io.status = I2C_STATUS_OS_ERROR;
    s.io.os_errno = EREMOTEIO;
    s.wall_ns = 0;
    s.wall_errno = EINVAL;
    s.clock_trust = CAPTURE_CLOCK_TRUST_UNKNOWN;
    s.clock_trust_errno = EPERM;
    c = log_sample(&s);
    assert(c.result == 0);
    assert(strstr(c.text, " phase=INIT init=0 valid=0 value=none "
                          "unit=millilux raw_count=none stage=POWER_ON "
                          "i2c=OS_ERROR errno=121 ") != NULL);
    assert(snprintf(want, sizeof want, " errno_text=\"%s\" ",
                    strerror(EREMOTEIO)) < (int)sizeof want);
    assert(strstr(c.text, want) != NULL);
    assert(strstr(c.text, " wall_ns=none mono_ns=5180000000 ") != NULL);
    assert(strstr(c.text, " clock_trust=UNKNOWN ") != NULL);
    assert(strstr(c.text, " wall_errno=22 ") != NULL);
    assert(strstr(c.text, " trust_errno=1\n") != NULL);
    assert(strstr(c.text, "requested_len") == NULL);

    s = light_valid();
    s.valid = 0;
    s.has_raw_count = 0;
    s.stage = BH1750_STAGE_READ;
    s.stage_name = "READ";
    s.io.status = I2C_STATUS_SHORT_TRANSFER;
    s.io.requested_len = 2;
    s.io.transferred_len = 1;
    s.mono_ns = 0;
    s.mono_errno = EOVERFLOW;
    c = log_sample(&s);
    assert(strstr(c.text, " stage=READ i2c=SHORT_TRANSFER errno=0 "
                          "requested_len=2 transferred_len=1 "
                          "rejected_value=none ") != NULL);
    assert(strstr(c.text, " mono_ns=none ") != NULL);
    assert(snprintf(want, sizeof want, " mono_errno=%d\n", EOVERFLOW) <
           (int)sizeof want);
    assert(strstr(c.text, want) != NULL);
    assert(strstr(c.text, "errno_text") == NULL);
}

/*
 * test_unexpected_values: out-of-range enums print UNKNOWN or INVALID,
 * never a real unit, phase or trust state, and a missing stage name
 * prints stage=none.
 */
static void test_unexpected_values(void)
{
    capture_sample s = light_valid();
    captured c;

    s.unit = (capture_unit)9;
    s.phase = (capture_phase)9;
    s.clock_trust = (capture_clock_trust)9;
    s.stage_name = NULL;
    c = log_sample(&s);
    assert(c.result == 0);
    assert(strstr(c.text, " phase=UNKNOWN ") != NULL);
    assert(strstr(c.text, " unit=UNKNOWN ") != NULL);
    assert(strstr(c.text, " stage=none ") != NULL);
    assert(strstr(c.text, " clock_trust=INVALID ") != NULL);
    assert(strstr(c.text, "trust_errno") == NULL);
    assert(strstr(c.text, "raw_counts") == NULL);
    assert(strstr(c.text, "millilux") == NULL);
}

/* test_heartbeat_line: the whole line for a degraded heartbeat. */
static void test_heartbeat_line(void)
{
    static const char expected[] =
        "SIMULATED sensor=soil event=heartbeat tick=7 "
        "scheduled_mono_ns=75000000000 state=DEGRADED last_seq=2 "
        "consecutive_failures=2 attempts=2 failures=2 missed_ticks=1 "
        "next_attempt_tick=12 initialized=0\n";
    capture_heartbeat h;
    FILE *f = open_tmp();
    char text[OUT_MAX];

    memset(&h, 0, sizeof h);
    h.sensor = CAPTURE_SENSOR_SOIL;
    h.is_simulated = 1;
    h.state = CAPTURE_STATE_DEGRADED;
    h.tick = 7;
    h.scheduled_mono_ns = INT64_C(75000000000);
    h.last_sequence = 2;
    h.consecutive_failures = 2;
    h.attempts = 2;
    h.failures = 2;
    h.missed_ticks = 1;
    h.next_attempt_tick = 12;
    assert(capture_log_heartbeat(f, &h) == 0);
    read_back(f, text, sizeof text);
    assert(strcmp(text, expected) == 0);
}

/*
 * test_exit_lines: a clean stop after a successful close, a failed open
 * (no close attempted), and a sink failure followed by a failed close,
 * where both errors appear.
 */
static void test_exit_lines(void)
{
    capture_outcome o;
    FILE *f;
    char text[OUT_MAX];

    memset(&o, 0, sizeof o);
    o.exit = CAPTURE_EXIT_STOPPED;
    o.close_attempted = 1;
    f = open_tmp();
    assert(capture_log_exit(f, CAPTURE_SENSOR_LIGHT, 0, &o) == 0);
    read_back(f, text, sizeof text);
    assert(strcmp(text, "LIVE sensor=light event=exit result=STOPPED "
                        "errno=0 open=OK open_errno=0 cleanup_errno=0 "
                        "close=OK close_errno=0\n") == 0);

    /* No open was attempted (USAGE, or a clock or sink failure first):
     * the zeroed open result must not read as a successful open. */
    memset(&o, 0, sizeof o);
    o.exit = CAPTURE_EXIT_USAGE;
    f = open_tmp();
    assert(capture_log_exit(f, CAPTURE_SENSOR_LIGHT, 0, &o) == 0);
    read_back(f, text, sizeof text);
    assert(strcmp(text, "LIVE sensor=light event=exit result=USAGE "
                        "errno=0 open=not_attempted open_errno=none "
                        "cleanup_errno=none close=not_attempted "
                        "close_errno=none\n") == 0);

    memset(&o, 0, sizeof o);
    o.exit = CAPTURE_EXIT_CLOCK_FAILED;
    o.exit_errno = EINVAL;
    f = open_tmp();
    assert(capture_log_exit(f, CAPTURE_SENSOR_SOIL, 1, &o) == 0);
    read_back(f, text, sizeof text);
    assert(strstr(text, " result=CLOCK_FAILED errno=22 errno_text=\"") !=
           NULL);
    assert(strstr(text, " open=not_attempted ") != NULL);

    memset(&o, 0, sizeof o);
    o.exit = CAPTURE_EXIT_OPEN_FAILED;
    o.exit_errno = ENOENT;
    o.open_io.status = I2C_STATUS_OS_ERROR;
    o.open_io.os_errno = ENOENT;
    o.open_cleanup_errno = EIO;
    f = open_tmp();
    assert(capture_log_exit(f, CAPTURE_SENSOR_SOIL, 1, &o) == 0);
    read_back(f, text, sizeof text);
    assert(strstr(text, "SIMULATED sensor=soil event=exit "
                        "result=OPEN_FAILED errno=2 errno_text=\"") == text);
    assert(strstr(text, " open=OS_ERROR open_errno=2 cleanup_errno=5 "
                        "close=not_attempted close_errno=none\n") != NULL);

    memset(&o, 0, sizeof o);
    o.exit = CAPTURE_EXIT_SINK_FAILED;
    o.exit_errno = EPIPE;
    o.close_attempted = 1;
    o.close_io.status = I2C_STATUS_OS_ERROR;
    o.close_io.os_errno = EIO;
    f = open_tmp();
    assert(capture_log_exit(f, CAPTURE_SENSOR_LIGHT, 0, &o) == 0);
    read_back(f, text, sizeof text);
    assert(strstr(text, " result=SINK_FAILED errno=32 errno_text=") != NULL);
    assert(strstr(text, " open=OK ") != NULL);   /* close was attempted */
    assert(strstr(text, " close=OS_ERROR close_errno=5\n") != NULL);
}

/*
 * test_write_failure: a stream that cannot be written reports EIO, both
 * unbuffered and with default (full) buffering, where the error only
 * shows when the line is flushed.
 */
static void test_write_failure(void)
{
    capture_sample b = light_valid();
    FILE *buffered = fopen("/dev/full", "w");

    assert(buffered != NULL);
    assert(capture_log_sample(buffered, &b) == EIO);
    (void)fclose(buffered);

    capture_sample s = light_valid();
    capture_heartbeat h;
    capture_outcome o;
    FILE *full = fopen("/dev/full", "w");

    assert(full != NULL);
    assert(setvbuf(full, NULL, _IONBF, 0) == 0);
    memset(&h, 0, sizeof h);
    memset(&o, 0, sizeof o);
    assert(capture_log_sample(full, &s) == EIO);
    clearerr(full);
    assert(capture_log_heartbeat(full, &h) == EIO);
    clearerr(full);
    assert(capture_log_exit(full, CAPTURE_SENSOR_LIGHT, 0, &o) == EIO);
    (void)fclose(full);
}

/* test_names: sensor names, including an out-of-range value. */
static void test_names(void)
{
    assert(strcmp(capture_sensor_name(CAPTURE_SENSOR_LIGHT), "light") == 0);
    assert(strcmp(capture_sensor_name(CAPTURE_SENSOR_SOIL), "soil") == 0);
    assert(strcmp(capture_sensor_name((capture_sensor)9), "unknown") == 0);
}

/* main: run every SIMULATED log-format test; abort on the first failure. */
int main(void)
{
    test_valid_light_sample();
    test_rejected_soil_sample();
    test_failure_details();
    test_unexpected_values();
    test_heartbeat_line();
    test_exit_lines();
    test_write_failure();
    test_names();

    puts("SIMULATED capture log tests passed (hand-built samples; no "
         "device, clock or bus used)");
    return 0;
}
