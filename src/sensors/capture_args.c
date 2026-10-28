#include "sensors/capture_args.h"

#include "sensors/bh1750.h"
#include "sensors/seesaw_soil.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NSEC_PER_MSEC INT64_C(1000000)

/* The options, in the order they are looked up. */
typedef enum {
    OPT_SENSOR,
    OPT_BUS,
    OPT_ADDR,
    OPT_TICK_MS,
    OPT_EVERY,
    OPT_STORE_DIR,      /* only for capture_parse_args_store */
    OPT_COUNT
} option_id;

static const char *const option_names[OPT_COUNT] = {
    "--sensor", "--bus", "--addr", "--tick-ms", "--every", "--store-dir"
};

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
 * decimal, never octal.
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

/* address_valid: an address the sensor's driver accepts. */
static int address_valid(capture_sensor sensor, unsigned long address)
{
    if (sensor == CAPTURE_SENSOR_LIGHT) {
        return address == BH1750_ADDRESS_ADDR_LOW ||
               address == BH1750_ADDRESS_ADDR_HIGH;
    }
    return address >= SOIL_ADDRESS_MIN && address <= SOIL_ADDRESS_MAX;
}

/* find_option: the option's id, or OPT_COUNT if unknown. */
static option_id find_option(const char *arg)
{
    int i;

    for (i = 0; i < OPT_COUNT; i++) {
        if (strcmp(arg, option_names[i]) == 0) {
            return (option_id)i;
        }
    }
    return OPT_COUNT;
}

/*
 * parse_args: both public parsers. A NULL store_dir makes --store-dir an
 * unknown argument, exactly as before it existed; otherwise it is
 * required.
 */
static capture_args_result parse_args(int argc, char *const argv[],
                                      capture_config *cfg,
                                      const char **store_dir, FILE *err)
{
    const char *value[OPT_COUNT] = {NULL};
    const char *program;
    capture_config parsed;
    capture_sensor sensor;
    unsigned long bus;
    unsigned long v;
    int i;

    if (argc < 1 || argv == NULL || cfg == NULL || err == NULL) {
        return CAPTURE_ARGS_ERROR;
    }
    program = argv[0] != NULL ? argv[0] : "capture";

    for (i = 1; i < argc; i++) {
        const char *opt = argv[i];
        option_id id;

        if (opt == NULL) {
            fprintf(err, "%s: missing argument\n", program);
            return CAPTURE_ARGS_ERROR;
        }
        if (strcmp(opt, "--help") == 0) {
            return CAPTURE_ARGS_HELP;
        }
        id = find_option(opt);
        if (id == OPT_COUNT || (id == OPT_STORE_DIR && store_dir == NULL)) {
            fprintf(err, "%s: unknown argument '%s'\n", program, opt);
            return CAPTURE_ARGS_ERROR;
        }
        if (value[id] != NULL) {
            fprintf(err, "%s: %s given more than once\n", program, opt);
            return CAPTURE_ARGS_ERROR;
        }
        if (i + 1 >= argc || argv[i + 1] == NULL ||
            strncmp(argv[i + 1], "--", 2) == 0) {
            fprintf(err, "%s: %s needs a value\n", program, opt);
            return CAPTURE_ARGS_ERROR;
        }
        value[id] = argv[++i];
    }

    if (value[OPT_SENSOR] == NULL) {
        fprintf(err, "%s: --sensor is required (light or soil)\n", program);
        return CAPTURE_ARGS_ERROR;
    }
    if (strcmp(value[OPT_SENSOR], "light") == 0) {
        sensor = CAPTURE_SENSOR_LIGHT;
    } else if (strcmp(value[OPT_SENSOR], "soil") == 0) {
        sensor = CAPTURE_SENSOR_SOIL;
    } else {
        fprintf(err, "%s: --sensor must be light or soil, not '%s'\n",
                program, value[OPT_SENSOR]);
        return CAPTURE_ARGS_ERROR;
    }
    if (value[OPT_BUS] == NULL) {
        fprintf(err, "%s: --bus is required; there is no default bus\n",
                program);
        return CAPTURE_ARGS_ERROR;
    }
    if (!parse_decimal(value[OPT_BUS], UINT_MAX, &bus)) {
        fprintf(err, "%s: --bus must be a decimal number from 0 to %u, "
                     "not '%s'\n", program, UINT_MAX, value[OPT_BUS]);
        return CAPTURE_ARGS_ERROR;
    }

    capture_config_proposed(&parsed, sensor, (unsigned)bus,
                            sensor == CAPTURE_SENSOR_LIGHT
                                ? BH1750_ADDRESS_ADDR_LOW
                                : SOIL_ADDRESS_MIN);

    if (value[OPT_ADDR] != NULL) {
        if (!parse_address(value[OPT_ADDR], &v) ||
            !address_valid(sensor, v)) {
            fprintf(err, "%s: --addr must be %s for %s, not '%s'\n",
                    program,
                    sensor == CAPTURE_SENSOR_LIGHT ? "0x23 or 0x5c"
                                                   : "0x36 to 0x39",
                    value[OPT_SENSOR], value[OPT_ADDR]);
            return CAPTURE_ARGS_ERROR;
        }
        parsed.address_7bit = (uint8_t)v;
    }
    if (value[OPT_TICK_MS] != NULL) {
        if (!parse_decimal(value[OPT_TICK_MS], CAPTURE_ARGS_TICK_MS_MAX,
                           &v) ||
            v < CAPTURE_ARGS_TICK_MS_MIN) {
            fprintf(err, "%s: --tick-ms must be %u to %u, not '%s'\n",
                    program, CAPTURE_ARGS_TICK_MS_MIN,
                    CAPTURE_ARGS_TICK_MS_MAX, value[OPT_TICK_MS]);
            return CAPTURE_ARGS_ERROR;
        }
        parsed.tick_period_ns = (int64_t)v * NSEC_PER_MSEC;
    }
    if (value[OPT_EVERY] != NULL) {
        if (!parse_decimal(value[OPT_EVERY], CAPTURE_ARGS_EVERY_MAX, &v) ||
            v < 1) {
            fprintf(err, "%s: --every must be 1 to %u, not '%s'\n",
                    program, CAPTURE_ARGS_EVERY_MAX, value[OPT_EVERY]);
            return CAPTURE_ARGS_ERROR;
        }
        parsed.sample_every_ticks = (uint32_t)v;
    }
    if (store_dir != NULL && value[OPT_STORE_DIR] == NULL) {
        fprintf(err, "%s: --store-dir is required (an existing "
                     "directory)\n", program);
        return CAPTURE_ARGS_ERROR;
    }
    if (store_dir != NULL && value[OPT_STORE_DIR][0] == '\0') {
        fprintf(err, "%s: --store-dir must not be empty\n", program);
        return CAPTURE_ARGS_ERROR;
    }

    *cfg = parsed;
    if (store_dir != NULL) {
        *store_dir = value[OPT_STORE_DIR];
    }
    return CAPTURE_ARGS_RUN;
}

