#include "sensors/i2c_mock.h"
#include "sensors/i2c_transport.h"
#include "sensors/seesaw_soil.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * SIMULATED: every test drives the soil-sensor driver through the scripted
 * I2C mock. No sensor or bus is used, and no value here is a measurement.
 * Bytes and waits come from Adafruit's seesaw firmware and libraries.
 */

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define INIT_STEPS 5
#define MEASURE_STEPS 3

static const uint8_t cmd_reset[] = {0x00, 0x7f, 0xff};
static const uint8_t cmd_hw_id[] = {0x00, 0x01};
static const uint8_t cmd_touch0[] = {0x0f, 0x10};
static const uint8_t id_samd[] = {0x55};
static const uint8_t value_500[] = {0x01, 0xf4};

/* A reading no driver would produce, to show failures leave *out alone. */
static const soil_reading reading_sentinel = {0xBEEF, 7};

/* reading_is_sentinel: true if every field still holds the sentinel. */
static int reading_is_sentinel(const soil_reading *r)
{
    return r->capacitance_counts == reading_sentinel.capacitance_counts &&
           r->is_simulated == reading_sentinel.is_simulated;
}

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

/* init_script: reset, wait, select HW_ID, wait, read the ID. */
static void init_script(i2c_mock_step steps[INIT_STEPS], const uint8_t *id)
{
    steps[0] = write_step(cmd_reset, sizeof cmd_reset);
    steps[1] = wait_step(500000);
    steps[2] = write_step(cmd_hw_id, sizeof cmd_hw_id);
    steps[3] = wait_step(8000);
    steps[4] = read_step(id, 1);
}

/* measure_script: select touch channel 0, wait, read two bytes. */
static void measure_script(i2c_mock_step steps[MEASURE_STEPS],
                           const uint8_t *value)
{
    steps[0] = write_step(cmd_touch0, sizeof cmd_touch0);
    steps[1] = wait_step(5000);
    steps[2] = read_step(value, 2);
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
 * step and nothing more, so continuing after a failure, or stopping early,
 * fails here.
 */
static void close_consumed(i2c_device *dev)
{
    assert(i2c_device_close(dev).status == I2C_STATUS_OK);
}

/* test_init_sends_documented_sequence: exact bytes, order and waits. */
static void test_init_sends_documented_sequence(void)
{
    i2c_mock_step steps[INIT_STEPS];
    i2c_mock mock;
    i2c_device dev = {0};
    soil_status s;

    init_script(steps, id_samd);
    open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, INIT_STEPS);
    s = soil_init(&dev);
    assert(s.stage == SOIL_STAGE_NONE && s.io.status == I2C_STATUS_OK);
    assert(s.rejected_value == 0);
    assert(mock.next_step == INIT_STEPS);
    assert(mock.waited_us == 500000 + 8000);
    close_consumed(&dev);
}

/*
 * test_init_stops_at_each_failure: an OS error at each of the five init
 * steps, and a short transfer at each of the three transfers, is reported
 * with that step's stage, and nothing after it runs.
 */
