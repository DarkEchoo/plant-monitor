#include "sensors/sensor_diag.h"

#include "sensors/bh1750.h"
#include "sensors/seesaw_soil.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NSEC_PER_SEC INT64_C(1000000000)

/* What one driver call reported, in a form shared by both sensors. */
typedef struct {
    i2c_result io;
    const char *stage_name;
    int ok;                   /* the driver reported overall success */
    int has_rejected_value;
    uint16_t rejected_value;
} diag_outcome;

/* One successful reading, in a form shared by both sensors. */
typedef struct {
    uint32_t value;           /* in the sensor's unit */
    uint16_t raw_count;       /* light only */
} diag_value;

/* How sensor-diag drives and prints one kind of sensor. */
typedef struct {
    const char *name;
    const char *unit;
    int prints_raw_count;
    diag_outcome (*init)(i2c_device *dev);
    diag_outcome (*measure)(i2c_device *dev, diag_value *out);
} diag_sensor;

/* State of one run. */
typedef struct {
    const sensor_diag_config *cfg;
    const sensor_diag_env *env;
    FILE *out;
    unsigned failures;
    unsigned valid_readings;
    int any_simulated;
} diag_run;

/*
 * from_bh1750: success needs stage NONE and an OK transport result; a
 * status claiming NONE with a failed transfer is treated as a failure.
 */
static diag_outcome from_bh1750(bh1750_status s)
{
    diag_outcome o;

    o.io = s.io;
    o.stage_name = bh1750_stage_name(s.stage);
    o.ok = s.stage == BH1750_STAGE_NONE && s.io.status == I2C_STATUS_OK;
    o.has_rejected_value = 0;
    o.rejected_value = 0;
    return o;
}

/*
 * from_soil: as from_bh1750. HW_ID and RANGE rejections carry the value
 * the driver refused; their transport result is OK.
 */
static diag_outcome from_soil(soil_status s)
{
    diag_outcome o;

    o.io = s.io;
    o.stage_name = soil_stage_name(s.stage);
    o.ok = s.stage == SOIL_STAGE_NONE && s.io.status == I2C_STATUS_OK;
    o.has_rejected_value =
        s.stage == SOIL_STAGE_HW_ID || s.stage == SOIL_STAGE_RANGE;
    o.rejected_value = s.rejected_value;
    return o;
}

/* light_init: Power On and set MTreg. */
static diag_outcome light_init(i2c_device *dev)
{
    return from_bh1750(bh1750_init(dev));
}

/* light_measure: one H-resolution reading in integer millilux. */
static diag_outcome light_measure(i2c_device *dev, diag_value *out)
{
    bh1750_reading reading;
    bh1750_status s = bh1750_measure_once_h(dev, &reading);
    diag_outcome o = from_bh1750(s);

    if (o.ok) {
        out->value = reading.illuminance_mlx;
        out->raw_count = reading.raw_count;
    }
    return o;
}

/* soil_init_check: software reset and hardware-ID check. */
static diag_outcome soil_init_check(i2c_device *dev)
{
    return from_soil(soil_init(dev));
}

/* soil_measure: one raw capacitance reading, uncalibrated. */
static diag_outcome soil_measure(i2c_device *dev, diag_value *out)
{
    soil_reading reading;
    soil_status s = soil_measure_capacitance(dev, &reading);
    diag_outcome o = from_soil(s);

    if (o.ok) {
        out->value = reading.capacitance_counts;
        out->raw_count = 0;
    }
    return o;
}

static const diag_sensor light_sensor = {
    "light", "millilux", 1, light_init, light_measure
};

static const diag_sensor soil_sensor = {
    "soil", "raw_counts", 0, soil_init_check, soil_measure
};

/* label: the prefix every output line starts with. */
static const char *label(int simulated)
{
    return simulated ? "SIMULATED" : "LIVE";
}

/* light_address_valid: the BH1750's two ADDR-pin addresses. */
static int light_address_valid(unsigned long address)
{
    return address == BH1750_ADDRESS_ADDR_LOW ||
           address == BH1750_ADDRESS_ADDR_HIGH;
}

