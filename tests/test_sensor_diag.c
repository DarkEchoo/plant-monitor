#include "sensors/i2c_mock.h"
#include "sensors/i2c_transport.h"
#include "sensors/sensor_diag.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * SIMULATED: every test runs sensor-diag against the scripted I2C mock and a
 * fake clock, through the open_device and read_clock seams. No test calls
 * i2c_linux_open, opens /dev/i2c-*, or reads a real clock, and no value here
 * is a measurement. Some tests mark a mock device "live" only to check the
 * LIVE label; those devices are still the mock.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_STEPS 16
#define OUTPUT_MAX 16384

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

/* A step script for one mock device. */
typedef struct {
    i2c_mock_step steps[MAX_STEPS];
    size_t count;
} script;

/* One fake sensor: its script, mock and injected failures. */
typedef struct {
    uint8_t address_7bit;
    script script;
    i2c_mock mock;
    int open_errno;        /* nonzero: open fails with OS_ERROR */
    int cleanup_errno;     /* reported alongside a failed open */
    int close_errno;       /* nonzero: close reports OS_ERROR */
    int device_live;       /* clear is_simulated on the mock device */
    unsigned opens;
    unsigned closes;
    i2c_status mock_close_status;  /* OK only if the script was consumed */
} fake_sensor;

/* The fake environment behind the seams. */
typedef struct {
    fake_sensor light;
    fake_sensor soil;
    unsigned open_calls;
    unsigned bus_seen;
    unsigned clock_calls;
    unsigned fail_clock_call;   /* 1-based clock call that fails; 0 none */
    int fail_clock_errno;
    unsigned bad_nsec_call;     /* 1-based clock call with tv_nsec = 1e9 */
    fake_sensor *current;       /* the sensor opened most recently */
    size_t steps_at_clock[33];  /* [n]: its mock's next_step at call n */
} fake_env;

static const i2c_device_ops *mock_ops;

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

/* script_add: append one step. */
static void script_add(script *s, i2c_mock_step step)
{
    assert(s->count < MAX_STEPS);
    s->steps[s->count++] = step;
}

/* light_init_script: Power On, then MTreg = 69 in two writes. */
static void light_init_script(script *s)
{
    script_add(s, write_step(cmd_power_on, 1));
    script_add(s, write_step(cmd_mtreg_high, 1));
    script_add(s, write_step(cmd_mtreg_low, 1));
}

/* light_measure_script: One Time H-Resolution, 180 ms, two bytes. */
static void light_measure_script(script *s, const uint8_t *bytes)
{
    script_add(s, write_step(cmd_one_time_h, 1));
    script_add(s, wait_step(180000));
    script_add(s, read_step(bytes, 2));
}

/* soil_init_script: reset, wait, select HW_ID, wait, read the ID. */
static void soil_init_script(script *s, const uint8_t *id)
{
    script_add(s, write_step(cmd_reset, sizeof cmd_reset));
    script_add(s, wait_step(500000));
    script_add(s, write_step(cmd_hw_id, sizeof cmd_hw_id));
    script_add(s, wait_step(8000));
    script_add(s, read_step(id, 1));
}

/* soil_measure_script: select touch channel 0, wait, read two bytes. */
static void soil_measure_script(script *s, const uint8_t *bytes)
{
    script_add(s, write_step(cmd_touch0, sizeof cmd_touch0));
    script_add(s, wait_step(5000));
    script_add(s, read_step(bytes, 2));
}

/* wrap_write: forward to the mock. */
static i2c_result wrap_write(void *ctx, const uint8_t *buf, size_t len)
{
    fake_sensor *fs = ctx;
    return mock_ops->write(&fs->mock, buf, len);
}

/* wrap_read: forward to the mock. */
static i2c_result wrap_read(void *ctx, uint8_t *buf, size_t len)
{
    fake_sensor *fs = ctx;
    return mock_ops->read(&fs->mock, buf, len);
}

/* wrap_wait_us: forward to the mock. */
static i2c_result wrap_wait_us(void *ctx, uint32_t duration_us)
{
    fake_sensor *fs = ctx;
    return mock_ops->wait_us(&fs->mock, duration_us);
}

/*
 * wrap_close: close the mock, remember whether its script was consumed
 * exactly, count the close, then apply any injected close failure.
 */
static i2c_result wrap_close(void *ctx)
{
    fake_sensor *fs = ctx;
    i2c_result r = mock_ops->close(&fs->mock);

    fs->closes++;
    fs->mock_close_status = r.status;
    if (fs->close_errno != 0) {
        r.status = I2C_STATUS_OS_ERROR;
        r.os_errno = fs->close_errno;
    }
    return r;
}

static const i2c_device_ops wrap_ops = {
    wrap_write,
    wrap_read,
    wrap_wait_us,
    wrap_close
};

/*
 * fake_open: open the mock for the matching address, following
 * i2c_linux_open's contract: dev untouched and *cleanup_errno reported on
 * failure, *cleanup_errno 0 otherwise.
 */