static void test_init_stops_at_each_failure(void)
{
    static const soil_stage stages[INIT_STEPS] = {
        SOIL_STAGE_RESET, SOIL_STAGE_RESET_WAIT, SOIL_STAGE_HW_ID_SELECT,
        SOIL_STAGE_HW_ID_WAIT, SOIL_STAGE_HW_ID_READ
    };
    size_t k;
    int short_transfer;

    for (k = 0; k < INIT_STEPS; k++) {
        for (short_transfer = 0; short_transfer <= 1; short_transfer++) {
            i2c_mock_step steps[INIT_STEPS];
            i2c_mock mock;
            i2c_device dev = {0};
            soil_status s;

            init_script(steps, id_samd);
            if (short_transfer) {
                if (steps[k].op == I2C_MOCK_WAIT) {
                    continue;
                }
                steps[k].transfer_len = steps[k].len - 1;
            } else {
                steps[k].fail_errno = EIO;
            }
            open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, k + 1);
            s = soil_init(&dev);
            assert(s.stage == stages[k]);
            assert(s.rejected_value == 0);
            if (short_transfer) {
                assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
                assert(s.io.transferred_len == steps[k].len - 1);
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
 * test_init_rejects_other_ids: a successful read of every ID except 0x55
 * is SOIL_STAGE_HW_ID, reporting the ID that was read. Checking all 255
 * catches a masked comparison or an extra accepted value. (0x87 is the
 * ATtiny817 seesaw code; 0x00 and 0xFF are what a floating or held bus
 * might return.)
 */
static void test_init_rejects_other_ids(void)
{
    unsigned id;

    for (id = 0; id <= 0xff; id++) {
        const uint8_t id_byte = (uint8_t)id;
        i2c_mock_step steps[INIT_STEPS];
        i2c_mock mock;
        i2c_device dev = {0};
        soil_status s;

        init_script(steps, &id_byte);
        open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, INIT_STEPS);
        s = soil_init(&dev);
        assert(s.io.status == I2C_STATUS_OK);
        if (id_byte == SOIL_HW_ID_SAMD_SEESAW) {
            assert(s.stage == SOIL_STAGE_NONE && s.rejected_value == 0);
        } else {
            assert(s.stage == SOIL_STAGE_HW_ID);
            assert(s.rejected_value == id_byte);
        }
        close_consumed(&dev);
    }
}

/*
 * test_measure_decodes_big_endian: 0x01F4 is 500 counts, after a wait of
 * at least 5 ms; {0x01, 0x02} is 258, not 513.
 */
static void test_measure_decodes_big_endian(void)
{
    static const uint8_t value_258[] = {0x01, 0x02};
    const uint8_t *values[] = {value_500, value_258};
    const uint16_t expected[] = {500, 258};
    size_t i;

    for (i = 0; i < COUNT_OF(values); i++) {
        i2c_mock_step steps[MEASURE_STEPS];
        i2c_mock mock;
        i2c_device dev = {0};
        soil_reading out = reading_sentinel;
        soil_status s;

        measure_script(steps, values[i]);
        open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, MEASURE_STEPS);
        s = soil_measure_capacitance(&dev, &out);
        assert(s.stage == SOIL_STAGE_NONE && s.io.status == I2C_STATUS_OK);
        assert(s.rejected_value == 0);
        assert(out.capacitance_counts == expected[i]);
        assert(out.is_simulated == 1);
        assert(mock.waited_us == 5000);
        close_consumed(&dev);
    }
}

/*
 * test_measure_range_boundary: every 16-bit value is decoded high byte
 * first; 0-4095 are accepted and 4096-65535 are SOIL_STAGE_RANGE,
 * reporting the value, with the output untouched.
 */
static void test_measure_range_boundary(void)
{
    unsigned value;

    for (value = 0; value <= 0xffff; value++) {
        const uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
        i2c_mock_step steps[MEASURE_STEPS];
        i2c_mock mock;
        i2c_device dev = {0};
        soil_reading out = reading_sentinel;
        soil_status s;

        measure_script(steps, bytes);
        open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, MEASURE_STEPS);
        s = soil_measure_capacitance(&dev, &out);
        assert(s.io.status == I2C_STATUS_OK);
        if (value <= 4095) {
            assert(s.stage == SOIL_STAGE_NONE && s.rejected_value == 0);
            assert(out.capacitance_counts == value);
        } else {
            assert(s.stage == SOIL_STAGE_RANGE);
            assert(s.rejected_value == value);
            assert(reading_is_sentinel(&out));
        }
        close_consumed(&dev);
    }
    assert(SOIL_CAPACITANCE_MAX_VALID_COUNTS == 4095u);
}

/*
 * expect_measure_failure: run a measurement against steps (which end at
 * the failing step), check the stage, that *out is untouched, and that
 * nothing after the failure was attempted.
 */
