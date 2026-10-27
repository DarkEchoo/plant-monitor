#include "sensors/capture.h"
#include "sensors/capture_args.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Tests for the capture program's options. No device, clock or bus is
 * used; parsing only.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* parse: run the parser on a NULL-terminated list; count error bytes. */
static capture_args_result parse(char *const *args, capture_config *cfg,
                                 long *err_bytes)
{
    char *argv[20];
    int argc = 0;
    FILE *err = tmpfile();
    capture_args_result r;

    assert(err != NULL);
    argv[argc++] = "capture";
    while (*args != NULL) {
        assert(argc < (int)COUNT_OF(argv) - 1);
        argv[argc++] = *args++;
    }
    argv[argc] = NULL;
    r = capture_parse_args(argc, argv, cfg, err);
    assert(fflush(err) == 0);
    *err_bytes = ftell(err);
    assert(fclose(err) == 0);
    return r;
}

/* same_config: every field equal (padding bytes are not compared). */
static int same_config(const capture_config *a, const capture_config *b)
{
    return a->sensor == b->sensor && a->bus_number == b->bus_number &&
           a->address_7bit == b->address_7bit &&
           a->tick_period_ns == b->tick_period_ns &&
           a->sample_every_ticks == b->sample_every_ticks &&
           a->backoff_cap_periods == b->backoff_cap_periods &&
           a->reinit_after_transport_failure ==
               b->reinit_after_transport_failure &&
           a->reinit_after_rejections == b->reinit_after_rejections &&
           a->first_sequence == b->first_sequence;
}

/*
 * test_defaults: with only the required options, each sensor gets its
 * default address and every proposed value.
 */
static void test_defaults(void)
{
    static char *const light[] = {"--sensor", "light", "--bus", "1", NULL};
    static char *const soil[] = {"--bus", "3", "--sensor", "soil", NULL};
    capture_config cfg;
    capture_config want;
    long err_bytes;

    assert(parse(light, &cfg, &err_bytes) == CAPTURE_ARGS_RUN);
    capture_config_proposed(&want, CAPTURE_SENSOR_LIGHT, 1, 0x23);
    assert(same_config(&cfg, &want) && err_bytes == 0);

    assert(parse(soil, &cfg, &err_bytes) == CAPTURE_ARGS_RUN);
    capture_config_proposed(&want, CAPTURE_SENSOR_SOIL, 3, 0x36);
    assert(same_config(&cfg, &want) && cfg.sample_every_ticks == 6);
}

/* test_overrides: address, tick and interval options, at their limits. */
static void test_overrides(void)
{
    static char *const light[] = {
        "--sensor", "light", "--bus", "0", "--addr", "0x5c",
        "--tick-ms", "100", "--every", "1000", NULL
    };
    static char *const soil[] = {
        "--sensor", "soil", "--bus", "4294967295", "--addr", "57",
        "--tick-ms", "3600000", "--every", "1", NULL
    };
    capture_config cfg;
    long err_bytes;

    assert(parse(light, &cfg, &err_bytes) == CAPTURE_ARGS_RUN);
    assert(cfg.sensor == CAPTURE_SENSOR_LIGHT && cfg.bus_number == 0);
    assert(cfg.address_7bit == 0x5c);
    assert(cfg.tick_period_ns == INT64_C(100000000));
    assert(cfg.sample_every_ticks == 1000);
    assert(cfg.backoff_cap_periods == CAPTURE_PROPOSED_BACKOFF_CAP_PERIODS);

    assert(parse(soil, &cfg, &err_bytes) == CAPTURE_ARGS_RUN);
    assert(cfg.sensor == CAPTURE_SENSOR_SOIL &&
           cfg.bus_number == 4294967295u);
    assert(cfg.address_7bit == 0x39);
    assert(cfg.tick_period_ns == INT64_C(3600000000000));
    assert(cfg.sample_every_ticks == 1);
}

/* test_help: --help wins wherever it appears after valid options. */
static void test_help(void)
{
    static char *const alone[] = {"--help", NULL};
    static char *const late[] = {"--sensor", "soil", "--help", NULL};
    capture_config cfg;
    long err_bytes;

    assert(parse(alone, &cfg, &err_bytes) == CAPTURE_ARGS_HELP);
    assert(parse(late, &cfg, &err_bytes) == CAPTURE_ARGS_HELP);
}

/*
 * test_rejects: each bad list is an error with a message, and *cfg is
 * untouched. There is no default sensor and no default bus.
 */