static i2c_result fake_open(void *ctx, i2c_device *dev, unsigned bus_number,
                            uint8_t address_7bit, int *cleanup_errno)
{
    fake_env *fe = ctx;
    fake_sensor *fs = NULL;
    i2c_result r = {I2C_STATUS_OK, 0, 0, 0};

    fe->open_calls++;
    fe->bus_seen = bus_number;
    assert(dev != NULL && dev->ops == NULL && cleanup_errno != NULL);
    if (address_7bit == fe->light.address_7bit) {
        fs = &fe->light;
    } else if (address_7bit == fe->soil.address_7bit) {
        fs = &fe->soil;
    }
    assert(fs != NULL);

    *cleanup_errno = 0;
    if (fs->open_errno != 0) {
        r.status = I2C_STATUS_OS_ERROR;
        r.os_errno = fs->open_errno;
        *cleanup_errno = fs->cleanup_errno;
        return r;
    }
    assert(i2c_mock_open(&fs->mock, dev, address_7bit, fs->script.steps,
                         fs->script.count) == I2C_STATUS_OK);
    mock_ops = dev->ops;
    dev->ops = &wrap_ops;
    dev->ctx = fs;
    if (fs->device_live) {
        dev->is_simulated = 0;
    }
    fs->opens++;
    fe->current = fs;
    return r;
}

/*
 * fake_clock: call n (1-based) returns 1000 + n seconds and n nanoseconds.
 * Calls must alternate wall, monotonic. Each call records how many steps
 * of the open sensor's script had run, so tests can check the clocks are
 * read after the measurement, not before it.
 */
static int fake_clock(void *ctx, sensor_diag_clock clock,
                      struct timespec *out)
{
    fake_env *fe = ctx;
    unsigned n = ++fe->clock_calls;

    assert(fe->current != NULL && n < COUNT_OF(fe->steps_at_clock));
    fe->steps_at_clock[n] = fe->current->mock.next_step;
    assert(clock == (n % 2 == 1 ? SENSOR_DIAG_CLOCK_WALL
                                : SENSOR_DIAG_CLOCK_MONOTONIC));
    if (n == fe->fail_clock_call) {
        return fe->fail_clock_errno;
    }
    out->tv_sec = (time_t)(1000 + n);
    out->tv_nsec = n == fe->bad_nsec_call ? 1000000000L : (long)n;
    return 0;
}

/* fake_env_init: both sensors at their default addresses, no scripts. */
static void fake_env_init(fake_env *fe)
{
    memset(fe, 0, sizeof *fe);
    fe->light.address_7bit = 0x23;
    fe->soil.address_7bit = 0x36;
}

/* good_scripts: both sensors initialize and measure samples times. */
static void good_scripts(fake_env *fe, unsigned samples)
{
    unsigned i;

    light_init_script(&fe->light.script);
    soil_init_script(&fe->soil.script, id_samd);
    for (i = 0; i < samples; i++) {
        light_measure_script(&fe->light.script, light_raw_12);
        soil_measure_script(&fe->soil.script, soil_500);
    }
}

/* config_for: bus 1, default addresses, samples per sensor. */
static sensor_diag_config config_for(unsigned samples)
{
    sensor_diag_config cfg = {1, 0x23, 0x36, samples};
    return cfg;
}

/* run_capture: run sensor-diag and copy its whole output into buf. */
static int run_capture(const sensor_diag_config *cfg, fake_env *fe,
                       int env_simulated, char *buf, size_t size)
{
    sensor_diag_env env = {fake_open, fake_clock, NULL, 0};
    FILE *f = tmpfile();
    size_t n;
    int rc;

    env.ctx = fe;
    env.is_simulated = env_simulated;
    assert(f != NULL);
    rc = sensor_diag_run(cfg, &env, f);
    assert(fflush(f) == 0);
    rewind(f);
    n = fread(buf, 1, size - 1, f);
    assert(!ferror(f) && n < size - 1);
    buf[n] = '\0';
    assert(fclose(f) == 0);
    return rc;
}

/*
 * assert_cleaned_up: every opened device was closed exactly once, and each
 * mock script was consumed exactly, so nothing ran after a failure and
 * nothing was skipped.
 */
static void assert_cleaned_up(const fake_env *fe)
{
    assert(fe->light.closes == fe->light.opens);
    assert(fe->soil.closes == fe->soil.opens);
    if (fe->light.opens > 0) {
        assert(fe->light.mock_close_status == I2C_STATUS_OK);
    }
    if (fe->soil.opens > 0) {
        assert(fe->soil.mock_close_status == I2C_STATUS_OK);
    }
}

