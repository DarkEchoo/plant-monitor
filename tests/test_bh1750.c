#include "sensors/bh1750.h"
#include "sensors/i2c_mock.h"
#include "sensors/i2c_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * SIMULATED: every test drives the BH1750 driver through the scripted I2C
 * mock. No sensor or bus is used, and no reading here is a measurement.
 * Expected bytes and timings come from ROHM's BH1750FVI Technical Note,
 * Rev.C (2010.04).
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define INIT_STEPS 3

static const uint8_t cmd_power_on[] = {0x01};
static const uint8_t cmd_mtreg_high[] = {0x42};
static const uint8_t cmd_mtreg_low[] = {0x65};
static const uint8_t cmd_measure[] = {0x20};
/* Rev.C worked example: High Byte "10000011", Low Byte "10010000". */
static const uint8_t example_bytes[] = {0x83, 0x90};

/* A reading no driver would produce, to show failures leave *out alone. */
static const bh1750_reading reading_sentinel = {0xBEEF, 0xDEADBEEFu, 7};

/* reading_is_sentinel: true if every field still holds the sentinel. */
static int reading_is_sentinel(const bh1750_reading *r)
{
    return r->raw_count == reading_sentinel.raw_count &&
           r->illuminance_mlx == reading_sentinel.illuminance_mlx &&
           r->is_simulated == reading_sentinel.is_simulated;
}

/* write_step: a scripted one-byte command that succeeds. */
static i2c_mock_step write_step(const uint8_t *cmd)
{
    i2c_mock_step step = {I2C_MOCK_WRITE, cmd, 1, 1, 0, 0};
    return step;
}

/* wait_step: a scripted wait of at least the documented maximum. */
static i2c_mock_step wait_step(void)
{
    i2c_mock_step step = {I2C_MOCK_WAIT, NULL, 0, 0, 0, 180000};
    return step;
}

/* read_step: a scripted two-byte read returning bytes. */
static i2c_mock_step read_step(const uint8_t *bytes)
{
    i2c_mock_step step = {I2C_MOCK_READ, bytes, 2, 2, 0, 0};
    return step;
}

/* init_script: the three initialization writes, in order. */
static void init_script(i2c_mock_step steps[INIT_STEPS])
{
    steps[0] = write_step(cmd_power_on);
    steps[1] = write_step(cmd_mtreg_high);
    steps[2] = write_step(cmd_mtreg_low);
}

/* open_scripted: open a mock device at address_7bit with steps. */
static void open_scripted(i2c_mock *mock, i2c_device *dev,
                          uint8_t address_7bit, const i2c_mock_step *steps,
                          size_t step_count)
{
    assert(i2c_mock_open(mock, dev, address_7bit, steps, step_count) ==
           I2C_STATUS_OK);
}

/*
 * close_consumed: close succeeds only if the driver used every scripted
 * step and nothing more, so a driver that kept going after a failure, or
 * stopped early, fails here.
 */
static void close_consumed(i2c_device *dev)
{
    assert(i2c_device_close(dev).status == I2C_STATUS_OK);
}

/*
 * test_raw_to_mlx_vectors: boundaries, small values, and the Rev.C worked
 * examples. 33680 counts / 1.2 = 28066.666... lx rounds to 28066667 mlx,
 * which is 28067 lx to the nearest lux, matching the note's "28067 [ lx ]".
 * 272 counts gives 226667 mlx, matching "227 [ lx ]". That second
 * example is the note's One Time L-resolution example, so it checks only
 * the counts / 1.2 arithmetic, not an H-resolution reading.
 */
static void test_raw_to_mlx_vectors(void)
{
    static const struct {
        uint16_t raw;
        uint32_t mlx;
    } vectors[] = {
        {0, 0},              /* boundary: dark */
        {1, 833},            /* 833.33 rounds down */
        {2, 1667},           /* 1666.67 rounds up */
        {3, 2500},           /* exact */
        {272, 226667},       /* Rev.C ex2 (L-res): arithmetic only */
        {33680, 28066667},   /* Rev.C example 1 */
        {65535, 54612500}    /* boundary: full scale, 54612500.0 exact */
    };
    size_t i;

    for (i = 0; i < COUNT_OF(vectors); i++) {
        assert(bh1750_raw_to_mlx(vectors[i].raw) == vectors[i].mlx);
    }
    assert((bh1750_raw_to_mlx(33680) + 500) / 1000 == 28067);
    assert((bh1750_raw_to_mlx(272) + 500) / 1000 == 227);
}

/* test_init_sends_documented_commands: Power On, MTreg high, MTreg low. */
static void test_init_sends_documented_commands(void)
{
    i2c_mock_step steps[INIT_STEPS];
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_status s;

    init_script(steps);
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, INIT_STEPS);
    s = bh1750_init(&dev);
    assert(s.stage == BH1750_STAGE_NONE && s.io.status == I2C_STATUS_OK);
    assert(mock.next_step == INIT_STEPS && mock.waited_us == 0);
    close_consumed(&dev);
}