static void test_rejects(void)
{
    static char *const none[] = {NULL};
    static char *const no_sensor[] = {"--bus", "1", NULL};
    static char *const no_bus[] = {"--sensor", "light", NULL};
    static char *const bad_sensor[] = {"--sensor", "Light", "--bus", "1",
                                       NULL};
    static char *const no_value[] = {"--sensor", "light", "--bus", NULL};
    static char *const bad_bus[] = {"--sensor", "light", "--bus", "1x",
                                    NULL};
    static char *const neg_bus[] = {"--sensor", "light", "--bus", "-1",
                                    NULL};
    static char *const big_bus[] = {"--sensor", "light", "--bus",
                                    "4294967296", NULL};
    static char *const plus_bus[] = {"--sensor", "light", "--bus", "+1",
                                     NULL};
    static char *const space_every[] = {"--sensor", "light", "--bus", "1",
                                        "--every", " 2", NULL};
    static char *const light_addr[] = {"--sensor", "light", "--bus", "1",
                                       "--addr", "0x36", NULL};
    static char *const soil_addr[] = {"--sensor", "soil", "--bus", "1",
                                      "--addr", "0x23", NULL};
    static char *const soil_high[] = {"--sensor", "soil", "--bus", "1",
                                      "--addr", "0x3a", NULL};
    static char *const tick_low[] = {"--sensor", "light", "--bus", "1",
                                     "--tick-ms", "99", NULL};
    static char *const tick_high[] = {"--sensor", "light", "--bus", "1",
                                      "--tick-ms", "3600001", NULL};
    static char *const every_zero[] = {"--sensor", "light", "--bus", "1",
                                       "--every", "0", NULL};
    static char *const every_high[] = {"--sensor", "light", "--bus", "1",
                                       "--every", "1001", NULL};
    static char *const twice[] = {"--sensor", "light", "--sensor", "soil",
                                  "--bus", "1", NULL};
    static char *const unknown[] = {"--sensor", "light", "--bus", "1",
                                    "--rate", "5", NULL};
    static char *const equals[] = {"--sensor=light", "--bus", "1", NULL};
    static char *const positional[] = {"--sensor", "light", "--bus", "1",
                                       "extra", NULL};
    static char *const *const cases[] = {
        none, no_sensor, no_bus, bad_sensor, no_value, bad_bus, neg_bus,
        big_bus, plus_bus, space_every, light_addr, soil_addr, soil_high, tick_low, tick_high,
        every_zero, every_high, twice, unknown, equals, positional
    };
    size_t i;

    for (i = 0; i < COUNT_OF(cases); i++) {
        capture_config cfg;
        capture_config before;
        long err_bytes;

        memset(&cfg, 0x5a, sizeof cfg);
        before = cfg;
        assert(parse(cases[i], &cfg, &err_bytes) == CAPTURE_ARGS_ERROR);
        assert(err_bytes > 0);
        assert(memcmp(&cfg, &before, sizeof cfg) == 0);
    }
}

/* test_usage_text: the help names the required options and proposals. */
static void test_usage_text(void)
{
    char out[4096];
    FILE *f = tmpfile();
    size_t n;

    assert(f != NULL);
    capture_usage(f, "capture");
    rewind(f);
    n = fread(out, 1, sizeof out - 1, f);
    assert(!ferror(f) && n < sizeof out - 1);
    out[n] = '\0';
    assert(fclose(f) == 0);
    assert(strstr(out, "usage: capture --sensor light|soil --bus N") != NULL);
    assert(strstr(out, "--bus is required") != NULL);
    assert(strstr(out, "PROPOSED") != NULL);
}

/* test_exit_status: 0 only for a clean stop, 2 for usage, 1 otherwise. */
static void test_exit_status(void)
{
    static const capture_exit failures[] = {
        CAPTURE_EXIT_OPEN_FAILED, CAPTURE_EXIT_CLOCK_FAILED,
        CAPTURE_EXIT_SLEEP_FAILED, CAPTURE_EXIT_SINK_FAILED,
        CAPTURE_EXIT_SEQUENCE_EXHAUSTED, CAPTURE_EXIT_CLOSE_FAILED,
        (capture_exit)99
    };
    size_t i;

    assert(capture_exit_status(CAPTURE_EXIT_STOPPED) == 0);
    assert(capture_exit_status(CAPTURE_EXIT_USAGE) == 2);
    for (i = 0; i < COUNT_OF(failures); i++) {
        assert(capture_exit_status(failures[i]) == 1);
    }
}

/* main: run every test; abort on the first failure. */
int main(void)
{
    test_defaults();
    test_overrides();
    test_help();
    test_rejects();
    test_usage_text();
    test_exit_status();

    puts("capture argument tests passed (parsing only; no device, clock "
         "or bus used)");
    return 0;
}