/* count_of: occurrences of needle in haystack. */
static unsigned count_of(const char *haystack, const char *needle)
{
    unsigned n = 0;
    const char *p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* has: true if needle occurs in haystack. */
static int has(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

/*
 * assert_no_valid_zero: a failed reading must never look like a zero
 * value, so " value=0 " never appears.
 */
static void assert_no_valid_zero(const char *out)
{
    assert(!has(out, " value=0 "));
}

/* test_all_ok_exact_output: the whole output of a successful run. */
static void test_all_ok_exact_output(void)
{
    static const char expected[] =
        "SIMULATED event=start tool=sensor-diag "
        "output=diagnostic-not-storage-record bus=1 light_addr=0x23 "
        "soil_addr=0x36 samples=2 clock_sync=unknown "
        "timestamps=attempt_completion\n"
        "SIMULATED sensor=light event=open addr=0x23 result=OK i2c=OK "
        "errno=0 cleanup_errno=0\n"
        "SIMULATED sensor=light event=init addr=0x23 result=OK stage=NONE "
        "i2c=OK errno=0 rejected_value=none\n"
        "SIMULATED sensor=light event=measure addr=0x23 sample=1/2 "
        "wall_ns=1001000000001 mono_ns=1002000000002 clock_sync=unknown "
        "valid=1 value=10000 unit=millilux raw_count=12 stage=NONE i2c=OK "
        "errno=0 rejected_value=none\n"
        "SIMULATED sensor=light event=measure addr=0x23 sample=2/2 "
        "wall_ns=1003000000003 mono_ns=1004000000004 clock_sync=unknown "
        "valid=1 value=10000 unit=millilux raw_count=12 stage=NONE i2c=OK "
        "errno=0 rejected_value=none\n"
        "SIMULATED sensor=light event=close addr=0x23 result=OK i2c=OK "
        "errno=0\n"
        "SIMULATED sensor=soil event=open addr=0x36 result=OK i2c=OK "
        "errno=0 cleanup_errno=0\n"
        "SIMULATED sensor=soil event=init addr=0x36 result=OK stage=NONE "
        "i2c=OK errno=0 rejected_value=none\n"
        "SIMULATED sensor=soil event=measure addr=0x36 sample=1/2 "
        "wall_ns=1005000000005 mono_ns=1006000000006 clock_sync=unknown "
        "valid=1 value=500 unit=raw_counts stage=NONE i2c=OK errno=0 "
        "rejected_value=none\n"
        "SIMULATED sensor=soil event=measure addr=0x36 sample=2/2 "
        "wall_ns=1007000000007 mono_ns=1008000000008 clock_sync=unknown "
        "valid=1 value=500 unit=raw_counts stage=NONE i2c=OK errno=0 "
        "rejected_value=none\n"
        "SIMULATED sensor=soil event=close addr=0x36 result=OK i2c=OK "
        "errno=0\n"
        "SIMULATED event=summary result=PASS failures=0 "
        "valid_readings=4/4\n";
    sensor_diag_config cfg = config_for(2);
    fake_env fe;
    char out[OUTPUT_MAX];

    fake_env_init(&fe);
    good_scripts(&fe, 2);
    assert(run_capture(&cfg, &fe, 1, out, sizeof out) == SENSOR_DIAG_EXIT_OK);
    assert(strcmp(out, expected) == 0);
    assert(fe.open_calls == 2 && fe.bus_seen == 1);
    assert(fe.clock_calls == 8);
    /* light: 3 init steps + 3 per sample; soil: 5 init + 3 per sample */
    assert(fe.steps_at_clock[1] == 6 && fe.steps_at_clock[2] == 6);
    assert(fe.steps_at_clock[3] == 9 && fe.steps_at_clock[4] == 9);
    assert(fe.steps_at_clock[5] == 8 && fe.steps_at_clock[6] == 8);
    assert(fe.steps_at_clock[7] == 11 && fe.steps_at_clock[8] == 11);
    assert_cleaned_up(&fe);
}

/* check_labels: the start line, then every other line, carry the labels. */
static void check_labels(const char *out, const char *start_label,
                         const char *other_label)
{
    const char *line = out;
    int first = 1;

    while (*line != '\0') {
        const char *label = first ? start_label : other_label;
        const char *end = strchr(line, '\n');

        assert(end != NULL);
        assert(strncmp(line, label, strlen(label)) == 0);
        assert(line[strlen(label)] == ' ');
        line = end + 1;
        first = 0;
    }
    assert(!first);
}

/*
 * test_simulated_label_from_env_or_device: a line is SIMULATED if either
 * the environment or the device is simulated. Only an all-live run prints
 * LIVE everywhere.
 */
static void test_simulated_label_from_env_or_device(void)
{
    static const struct {
        int env_simulated;
        int device_live;
        const char *start_label;
        const char *other_label;
    } cases[] = {
        {1, 0, "SIMULATED", "SIMULATED"},
        {0, 0, "LIVE", "SIMULATED"},
        {1, 1, "SIMULATED", "SIMULATED"},
        {0, 1, "LIVE", "LIVE"},
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        sensor_diag_config cfg = config_for(1);
        fake_env fe;
        char out[OUTPUT_MAX];

        fake_env_init(&fe);
        good_scripts(&fe, 1);
        fe.light.device_live = cases[i].device_live;
        fe.soil.device_live = cases[i].device_live;
        assert(run_capture(&cfg, &fe, cases[i].env_simulated, out,
                           sizeof out) == SENSOR_DIAG_EXIT_OK);
        check_labels(out, cases[i].start_label, cases[i].other_label);
        assert_cleaned_up(&fe);
    }
}

/*
 * test_light_open_failure_soil_still_checked: a failed open prints the
 * error, skips that sensor's init, measurements and close, and the soil
 * sensor is still checked.
 */
static void test_light_open_failure_soil_still_checked(void)
{
    sensor_diag_config cfg = config_for(1);
    fake_env fe;
    char out[OUTPUT_MAX];

    fake_env_init(&fe);
    fe.light.open_errno = ENOENT;
    soil_init_script(&fe.soil.script, id_samd);
    soil_measure_script(&fe.soil.script, soil_500);
    assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
           SENSOR_DIAG_EXIT_FAILURE);
    assert(has(out, "SIMULATED sensor=light event=open addr=0x23 "
                    "result=FAIL i2c=OS_ERROR errno=2 "));
    assert(has(out, " cleanup_errno=0\n"));
    assert(!has(out, "sensor=light event=init"));
    assert(!has(out, "sensor=light event=measure"));
    assert(!has(out, "sensor=light event=close"));
    assert(has(out, "sensor=soil event=measure addr=0x36 sample=1/1 "));
    assert(count_of(out, "valid=1 ") == 1);
    assert(has(out, "event=summary result=FAIL failures=1 "
                    "valid_readings=1/2\n"));
    assert(fe.open_calls == 2 && fe.light.opens == 0);
    assert_cleaned_up(&fe);
}

/* test_open_failure_reports_cleanup_errno: both errnos are printed. */
static void test_open_failure_reports_cleanup_errno(void)
{
    sensor_diag_config cfg = config_for(1);
    fake_env fe;
    char out[OUTPUT_MAX];

    fake_env_init(&fe);
    fe.soil.open_errno = EBUSY;
    fe.soil.cleanup_errno = EIO;
    light_init_script(&fe.light.script);
    light_measure_script(&fe.light.script, light_raw_12);
    assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
           SENSOR_DIAG_EXIT_FAILURE);
    assert(has(out, "sensor=soil event=open addr=0x36 result=FAIL "
                    "i2c=OS_ERROR errno=16 "));
    assert(has(out, " cleanup_errno=5\n"));
    assert(has(out, "sensor=light event=measure addr=0x23 sample=1/1 "));
    assert(has(out, "result=FAIL failures=1 valid_readings=1/2\n"));
    assert_cleaned_up(&fe);
}