/* capture_parse_args: see capture_args.h. */
capture_args_result capture_parse_args(int argc, char *const argv[],
                                       capture_config *cfg, FILE *err)
{
    return parse_args(argc, argv, cfg, NULL, err);
}

/* capture_parse_args_store: see capture_args.h. */
capture_args_result capture_parse_args_store(int argc, char *const argv[],
                                             capture_config *cfg,
                                             const char **store_dir,
                                             FILE *err)
{
    if (store_dir == NULL) {
        return CAPTURE_ARGS_ERROR;
    }
    return parse_args(argc, argv, cfg, store_dir, err);
}

/* capture_usage: the --help text. */
void capture_usage(FILE *out, const char *program)
{
    fprintf(out,
        "usage: %s --sensor light|soil --bus N --store-dir DIR\n"
        "       [--addr ADDR] [--tick-ms N] [--every N]\n"
        "\n"
        "Capture one sensor on /dev/i2c-N. Run one process per sensor.\n"
        "\n"
        "  --sensor light|soil  which sensor this process captures\n"
        "  --bus N              I2C bus number. --bus is required; there is\n"
        "                       no default, because initialization writes\n"
        "                       to the bus.\n"
        "  --addr ADDR          light 0x23 (default) or 0x5c; soil 0x36\n"
        "                       (default) to 0x39\n"
        "  --tick-ms N          heartbeat tick, %u to %u ms\n"
        "                       (PROPOSED default 10000)\n"
        "  --every N            ticks between attempts, 1 to %u\n"
        "                       (PROPOSED default: light 1, soil 6)\n"
        "  --store-dir DIR      existing directory for this sensor's storage\n"
        "                       log. TEMPORARY per-sensor integration until\n"
        "                       the hub exists: use one directory per sensor;\n"
        "                       sequence numbers are per process, not\n"
        "                       hub-wide. A previous run's log in DIR is\n"
        "                       refused (no recovery yet).\n"
        "  --help               show this text\n"
        "\n"
        "Numbers are decimal; hex only for --addr (0x23). A leading 0 is\n"
        "decimal. The rates, backoff and reinitialization policy are\n"
        "PROPOSED values awaiting agreement, not requirements.\n"
        "\n"
        "Exit status: 0 after a requested stop, 1 for any failure, 2 for a\n"
        "usage error.\n",
        program, CAPTURE_ARGS_TICK_MS_MIN, CAPTURE_ARGS_TICK_MS_MAX,
        CAPTURE_ARGS_EVERY_MAX);
}

/* capture_exit_status: see capture_args.h. */
int capture_exit_status(capture_exit result)
{
    if (result == CAPTURE_EXIT_STOPPED) {
        return 0;
    }
    return result == CAPTURE_EXIT_USAGE ? 2 : 1;
}