static soil_status expect_measure_failure(const i2c_mock_step *steps,
                                          size_t step_count,
                                          soil_stage stage)
{
    i2c_mock mock;
    i2c_device dev = {0};
    soil_reading out = reading_sentinel;
    soil_status s;

    open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, step_count);
    s = soil_measure_capacitance(&dev, &out);
    assert(s.stage == stage);
    assert(s.rejected_value == 0);
    assert(reading_is_sentinel(&out));
    assert(mock.next_step == step_count);
    close_consumed(&dev);
    return s;
}

/* test_measure_stops_at_each_failure: select, wait and read failures. */
static void test_measure_stops_at_each_failure(void)
{
    i2c_mock_step steps[MEASURE_STEPS];
    soil_status s;

    measure_script(steps, value_500);
    steps[0].fail_errno = EIO;
    s = expect_measure_failure(steps, 1, SOIL_STAGE_MEASURE_SELECT);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == EIO);

    measure_script(steps, value_500);
    steps[0].transfer_len = 1;
    s = expect_measure_failure(steps, 1, SOIL_STAGE_MEASURE_SELECT);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);

    measure_script(steps, value_500);
    steps[1].fail_errno = EINVAL;
    s = expect_measure_failure(steps, 2, SOIL_STAGE_MEASURE_WAIT);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == EINVAL);

    measure_script(steps, value_500);
    steps[2].fail_errno = ENXIO;
    s = expect_measure_failure(steps, 3, SOIL_STAGE_MEASURE_READ);
    assert(s.io.status == I2C_STATUS_OS_ERROR && s.io.os_errno == ENXIO);

    /* One byte arrives: still a failure, and the partial byte is unused. */
    measure_script(steps, value_500);
    steps[2].transfer_len = 1;
    s = expect_measure_failure(steps, 3, SOIL_STAGE_MEASURE_READ);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
    assert(s.io.requested_len == 2 && s.io.transferred_len == 1);

    measure_script(steps, NULL);
    steps[2].transfer_len = 0;
    s = expect_measure_failure(steps, 3, SOIL_STAGE_MEASURE_READ);
    assert(s.io.status == I2C_STATUS_SHORT_TRANSFER);
    assert(s.io.transferred_len == 0);
}

/* test_addresses: 0x36-0x39 work; nothing else reaches the bus. */
static void test_addresses(void)
{
    static const uint8_t other_addresses[] = {
        0x00, 0x23, 0x35, 0x3a, 0x49, 0x5c, 0x7f
    };
    uint8_t address;
    size_t i;

    for (address = SOIL_ADDRESS_MIN; address <= SOIL_ADDRESS_MAX; address++) {
        i2c_mock_step steps[INIT_STEPS + MEASURE_STEPS];
        i2c_mock mock;
        i2c_device dev = {0};
        soil_reading out = reading_sentinel;

        init_script(steps, id_samd);
        measure_script(steps + INIT_STEPS, value_500);
        open_scripted(&mock, &dev, address, steps, COUNT_OF(steps));
        assert(soil_init(&dev).stage == SOIL_STAGE_NONE);
        assert(soil_measure_capacitance(&dev, &out).stage == SOIL_STAGE_NONE);
        assert(out.capacitance_counts == 500);
        close_consumed(&dev);
    }

    for (i = 0; i < COUNT_OF(other_addresses); i++) {
        i2c_mock mock;
        i2c_device dev = {0};
        soil_reading out = reading_sentinel;
        soil_status s;

        open_scripted(&mock, &dev, other_addresses[i], NULL, 0);
        s = soil_init(&dev);
        assert(s.stage == SOIL_STAGE_ARGUMENT);
        assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
        s = soil_measure_capacitance(&dev, &out);
        assert(s.stage == SOIL_STAGE_ARGUMENT && reading_is_sentinel(&out));
        close_consumed(&dev);
    }
}

/*
 * test_arguments_rejected: NULL arguments, a zeroed device, and a device
 * closed after being open at a valid address are refused before any I/O.
 */