/*
 * test_light_init_failure_each_step: a failure at each init step (the later
 * ones after earlier steps succeeded) skips measurements, still closes the
 * device, and leaves the soil check intact.
 */
static void test_light_init_failure_each_step(void)
{
    static const char *const stage_lines[] = {
        "sensor=light event=init addr=0x23 result=FAIL stage=POWER_ON "
        "i2c=OS_ERROR errno=121 ",
        "sensor=light event=init addr=0x23 result=FAIL stage=MTREG_HIGH "
        "i2c=OS_ERROR errno=121 ",
        "sensor=light event=init addr=0x23 result=FAIL stage=MTREG_LOW "
        "i2c=OS_ERROR errno=121 ",
    };
    size_t k;

    for (k = 0; k < COUNT_OF(stage_lines); k++) {
        sensor_diag_config cfg = config_for(2);
        fake_env fe;
        char out[OUTPUT_MAX];
        unsigned i;

        fake_env_init(&fe);
        light_init_script(&fe.light.script);
        fe.light.script.steps[k].fail_errno = EREMOTEIO;
        fe.light.script.count = k + 1;
        soil_init_script(&fe.soil.script, id_samd);
        for (i = 0; i < 2; i++) {
            soil_measure_script(&fe.soil.script, soil_500);
        }
        assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
               SENSOR_DIAG_EXIT_FAILURE);
        assert(has(out, stage_lines[k]));
        assert(has(out, " rejected_value=none measurements=skipped\n"));
        assert(!has(out, "sensor=light event=measure"));
        assert(has(out, "sensor=light event=close addr=0x23 result=OK "));
        assert(count_of(out, "sensor=soil event=measure") == 2);
        assert(has(out, "result=FAIL failures=1 valid_readings=2/4\n"));
        assert(fe.clock_calls == 4);
        assert_cleaned_up(&fe);
    }
}

/*
 * test_soil_init_rejections: a wrong hardware ID is a failure with
 * i2c=OK and the ID as the rejected value; a reset write failure reports
 * its errno. The light check before it is unaffected.
 */
static void test_soil_init_rejections(void)
{
    int variant;

    for (variant = 0; variant < 2; variant++) {
        sensor_diag_config cfg = config_for(1);
        fake_env fe;
        char out[OUTPUT_MAX];

        fake_env_init(&fe);
        light_init_script(&fe.light.script);
        light_measure_script(&fe.light.script, light_raw_12);
        if (variant == 0) {
            soil_init_script(&fe.soil.script, id_other);
        } else {
            soil_init_script(&fe.soil.script, id_samd);
            fe.soil.script.steps[0].fail_errno = ENXIO;
            fe.soil.script.count = 1;
        }
        assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
               SENSOR_DIAG_EXIT_FAILURE);
        if (variant == 0) {
            assert(has(out, "sensor=soil event=init addr=0x36 result=FAIL "
                            "stage=HW_ID i2c=OK errno=0 rejected_value=84 "
                            "measurements=skipped\n"));
        } else {
            assert(has(out, "sensor=soil event=init addr=0x36 result=FAIL "
                            "stage=RESET i2c=OS_ERROR errno=6 "));
            assert(has(out, " rejected_value=none measurements=skipped\n"));
        }
        assert(!has(out, "sensor=soil event=measure"));
        assert(has(out, "sensor=soil event=close addr=0x36 result=OK "));
        assert(has(out, "sensor=light event=measure addr=0x23 sample=1/1 "
                        "wall_ns=1001000000001 mono_ns=1002000000002 "
                        "clock_sync=unknown valid=1 value=10000 "));
        assert(has(out, "result=FAIL failures=1 valid_readings=1/2\n"));
        assert_cleaned_up(&fe);
    }
}