/* soil_address_valid: the soil board's four jumper addresses. */
static int soil_address_valid(unsigned long address)
{
    return address >= SOIL_ADDRESS_MIN && address <= SOIL_ADDRESS_MAX;
}

/* samples_valid: the allowed measurement count per sensor. */
static int samples_valid(unsigned long samples)
{
    return samples >= 1 && samples <= SENSOR_DIAG_SAMPLES_MAX;
}

/* config_usable: every field is in range. */
static int config_usable(const sensor_diag_config *cfg)
{
    return cfg != NULL && samples_valid(cfg->samples) &&
           light_address_valid(cfg->light_address_7bit) &&
           soil_address_valid(cfg->soil_address_7bit);
}

/* print_io: the transport status and errno, with its text when nonzero. */
static void print_io(FILE *out, i2c_result io)
{
    fprintf(out, " i2c=%s errno=%d", i2c_status_name(io.status), io.os_errno);
    if (io.os_errno != 0) {
        fprintf(out, " errno_text=\"%s\"", strerror(io.os_errno));
    }
}

/* print_rejected: the value a driver refused, or none. */
static void print_rejected(FILE *out, const diag_outcome *o)
{
    if (o->has_rejected_value) {
        fprintf(out, " rejected_value=%u", (unsigned)o->rejected_value);
    } else {
        fputs(" rejected_value=none", out);
    }
}

/* One clock reading: nanoseconds, or why there is none. */
typedef struct {
    int64_t ns;
    int call_errno;    /* the clock call failed */
    int value_errno;   /* the call succeeded but its value is unusable */
} diag_clock_reading;

/* read_clock_ns: read one clock and convert it to nanoseconds. */
static diag_clock_reading read_clock_ns(const diag_run *run,
                                        sensor_diag_clock clock)
{
    diag_clock_reading r = {0, 0, 0};
    struct timespec ts;

    r.call_errno = run->env->read_clock(run->env->ctx, clock, &ts);
    if (r.call_errno == 0) {
        r.value_errno = sensor_diag_timespec_to_ns(&ts, &r.ns);
    }
    return r;
}

/* clock_failed: true if the reading has no usable timestamp. */
static int clock_failed(const diag_clock_reading *r)
{
    return r->call_errno != 0 || r->value_errno != 0;
}

/* print_ns: a timestamp field, or none if the clock gave none. */
static void print_ns(FILE *out, const char *key,
                     const diag_clock_reading *r)
{
    if (!clock_failed(r)) {
        fprintf(out, " %s=%" PRId64, key, r->ns);
    } else {
        fprintf(out, " %s=none", key);
    }
}

/* print_clock_error: why a clock gave no timestamp, if it did not. */
static void print_clock_error(FILE *out, const char *clock_name,
                              const diag_clock_reading *r)
{
    if (r->call_errno != 0) {
        fprintf(out, " %s_clock_errno=%d", clock_name, r->call_errno);
    } else if (r->value_errno != 0) {
        fprintf(out, " %s_clock_value_errno=%d", clock_name,
                r->value_errno);
    }
}

/*
 * measure_once: one measurement attempt, then both clocks straight away,
 * so the timestamps mark when the attempt completed, success or not.
 */
static void measure_once(diag_run *run, const diag_sensor *sensor,
                         i2c_device *dev, uint8_t address, int simulated,
                         unsigned sample)
{
    FILE *out = run->out;
    diag_value v = {0, 0};
    diag_outcome o = sensor->measure(dev, &v);
    diag_clock_reading wall = read_clock_ns(run, SENSOR_DIAG_CLOCK_WALL);
    diag_clock_reading mono =
        read_clock_ns(run, SENSOR_DIAG_CLOCK_MONOTONIC);

    fprintf(out, "%s sensor=%s event=measure addr=0x%02x sample=%u/%u",
            label(simulated), sensor->name, (unsigned)address, sample,
            run->cfg->samples);
    print_ns(out, "wall_ns", &wall);
    print_ns(out, "mono_ns", &mono);
    fputs(" clock_sync=unknown", out);
    if (o.ok) {
        fprintf(out, " valid=1 value=%" PRIu32 " unit=%s", v.value,
                sensor->unit);
        if (sensor->prints_raw_count) {
            fprintf(out, " raw_count=%u", (unsigned)v.raw_count);
        }
    } else {
        fprintf(out, " valid=0 value=none unit=%s", sensor->unit);
        if (sensor->prints_raw_count) {
            fputs(" raw_count=none", out);
        }
    }
    fprintf(out, " stage=%s", o.stage_name);
    print_io(out, o.io);
    print_rejected(out, &o);
    print_clock_error(out, "wall", &wall);
    print_clock_error(out, "mono", &mono);
    fputc('\n', out);

    if (o.ok) {
        run->valid_readings++;
    } else {
        run->failures++;
    }
    run->failures += clock_failed(&wall) + clock_failed(&mono);
}