/*
 * test_init_stops_at_each_failure: a failed or short write at any of the
 * three init commands is reported with that command's stage, and nothing
 * after it is sent.
 */
static void test_init_stops_at_each_failure(void)
{
    static const bh1750_stage stages[INIT_STEPS] = {
        BH1750_STAGE_POWER_ON, BH1750_STAGE_MTREG_HIGH, BH1750_STAGE_MTREG_LOW
    };
    size_t k;
    int short_write;

    for (k = 0; k < INIT_STEPS; k++) {
        for (short_write = 0; short_write <= 1; short_write++) {
            i2c_mock_step steps[INIT_STEPS];
            i2c_mock mock;
            i2c_device dev = {0};
            bh1750_status s;

            init_script(steps);
            if (short_write) {
                steps[k].transfer_len = 0;
            } else {
                steps[k].fail_errno = EIO;
            }
            open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, k + 1);
            s = bh1750_init(&dev);
            assert(s.stage == stages[k]);
            if (short_write) {
                assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
                assert(s.io.requested_len == 1 && s.io.transferred_len == 0);
            } else {
                assert(s.io.status == I2C_STATUS_OS_ERROR);
                assert(s.io.os_errno == EIO);
            }
            assert(mock.next_step == k + 1);
            close_consumed(&dev);
        }
    }
}

/*
 * test_measure_example: one measurement decodes the Rev.C example bytes,
 * high byte first, after waiting the documented maximum.
 */
static void test_measure_example(void)
{
    i2c_mock_step steps[3];
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;
    bh1750_status s;

    steps[0] = write_step(cmd_measure);
    steps[1] = wait_step();
    steps[2] = read_step(example_bytes);
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, 3);
    s = bh1750_measure_once_h(&dev, &out);
    assert(s.stage == BH1750_STAGE_NONE && s.io.status == I2C_STATUS_OK);
    assert(out.raw_count == 33680);
    assert(out.illuminance_mlx == 28066667);
    assert(out.is_simulated == 1);
    assert(mock.waited_us == BH1750_H_RES_MAX_WAIT_US);
    assert(BH1750_H_RES_MAX_WAIT_US == 180000u);
    close_consumed(&dev);
}

/* test_byte_order: {0x01, 0x10} is 272 counts, not 0x1001 = 4097. */
static void test_byte_order(void)
{
    static const uint8_t bytes[] = {0x01, 0x10};
    i2c_mock_step steps[3];
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;

    steps[0] = write_step(cmd_measure);
    steps[1] = wait_step();
    steps[2] = read_step(bytes);
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, 3);
    assert(bh1750_measure_once_h(&dev, &out).stage == BH1750_STAGE_NONE);
    assert(out.raw_count == 272 && out.illuminance_mlx == 226667);
    close_consumed(&dev);
}

/* test_init_then_measure: the full documented sequence at address 0x5c. */
static void test_init_then_measure(void)
{
    i2c_mock_step steps[INIT_STEPS + 3];
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;

    init_script(steps);
    steps[3] = write_step(cmd_measure);
    steps[4] = wait_step();
    steps[5] = read_step(example_bytes);
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_HIGH, steps,
                  COUNT_OF(steps));
    assert(bh1750_init(&dev).stage == BH1750_STAGE_NONE);
    assert(bh1750_measure_once_h(&dev, &out).stage == BH1750_STAGE_NONE);
    assert(out.raw_count == 33680);
    close_consumed(&dev);
}

/*
 * expect_measure_failure: run a measurement against steps (which end at
 * the failing step), check the stage and status, that *out is untouched,
 * and that nothing after the failure was attempted.
 */
static bh1750_status expect_measure_failure(const i2c_mock_step *steps,
                                            size_t step_count,
                                            bh1750_stage stage)
{
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;
    bh1750_status s;

    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, step_count);
    s = bh1750_measure_once_h(&dev, &out);
    assert(s.stage == stage);
    assert(reading_is_sentinel(&out));
    assert(mock.next_step == step_count);
    close_consumed(&dev);
    return s;
}

/* test_measure_stops_at_each_failure: command, wait and read failures. */
static void test_measure_stops_at_each_failure(void)
{
    i2c_mock_step steps[3];
    bh1750_status s;

    steps[0] = write_step(cmd_measure);
    steps[0].fail_errno = EIO;
    s = expect_measure_failure(steps, 1, BH1750_STAGE_MEASURE_COMMAND);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == EIO);

    steps[0] = write_step(cmd_measure);
    steps[0].transfer_len = 0;
    s = expect_measure_failure(steps, 1, BH1750_STAGE_MEASURE_COMMAND);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);

    steps[0] = write_step(cmd_measure);
    steps[1] = wait_step();
    steps[1].fail_errno = EINVAL;
    s = expect_measure_failure(steps, 2, BH1750_STAGE_WAIT);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == EINVAL);

    steps[1] = wait_step();
    steps[2] = read_step(example_bytes);
    steps[2].fail_errno = ENXIO;
    s = expect_measure_failure(steps, 3, BH1750_STAGE_READ);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == ENXIO);

    /* One byte arrives: still a failure, and the partial byte is unused. */
    steps[2] = read_step(example_bytes);
    steps[2].transfer_len = 1;
    s = expect_measure_failure(steps, 3, BH1750_STAGE_READ);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
    assert(s.io.requested_len == 2 && s.io.transferred_len == 1);

    steps[2] = read_step(NULL);
    steps[2].transfer_len = 0;
    s = expect_measure_failure(steps, 3, BH1750_STAGE_READ);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
    assert(s.io.transferred_len == 0);
}