/*
 * test_soil_stage_failures: an OS error at each remaining soil init step
 * and at each measure step is printed with its stage and no rejected
 * value; a measure failure prints valid=0 value=none.
 */
static void test_soil_stage_failures(void)
{
    static const struct {
        int in_measure;
        size_t step;
        const char *stage;
    } cases[] = {
        {0, 1, "RESET_WAIT"},
        {0, 2, "HW_ID_SELECT"},
        {0, 3, "HW_ID_WAIT"},
        {0, 4, "HW_ID_READ"},
        {1, 0, "MEASURE_SELECT"},
        {1, 1, "MEASURE_WAIT"},
        {1, 2, "MEASURE_READ"},
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        sensor_diag_config cfg = config_for(1);
        fake_env fe;
        char out[OUTPUT_MAX];
        char expect[256];
        size_t k = cases[i].step + (cases[i].in_measure ? 5 : 0);

        fake_env_init(&fe);
        light_init_script(&fe.light.script);
        light_measure_script(&fe.light.script, light_raw_12);
        soil_init_script(&fe.soil.script, id_samd);
        soil_measure_script(&fe.soil.script, soil_500);
        fe.soil.script.steps[k].fail_errno = EREMOTEIO;
        fe.soil.script.count = k + 1;
        assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
               SENSOR_DIAG_EXIT_FAILURE);
        if (cases[i].in_measure) {
            assert(snprintf(expect, sizeof expect,
                            "clock_sync=unknown valid=0 value=none "
                            "unit=raw_counts stage=%s i2c=OS_ERROR "
                            "errno=121 ", cases[i].stage) <
                   (int)sizeof expect);
        } else {
            assert(snprintf(expect, sizeof expect,
                            "sensor=soil event=init addr=0x36 result=FAIL "
                            "stage=%s i2c=OS_ERROR errno=121 ",
                            cases[i].stage) < (int)sizeof expect);
            assert(!has(out, "sensor=soil event=measure"));
        }
        assert(has(out, expect));
        /* all but start, summary, 2 open and 2 close lines carry it */
        assert(count_of(out, " rejected_value=none") ==
               count_of(out, "\n") - 6);
        assert(has(out, "sensor=soil event=close addr=0x36 result=OK "));
        assert(has(out, "result=FAIL failures=1 valid_readings=1/2\n"));
        assert_no_valid_zero(out);
        assert_cleaned_up(&fe);
    }
}

/*
 * test_soil_range_rejection_then_success: a rejected value prints valid=0,
 * value=none and the rejected counts, still gets timestamps, and the next
 * sample is still attempted.
 */
static void test_soil_range_rejection_then_success(void)
{
    sensor_diag_config cfg = config_for(2);
    fake_env fe;
    char out[OUTPUT_MAX];

    fake_env_init(&fe);
    light_init_script(&fe.light.script);
    light_measure_script(&fe.light.script, light_raw_12);
    light_measure_script(&fe.light.script, light_raw_12);
    soil_init_script(&fe.soil.script, id_samd);
    soil_measure_script(&fe.soil.script, soil_5000);
    soil_measure_script(&fe.soil.script, soil_500);
    assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
           SENSOR_DIAG_EXIT_FAILURE);
    assert(has(out, "SIMULATED sensor=soil event=measure addr=0x36 "
                    "sample=1/2 wall_ns=1005000000005 "
                    "mono_ns=1006000000006 clock_sync=unknown valid=0 "
                    "value=none unit=raw_counts stage=RANGE i2c=OK errno=0 "
                    "rejected_value=5000\n"));
    assert(has(out, "SIMULATED sensor=soil event=measure addr=0x36 "
                    "sample=2/2 wall_ns=1007000000007 "
                    "mono_ns=1008000000008 clock_sync=unknown valid=1 "
                    "value=500 unit=raw_counts "));
    assert(!has(out, " value=5000"));  /* only as rejected_value=5000 */
    assert(has(out, "result=FAIL failures=1 valid_readings=3/4\n"));
    assert(fe.clock_calls == 8);
    assert(fe.steps_at_clock[5] == 8 && fe.steps_at_clock[6] == 8);
    assert_no_valid_zero(out);
    assert_cleaned_up(&fe);
}

/*
 * test_light_measure_failure_each_step: an OS error at each measurement
 * step, and a short read, prints valid=0 with no value, keeps the
 * timestamps of the failed attempt, and reports the stage and errno.
 */