/*
 * check_sensor: open, initialize once, measure, close. A failed open
 * leaves nothing to close; any later failure still closes the device.
 */
static void check_sensor(diag_run *run, const diag_sensor *sensor,
                         uint8_t address)
{
    FILE *out = run->out;
    i2c_device dev = {0};
    int cleanup_errno = 0;
    int simulated = run->env->is_simulated;
    i2c_result io;
    diag_outcome o;
    unsigned sample;

    io = run->env->open_device(run->env->ctx, &dev, run->cfg->bus_number,
                               address, &cleanup_errno);
    if (io.status == I2C_STATUS_OK) {
        simulated = simulated || dev.is_simulated;
    }
    run->any_simulated = run->any_simulated || simulated;
    fprintf(out, "%s sensor=%s event=open addr=0x%02x result=%s",
            label(simulated), sensor->name, (unsigned)address,
            io.status == I2C_STATUS_OK ? "OK" : "FAIL");
    print_io(out, io);
    fprintf(out, " cleanup_errno=%d\n", cleanup_errno);
    if (io.status != I2C_STATUS_OK) {
        run->failures++;
        return;
    }

    o = sensor->init(&dev);
    fprintf(out, "%s sensor=%s event=init addr=0x%02x result=%s stage=%s",
            label(simulated), sensor->name, (unsigned)address,
            o.ok ? "OK" : "FAIL", o.stage_name);
    print_io(out, o.io);
    print_rejected(out, &o);
    fputs(o.ok ? "\n" : " measurements=skipped\n", out);
    if (!o.ok) {
        run->failures++;
        goto close_device;
    }

    for (sample = 1; sample <= run->cfg->samples; sample++) {
        measure_once(run, sensor, &dev, address, simulated, sample);
    }

close_device:
    io = i2c_device_close(&dev);
    fprintf(out, "%s sensor=%s event=close addr=0x%02x result=%s",
            label(simulated), sensor->name, (unsigned)address,
            io.status == I2C_STATUS_OK ? "OK" : "FAIL");
    print_io(out, io);
    fputc('\n', out);
    if (io.status != I2C_STATUS_OK) {
        run->failures++;
    }
}

/* sensor_diag_run: check the light sensor, then the soil sensor. */
int sensor_diag_run(const sensor_diag_config *cfg,
                    const sensor_diag_env *env, FILE *out)
{
    diag_run run;

    if (!config_usable(cfg) || env == NULL || env->open_device == NULL ||
        env->read_clock == NULL || out == NULL) {
        return SENSOR_DIAG_EXIT_USAGE;
    }
    run.cfg = cfg;
    run.env = env;
    run.out = out;
    run.failures = 0;
    run.valid_readings = 0;
    run.any_simulated = env->is_simulated;

    fprintf(out, "%s event=start tool=sensor-diag "
                 "output=diagnostic-not-storage-record bus=%u "
                 "light_addr=0x%02x soil_addr=0x%02x samples=%u "
                 "clock_sync=unknown timestamps=attempt_completion\n",
            label(env->is_simulated), cfg->bus_number,
            (unsigned)cfg->light_address_7bit,
            (unsigned)cfg->soil_address_7bit, cfg->samples);

    check_sensor(&run, &light_sensor, cfg->light_address_7bit);
    check_sensor(&run, &soil_sensor, cfg->soil_address_7bit);

    fprintf(out, "%s event=summary result=%s failures=%u "
                 "valid_readings=%u/%u\n",
            label(run.any_simulated), run.failures == 0 ? "PASS" : "FAIL",
            run.failures, run.valid_readings, 2 * cfg->samples);

    if (fflush(out) != 0 || ferror(out)) {
        return SENSOR_DIAG_EXIT_FAILURE;
    }
    return run.failures == 0 ? SENSOR_DIAG_EXIT_OK : SENSOR_DIAG_EXIT_FAILURE;
}