static void test_arguments_rejected(void)
{
    i2c_mock mock;
    i2c_device dev = {0};
    soil_reading out = reading_sentinel;
    soil_status s;

    s = soil_init(NULL);
    assert(s.stage == SOIL_STAGE_ARGUMENT);
    assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
    s = soil_measure_capacitance(NULL, &out);
    assert(s.stage == SOIL_STAGE_ARGUMENT && reading_is_sentinel(&out));
    s = soil_init(&dev);
    assert(s.stage == SOIL_STAGE_ARGUMENT);

    open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, NULL, 0);
    close_consumed(&dev);
    assert(dev.ops == NULL && dev.address_7bit == SOIL_ADDRESS_MIN);
    s = soil_init(&dev);
    assert(s.stage == SOIL_STAGE_ARGUMENT);
    s = soil_measure_capacitance(&dev, &out);
    assert(s.stage == SOIL_STAGE_ARGUMENT && reading_is_sentinel(&out));

    open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, NULL, 0);
    s = soil_measure_capacitance(&dev, NULL);
    assert(s.stage == SOIL_STAGE_ARGUMENT);
    assert(s.io.status == I2C_STATUS_INVALID_ARGUMENT);
    close_consumed(&dev);
}

/*
 * test_simulation_label_copied: the reading takes is_simulated from the
 * device. The mock always opens simulated devices, so the flag is cleared
 * by hand to show it is copied, not hard-coded.
 */
static void test_simulation_label_copied(void)
{
    i2c_mock_step steps[MEASURE_STEPS];
    i2c_mock mock;
    i2c_device dev = {0};
    soil_reading out = reading_sentinel;

    measure_script(steps, value_500);
    open_scripted(&mock, &dev, SOIL_ADDRESS_MIN, steps, MEASURE_STEPS);
    dev.is_simulated = 0;
    assert(soil_measure_capacitance(&dev, &out).stage == SOIL_STAGE_NONE);
    assert(out.is_simulated == 0);
    close_consumed(&dev);
}

/* test_stage_names: every stage has stable log text. */
static void test_stage_names(void)
{
    static const struct {
        soil_stage stage;
        const char *name;
    } names[] = {
        {SOIL_STAGE_NONE, "NONE"},
        {SOIL_STAGE_ARGUMENT, "ARGUMENT"},
        {SOIL_STAGE_RESET, "RESET"},
        {SOIL_STAGE_RESET_WAIT, "RESET_WAIT"},
        {SOIL_STAGE_HW_ID_SELECT, "HW_ID_SELECT"},
        {SOIL_STAGE_HW_ID_WAIT, "HW_ID_WAIT"},
        {SOIL_STAGE_HW_ID_READ, "HW_ID_READ"},
        {SOIL_STAGE_HW_ID, "HW_ID"},
        {SOIL_STAGE_MEASURE_SELECT, "MEASURE_SELECT"},
        {SOIL_STAGE_MEASURE_WAIT, "MEASURE_WAIT"},
        {SOIL_STAGE_MEASURE_READ, "MEASURE_READ"},
        {SOIL_STAGE_RANGE, "RANGE"},
        {(soil_stage)99, "UNKNOWN"}
    };
    size_t i;

    for (i = 0; i < COUNT_OF(names); i++) {
        assert(strcmp(soil_stage_name(names[i].stage), names[i].name) == 0);
    }
}

/* main: run every SIMULATED driver test; abort on the first failure. */
int main(void)
{
    test_init_sends_documented_sequence();
    test_init_stops_at_each_failure();
    test_init_rejects_other_ids();
    test_measure_decodes_big_endian();
    test_measure_range_boundary();
    test_measure_stops_at_each_failure();
    test_addresses();
    test_arguments_rejected();
    test_simulation_label_copied();
    test_stage_names();

    puts("SIMULATED seesaw soil driver tests passed (scripted mock; "
         "no sensor hardware used)");
    return 0;
}