static void test_light_measure_failure_each_step(void)
{
    static const struct {
        size_t step;
        int short_read;
        const char *tail;
    } cases[] = {
        {0, 0, "stage=MEASURE_COMMAND i2c=OS_ERROR errno=121 "},
        {1, 0, "stage=WAIT i2c=OS_ERROR errno=4 "},
        {2, 0, "stage=READ i2c=OS_ERROR errno=121 "},
        {2, 1, "stage=READ i2c=SHORT_TRANSFER errno=0 rejected_value=none\n"},
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        sensor_diag_config cfg = config_for(1);
        fake_env fe;
        char out[OUTPUT_MAX];
        char line[256];
        i2c_mock_step *step;

        fake_env_init(&fe);
        light_init_script(&fe.light.script);
        light_measure_script(&fe.light.script, light_raw_12);
        step = &fe.light.script.steps[3 + cases[i].step];
        if (cases[i].short_read) {
            step->transfer_len = 1;
        } else {
            step->fail_errno = cases[i].step == 1 ? EINTR : EREMOTEIO;
        }
        fe.light.script.count = 3 + cases[i].step + 1;
        soil_init_script(&fe.soil.script, id_samd);
        soil_measure_script(&fe.soil.script, soil_500);
        assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
               SENSOR_DIAG_EXIT_FAILURE);
        assert(snprintf(line, sizeof line,
                        "SIMULATED sensor=light event=measure addr=0x23 "
                        "sample=1/1 wall_ns=1001000000001 "
                        "mono_ns=1002000000002 clock_sync=unknown valid=0 "
                        "value=none unit=millilux raw_count=none %s",
                        cases[i].tail) < (int)sizeof line);
        assert(has(out, line));
        /* the failed attempt's last step ran before its timestamps */
        assert(fe.steps_at_clock[1] == fe.light.script.count);
        assert(fe.steps_at_clock[2] == fe.light.script.count);
        assert(has(out, "sensor=light event=close addr=0x23 result=OK "));
        assert(has(out, "sensor=soil event=measure addr=0x36 sample=1/1 "
                        "wall_ns=1003000000003 mono_ns=1004000000004 "
                        "clock_sync=unknown valid=1 value=500 "));
        assert(has(out, "result=FAIL failures=1 valid_readings=1/2\n"));
        assert_no_valid_zero(out);
        assert_cleaned_up(&fe);
    }
}

/* test_close_failure_reported: a failed close fails the run. */
static void test_close_failure_reported(void)
{
    sensor_diag_config cfg = config_for(1);
    fake_env fe;
    char out[OUTPUT_MAX];

    fake_env_init(&fe);
    good_scripts(&fe, 1);
    fe.light.close_errno = EIO;
    assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
           SENSOR_DIAG_EXIT_FAILURE);
    assert(has(out, "SIMULATED sensor=light event=close addr=0x23 "
                    "result=FAIL i2c=OS_ERROR errno=5 "));
    assert(has(out, "sensor=soil event=close addr=0x36 result=OK "));
    assert(has(out, "result=FAIL failures=1 valid_readings=2/2\n"));
    assert_cleaned_up(&fe);
}

/*
 * test_clock_failures: a failed clock call prints none and
 * <clock>_clock_errno; an unusable clock value prints none and
 * <clock>_clock_value_errno, so the two are told apart. Either fails the
 * run without changing the reading's validity, the driver's result alone.
 */
static void test_clock_failures(void)
{
    int variant;

    for (variant = 0; variant < 2; variant++) {
        sensor_diag_config cfg = config_for(1);
        fake_env fe;
        char out[OUTPUT_MAX];

        fake_env_init(&fe);
        good_scripts(&fe, 1);
        if (variant == 0) {
            fe.fail_clock_call = 1;
            fe.fail_clock_errno = EINVAL;
        } else {
            fe.bad_nsec_call = 2;
        }
        assert(run_capture(&cfg, &fe, 1, out, sizeof out) ==
               SENSOR_DIAG_EXIT_FAILURE);
        if (variant == 0) {
            assert(has(out, "sensor=light event=measure addr=0x23 "
                            "sample=1/1 wall_ns=none "
                            "mono_ns=1002000000002 clock_sync=unknown "
                            "valid=1 value=10000 "));
            assert(has(out, " wall_clock_errno=22\n"));
            assert(!has(out, "wall_clock_value_errno="));
        } else {
            assert(has(out, "sensor=light event=measure addr=0x23 "
                            "sample=1/1 wall_ns=1001000000001 "
                            "mono_ns=none clock_sync=unknown valid=1 "
                            "value=10000 "));
            assert(has(out, " mono_clock_value_errno=22\n"));
            assert(!has(out, "mono_clock_errno="));
        }
        assert(has(out, "result=FAIL failures=1 valid_readings=2/2\n"));
        assert(fe.clock_calls == 4);
        assert_cleaned_up(&fe);
    }
}

/*
 * test_output_write_failure: if the output cannot be written (/dev/full,
 * not an I2C device), the run fails, and the devices are still closed.
 */
static void test_output_write_failure(void)
{
    sensor_diag_config cfg = config_for(1);
    sensor_diag_env env = {fake_open, fake_clock, NULL, 1};
    fake_env fe;
    FILE *full = fopen("/dev/full", "w");

    assert(full != NULL);
    fake_env_init(&fe);
    good_scripts(&fe, 1);
    env.ctx = &fe;
    assert(sensor_diag_run(&cfg, &env, full) == SENSOR_DIAG_EXIT_FAILURE);
    assert(fe.open_calls == 2);
    assert_cleaned_up(&fe);
    (void)fclose(full);  /* fails again on /dev/full; nothing to check */
}

/*
 * test_unusable_run_arguments: a bad config or environment is refused
 * before anything is opened or printed.
 */