/* sensor_diag_timespec_to_ns: checked tv_sec * 1e9 + tv_nsec. */
int sensor_diag_timespec_to_ns(const struct timespec *ts, int64_t *out_ns)
{
    intmax_t sec;

    if (ts == NULL || out_ns == NULL || ts->tv_nsec < 0 ||
        ts->tv_nsec >= NSEC_PER_SEC) {
        return EINVAL;
    }
    sec = (intmax_t)ts->tv_sec;
    if (sec < 0 || sec > INT64_MAX / NSEC_PER_SEC ||
        (sec == INT64_MAX / NSEC_PER_SEC &&
         ts->tv_nsec > INT64_MAX % NSEC_PER_SEC)) {
        return ERANGE;
    }
    *out_ns = (int64_t)sec * NSEC_PER_SEC + ts->tv_nsec;
    return 0;
}

/*
 * parse_decimal: digits only, no sign or space, at most max. strtoul alone
 * would accept " 1", "+1" and "-1" (as a huge value).
 */
static int parse_decimal(const char *text, unsigned long max,
                         unsigned long *out)
{
    char *end;
    unsigned long v;

    if (!isdigit((unsigned char)text[0])) {
        return 0;
    }
    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || v > max) {
        return 0;
    }
    *out = v;
    return 1;
}

/*
 * parse_address: "0x" plus hex digits, or decimal digits. A leading 0 is
 * decimal, never octal, so "043" cannot silently mean 0x23.
 */
static int parse_address(const char *text, unsigned long *out)
{
    char *end;
    unsigned long v;

    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return parse_decimal(text, I2C_ADDRESS_7BIT_MAX, out);
    }
    if (!isxdigit((unsigned char)text[2])) {
        return 0;
    }
    errno = 0;
    v = strtoul(text + 2, &end, 16);
    if (errno == ERANGE || *end != '\0' || v > I2C_ADDRESS_7BIT_MAX) {
        return 0;
    }
    *out = v;
    return 1;
}