/*
 * test_arguments_rejected: NULL arguments, a closed device, and any
 * address other than 0x23 or 0x5c are refused before any I/O.
 */
static void test_arguments_rejected(void)
{
    static const uint8_t other_addresses[] = {
        0x00, 0x22, 0x24, 0x36, 0x5b, 0x5d, 0x7f
    };
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;
    bh1750_status s;
    size_t i;

    s = bh1750_init(NULL);
    assert(s.stage == BH1750_STAGE_ARGUMENT);
    assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
    s = bh1750_measure_once_h(NULL, &out);
    assert(s.stage == BH1750_STAGE_ARGUMENT && reading_is_sentinel(&out));
    s = bh1750_init(&dev);
    assert(s.stage == BH1750_STAGE_ARGUMENT);
    s = bh1750_measure_once_h(&dev, &out);
    assert(s.stage == BH1750_STAGE_ARGUMENT && reading_is_sentinel(&out));

    /* Closed after being open at a valid address: refused by ops, not by
     * the address check. */
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, NULL, 0);
    close_consumed(&dev);
    assert(dev.ops == NULL && dev.address_7bit == BH1750_ADDRESS_ADDR_LOW);
    s = bh1750_init(&dev);
    assert(s.stage == BH1750_STAGE_ARGUMENT);
    assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
    s = bh1750_measure_once_h(&dev, &out);
    assert(s.stage == BH1750_STAGE_ARGUMENT && reading_is_sentinel(&out));

    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, NULL, 0);
    s = bh1750_measure_once_h(&dev, NULL);
    assert(s.stage == BH1750_STAGE_ARGUMENT);
    assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
    close_consumed(&dev);

    for (i = 0; i < COUNT_OF(other_addresses); i++) {
        open_scripted(&mock, &dev, other_addresses[i], NULL, 0);
        s = bh1750_init(&dev);
        assert(s.stage == BH1750_STAGE_ARGUMENT);
        assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
        s = bh1750_measure_once_h(&dev, &out);
        assert(s.stage == BH1750_STAGE_ARGUMENT && reading_is_sentinel(&out));
        close_consumed(&dev);
    }
}

/*
 * test_simulation_label_copied: the reading takes is_simulated from the
 * device rather than assuming it. The mock always opens simulated devices,
 * so the flag is cleared by hand to show it is copied, not hard-coded.
 */
static void test_simulation_label_copied(void)
{
    i2c_mock_step steps[3];
    i2c_mock mock;
    i2c_device dev = {0};
    bh1750_reading out = reading_sentinel;

    steps[0] = write_step(cmd_measure);
    steps[1] = wait_step();
    steps[2] = read_step(example_bytes);
    open_scripted(&mock, &dev, BH1750_ADDRESS_ADDR_LOW, steps, 3);
    dev.is_simulated = 0;
    assert(bh1750_measure_once_h(&dev, &out).stage == BH1750_STAGE_NONE);
    assert(out.is_simulated == 0);
    close_consumed(&dev);
}

/* test_stage_names: every stage has stable log text. */
static void test_stage_names(void)
{
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_NONE), "NONE") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_ARGUMENT), "ARGUMENT") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_POWER_ON), "POWER_ON") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_MTREG_HIGH),
                  "MTREG_HIGH") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_MTREG_LOW),
                  "MTREG_LOW") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_MEASURE_COMMAND),
                  "MEASURE_COMMAND") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_WAIT), "WAIT") == 0);
    assert(strcmp(bh1750_stage_name(BH1750_STAGE_READ), "READ") == 0);
    assert(strcmp(bh1750_stage_name((bh1750_stage)99), "UNKNOWN") == 0);
}

/* main: run every SIMULATED driver test; abort on the first failure. */
int main(void)
{
    test_raw_to_mlx_vectors();
    test_init_sends_documented_commands();
    test_init_stops_at_each_failure();
    test_measure_example();
    test_byte_order();
    test_init_then_measure();
    test_measure_stops_at_each_failure();
    test_arguments_rejected();
    test_simulation_label_copied();
    test_stage_names();

    puts("SIMULATED bh1750 driver tests passed (scripted mock; "
         "no sensor hardware used)");
    return 0;
}