static void test_unusable_run_arguments(void)
{
    sensor_diag_config bad[6];
    sensor_diag_config good = config_for(1);
    sensor_diag_env env = {fake_open, fake_clock, NULL, 1};
    sensor_diag_env no_open = {NULL, fake_clock, NULL, 1};
    sensor_diag_env no_clock = {fake_open, NULL, NULL, 1};
    fake_env fe;
    FILE *f = tmpfile();
    size_t i;

    assert(f != NULL);
    fake_env_init(&fe);
    env.ctx = &fe;
    no_open.ctx = &fe;
    no_clock.ctx = &fe;
    for (i = 0; i < COUNT_OF(bad); i++) {
        bad[i] = good;
    }
    bad[0].samples = 0;
    bad[1].samples = SENSOR_DIAG_SAMPLES_MAX + 1;
    bad[2].light_address_7bit = 0x24;
    bad[3].soil_address_7bit = 0x35;
    bad[4].soil_address_7bit = 0x3a;
    bad[5].light_address_7bit = 0x36;

    for (i = 0; i < COUNT_OF(bad); i++) {
        assert(sensor_diag_run(&bad[i], &env, f) == SENSOR_DIAG_EXIT_USAGE);
    }
    assert(sensor_diag_run(NULL, &env, f) == SENSOR_DIAG_EXIT_USAGE);
    assert(sensor_diag_run(&good, NULL, f) == SENSOR_DIAG_EXIT_USAGE);
    assert(sensor_diag_run(&good, &env, NULL) == SENSOR_DIAG_EXIT_USAGE);
    assert(sensor_diag_run(&good, &no_open, f) == SENSOR_DIAG_EXIT_USAGE);
    assert(sensor_diag_run(&good, &no_clock, f) == SENSOR_DIAG_EXIT_USAGE);
    assert(fe.open_calls == 0 && fe.clock_calls == 0);
    assert(ftell(f) == 0);
    assert(fclose(f) == 0);
}

/* parse: run the parser on a NULL-terminated argument list. */
static sensor_diag_args parse(char *const *args, sensor_diag_config *cfg,
                              long *err_bytes)
{
    char *argv[16];
    int argc = 0;
    FILE *err = tmpfile();
    sensor_diag_args r;

    assert(err != NULL);
    argv[argc++] = "sensor-diag";
    while (*args != NULL) {
        assert(argc < (int)COUNT_OF(argv) - 1);
        argv[argc++] = *args++;
    }
    argv[argc] = NULL;
    r = sensor_diag_parse_args(argc, argv, cfg, err);
    assert(fflush(err) == 0);
    *err_bytes = ftell(err);
    assert(fclose(err) == 0);
    return r;
}