/* sensor_diag_parse_args: see sensor_diag.h. */
sensor_diag_args sensor_diag_parse_args(int argc, char *const argv[],
                                        sensor_diag_config *cfg, FILE *err)
{
    sensor_diag_config parsed;
    int have_bus = 0;
    int have_samples = 0;
    int have_light = 0;
    int have_soil = 0;
    const char *program;
    int i;

    if (argc < 1 || argv == NULL || cfg == NULL || err == NULL) {
        return SENSOR_DIAG_ARGS_ERROR;
    }
    program = argv[0] != NULL ? argv[0] : "sensor-diag";
    parsed.bus_number = 0;
    parsed.light_address_7bit = BH1750_ADDRESS_ADDR_LOW;
    parsed.soil_address_7bit = SOIL_ADDRESS_MIN;
    parsed.samples = SENSOR_DIAG_SAMPLES_DEFAULT;

    for (i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *value;
        unsigned long v;
        int *seen;

        if (opt == NULL) {
            fprintf(err, "%s: missing argument\n", program);
            return SENSOR_DIAG_ARGS_ERROR;
        }
        if (strcmp(opt, "--help") == 0) {
            return SENSOR_DIAG_ARGS_HELP;
        }
        if (strcmp(opt, "--bus") == 0) {
            seen = &have_bus;
        } else if (strcmp(opt, "--samples") == 0) {
            seen = &have_samples;
        } else if (strcmp(opt, "--light-addr") == 0) {
            seen = &have_light;
        } else if (strcmp(opt, "--soil-addr") == 0) {
            seen = &have_soil;
        } else {
            fprintf(err, "%s: unknown argument '%s'\n", program, opt);
            return SENSOR_DIAG_ARGS_ERROR;
        }
        if (*seen) {
            fprintf(err, "%s: %s given more than once\n", program, opt);
            return SENSOR_DIAG_ARGS_ERROR;
        }
        if (i + 1 >= argc || argv[i + 1] == NULL) {
            fprintf(err, "%s: %s needs a value\n", program, opt);
            return SENSOR_DIAG_ARGS_ERROR;
        }
        value = argv[++i];

        if (seen == &have_bus) {
            if (!parse_decimal(value, UINT_MAX, &v)) {
                fprintf(err, "%s: --bus must be a decimal number from 0 "
                             "to %u, not '%s'\n", program, UINT_MAX, value);
                return SENSOR_DIAG_ARGS_ERROR;
            }
            parsed.bus_number = (unsigned)v;
        } else if (seen == &have_samples) {
            if (!parse_decimal(value, ULONG_MAX, &v) || !samples_valid(v)) {
                fprintf(err, "%s: --samples must be 1 to %u, not '%s'\n",
                        program, SENSOR_DIAG_SAMPLES_MAX, value);
                return SENSOR_DIAG_ARGS_ERROR;
            }
            parsed.samples = (unsigned)v;
        } else if (seen == &have_light) {
            if (!parse_address(value, &v) || !light_address_valid(v)) {
                fprintf(err, "%s: --light-addr must be 0x23 or 0x5c, "
                             "not '%s'\n", program, value);
                return SENSOR_DIAG_ARGS_ERROR;
            }
            parsed.light_address_7bit = (uint8_t)v;
        } else {
            if (!parse_address(value, &v) || !soil_address_valid(v)) {
                fprintf(err, "%s: --soil-addr must be 0x36 to 0x39, "
                             "not '%s'\n", program, value);
                return SENSOR_DIAG_ARGS_ERROR;
            }
            parsed.soil_address_7bit = (uint8_t)v;
        }
        *seen = 1;
    }

    if (!have_bus) {
        fprintf(err, "%s: --bus is required; there is no default bus\n",
                program);
        return SENSOR_DIAG_ARGS_ERROR;
    }
    *cfg = parsed;
    return SENSOR_DIAG_ARGS_RUN;
}

/* sensor_diag_usage: the --help text. */
void sensor_diag_usage(FILE *out, const char *program)
{
    fprintf(out,
        "usage: %s --bus N [--samples K] [--light-addr ADDR] "
        "[--soil-addr ADDR]\n"
        "\n"
        "One-shot check of the BH1750 light sensor and the seesaw soil\n"
        "sensor on /dev/i2c-N. Run it on the Raspberry Pi the sensors are\n"
        "wired to.\n"
        "\n"
        "  --bus N            I2C bus number. --bus is required; there is\n"
        "                     no default, because initialization writes to\n"
        "                     the bus (BH1750 Power On, soil-sensor reset).\n"
        "  --samples K        measurements per sensor, 1 to %u (default %u)\n"
        "  --light-addr ADDR  BH1750 address, 0x23 (default) or 0x5c\n"
        "  --soil-addr ADDR   soil-sensor address, 0x36 (default) to 0x39\n"
        "  --help             show this text\n"
        "\n"
        "Each sensor is opened, initialized once and measured K times, with\n"
        "no retries. Light is integer millilux; soil is raw capacitance\n"
        "counts (uncalibrated). A reading is valid only when the driver\n"
        "reports success; a failed reading prints valid=0 value=none. Both\n"
        "clocks are read right after each measurement attempt, including\n"
        "failed ones, and mark when the attempt completed. clock_sync is\n"
        "always unknown.\n"
        "\n"
        "This is a diagnostic format, not the storage record format.\n"
        "\n"
        "A short check, such as --samples 3, can demonstrate only\n"
        "basic communication and responsiveness.\n"
        "It cannot establish timing reliability, firmware behavior,\n"
        "or the full valid measurement range.\n"
        "\n"
        "Exit status: 0 if every operation succeeded, 1 if any failed, 2 for\n"
        "a usage error (nothing is opened).\n",
        program, SENSOR_DIAG_SAMPLES_MAX, SENSOR_DIAG_SAMPLES_DEFAULT);
}