/* test_parse_accepts: valid argument lists and the values they give. */
static void test_parse_accepts(void)
{
    static char *const minimal[] = {"--bus", "1", NULL};
    static char *const all[] = {
        "--samples", "3", "--soil-addr", "0x39", "--bus", "0",
        "--light-addr", "0x5c", NULL
    };
    static char *const decimal_addr[] = {
        "--bus", "7", "--light-addr", "35", "--soil-addr", "54", NULL
    };
    static char *const max_bus[] = {
        "--bus", "4294967295", "--samples", "100", NULL
    };
    static char *const help_late[] = {"--bus", "1", "--help", NULL};
    static char *const help_only[] = {"--help", NULL};
    sensor_diag_config cfg;
    long err_bytes;

    assert(UINT_MAX == 4294967295u);
    assert(parse(minimal, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_RUN);
    assert(cfg.bus_number == 1 && cfg.light_address_7bit == 0x23 &&
           cfg.soil_address_7bit == 0x36 &&
           cfg.samples == SENSOR_DIAG_SAMPLES_DEFAULT && err_bytes == 0);

    assert(parse(all, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_RUN);
    assert(cfg.bus_number == 0 && cfg.light_address_7bit == 0x5c &&
           cfg.soil_address_7bit == 0x39 && cfg.samples == 3);

    assert(parse(decimal_addr, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_RUN);
    assert(cfg.bus_number == 7 && cfg.light_address_7bit == 0x23 &&
           cfg.soil_address_7bit == 0x36);

    assert(parse(max_bus, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_RUN);
    assert(cfg.bus_number == UINT_MAX && cfg.samples == 100);

    assert(parse(help_late, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_HELP);
    assert(parse(help_only, &cfg, &err_bytes) == SENSOR_DIAG_ARGS_HELP);
}

/*
 * test_parse_rejects: each bad list is an error, prints a message and
 * leaves *cfg untouched. A missing --bus is always an error: there is no
 * default bus.
 */
static void test_parse_rejects(void)
{
    static char *const none[] = {NULL};
    static char *const no_bus[] = {"--samples", "2", NULL};
    static char *const no_value[] = {"--bus", NULL};
    static char *const empty[] = {"--bus", "", NULL};
    static char *const letters[] = {"--bus", "abc", NULL};
    static char *const trailing[] = {"--bus", "1x", NULL};
    static char *const hex_bus[] = {"--bus", "0x1", NULL};
    static char *const negative[] = {"--bus", "-1", NULL};
    static char *const plus[] = {"--bus", "+1", NULL};
    static char *const space[] = {"--bus", " 1", NULL};
    static char *const too_big[] = {"--bus", "4294967296", NULL};
    static char *const huge[] = {"--bus", "99999999999999999999999", NULL};
    static char *const twice[] = {"--bus", "1", "--bus", "2", NULL};
    static char *const zero_samples[] = {"--bus", "1", "--samples", "0", NULL};
    static char *const many_samples[] = {
        "--bus", "1", "--samples", "101", NULL
    };
    static char *const light_bad[] = {
        "--bus", "1", "--light-addr", "0x24", NULL
    };
    static char *const soil_low[] = {
        "--bus", "1", "--soil-addr", "0x35", NULL
    };
    static char *const soil_high[] = {
        "--bus", "1", "--soil-addr", "0x3a", NULL
    };
    static char *const soil_wide[] = {
        "--bus", "1", "--soil-addr", "0x136", NULL
    };
    static char *const addr_twice[] = {
        "--bus", "1", "--soil-addr", "0x36", "--soil-addr", "0x37", NULL
    };
    static char *const unknown[] = {"--bus", "1", "--frobnicate", NULL};
    static char *const equals[] = {"--bus=1", NULL};
    static char *const positional[] = {"--bus", "1", "extra", NULL};
    static char *const *const cases[] = {
        none, no_bus, no_value, empty, letters, trailing, hex_bus, negative,
        plus, space, too_big, huge, twice, zero_samples, many_samples,
        light_bad, soil_low, soil_high, soil_wide, addr_twice, unknown,
        equals, positional
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        sensor_diag_config cfg = {12345, 0x7f, 0x7f, 999};
        long err_bytes;

        assert(parse(cases[i], &cfg, &err_bytes) == SENSOR_DIAG_ARGS_ERROR);
        assert(err_bytes > 0);
        assert(cfg.bus_number == 12345 && cfg.light_address_7bit == 0x7f &&
               cfg.soil_address_7bit == 0x7f && cfg.samples == 999);
    }
}

/* test_usage_text: the help states the limits of a short check. */
static void test_usage_text(void)
{
    char out[OUTPUT_MAX];
    FILE *f = tmpfile();
    size_t n;

    assert(f != NULL);
    sensor_diag_usage(f, "sensor-diag");
    rewind(f);
    n = fread(out, 1, sizeof out - 1, f);
    assert(!ferror(f) && n < sizeof out - 1);
    out[n] = '\0';
    assert(fclose(f) == 0);
    assert(has(out, "usage: sensor-diag --bus N"));
    assert(has(out, "--bus is required"));
    assert(has(out, "basic communication and responsiveness"));
    assert(has(out, "timing reliability"));
    assert(has(out, "firmware behavior"));
    assert(has(out, "full valid measurement range"));
    assert(has(out, "not the storage record format"));
}

/* test_timespec_to_ns: conversion, its bounds and its refusals. */
static void test_timespec_to_ns(void)
{
    struct timespec ts;
    int64_t ns = 77;

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == 0 && ns == 0);

    ts.tv_sec = 1700000000;
    ts.tv_nsec = 123456789;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == 0 &&
           ns == INT64_C(1700000000123456789));

    /* the largest 32-bit time_t (2038-01-19) fits on every target */
    ts.tv_sec = (time_t)INT32_MAX;
    ts.tv_nsec = 999999999L;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == 0 &&
           ns == INT64_C(2147483647999999999));

    /*
     * The int64_t limits need a 64-bit time_t; a 32-bit one (older 32-bit
     * Raspberry Pi OS) cannot hold these tv_sec values, and its readings
     * can never reach the limit.
     */
    if (sizeof(time_t) >= 8) {
        ts.tv_sec = (time_t)INT64_C(9223372036);
        ts.tv_nsec = 854775807L;
        assert(sensor_diag_timespec_to_ns(&ts, &ns) == 0 &&
               ns == INT64_MAX);

        ns = 77;
        ts.tv_nsec = 854775808L;
        assert(sensor_diag_timespec_to_ns(&ts, &ns) == ERANGE && ns == 77);

        ts.tv_sec = (time_t)INT64_C(9223372037);
        ts.tv_nsec = 0;
        assert(sensor_diag_timespec_to_ns(&ts, &ns) == ERANGE && ns == 77);
    }
    ns = 77;

    ts.tv_sec = -1;
    ts.tv_nsec = 0;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == ERANGE && ns == 77);

    ts.tv_sec = 1;
    ts.tv_nsec = 1000000000L;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == EINVAL && ns == 77);

    ts.tv_nsec = -1;
    assert(sensor_diag_timespec_to_ns(&ts, &ns) == EINVAL && ns == 77);

    assert(sensor_diag_timespec_to_ns(NULL, &ns) == EINVAL);
    assert(sensor_diag_timespec_to_ns(&ts, NULL) == EINVAL);
}

/* main: run every SIMULATED sensor-diag test; abort on the first failure. */
int main(void)
{
    test_all_ok_exact_output();
    test_simulated_label_from_env_or_device();
    test_light_open_failure_soil_still_checked();
    test_open_failure_reports_cleanup_errno();
    test_light_init_failure_each_step();
    test_soil_init_rejections();
    test_soil_stage_failures();
    test_soil_range_rejection_then_success();
    test_light_measure_failure_each_step();
    test_close_failure_reported();
    test_clock_failures();
    test_output_write_failure();
    test_unusable_run_arguments();
    test_parse_accepts();
    test_parse_rejects();
    test_usage_text();
    test_timespec_to_ns();

    puts("SIMULATED sensor-diag tests passed (scripted mock and fake "
         "clock; no sensor hardware or /dev/i2c device used)");
    return 0;
}
