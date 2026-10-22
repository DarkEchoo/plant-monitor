#include "sensors/seesaw_soil.h"

#include <stddef.h>

/* soil_ok: the status of a call that completed every step. */
static soil_status soil_ok(void)
{
    soil_status s = {{I2C_STATUS_OK, 0, 0, 0}, SOIL_STAGE_NONE, 0};
    return s;
}

/* soil_failed: the status for a step that failed or was rejected. */
static soil_status soil_failed(i2c_result io, soil_stage stage,
                               uint16_t rejected_value)
{
    soil_status s;

    s.io = io;
    s.stage = stage;
    s.rejected_value = rejected_value;
    return s;
}

/* soil_bad_argument: the status for a call refused before any I/O. */
static soil_status soil_bad_argument(void)
{
    i2c_result io = {I2C_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    return soil_failed(io, SOIL_STAGE_ARGUMENT, 0);
}

/*
 * device_usable: true if dev is open at one of the board's four jumper
 * addresses, so seesaw commands never go to another device on the bus.
 */
static int device_usable(const i2c_device *dev)
{
    return dev != NULL && dev->ops != NULL &&
           dev->address_7bit >= SOIL_ADDRESS_MIN &&
           dev->address_7bit <= SOIL_ADDRESS_MAX;
}

/*
 * register_read: select a seesaw register, wait, then read len bytes, each
 * a separate transaction. On failure, reports the failing step's stage
 * through *stage and returns that step's result.
 */
static i2c_result register_read(i2c_device *dev, uint8_t base, uint8_t reg,
                                uint32_t wait_us, uint8_t *buf, size_t len,
                                const soil_stage stages[3],
                                soil_stage *stage)
{
    const uint8_t select[2] = {base, reg};
    i2c_result io;

    io = i2c_device_write(dev, select, sizeof select);
    if (io.status != I2C_STATUS_OK) {
        *stage = stages[0];
        return io;
    }
    io = i2c_device_wait_us(dev, wait_us);
    if (io.status != I2C_STATUS_OK) {
        *stage = stages[1];
        return io;
    }
    io = i2c_device_read(dev, buf, len);
    if (io.status != I2C_STATUS_OK) {
        *stage = stages[2];
    }
    return io;
}

/*
 * soil_init: software reset, wait for the firmware to restart, then read
 * the hardware ID and require the SAMD seesaw code.
 */
soil_status soil_init(i2c_device *dev)
{
    static const uint8_t reset[3] = {
        SOIL_STATUS_BASE, SOIL_STATUS_SWRST, SOIL_SWRST_VALUE
    };
    static const soil_stage id_stages[3] = {
        SOIL_STAGE_HW_ID_SELECT, SOIL_STAGE_HW_ID_WAIT, SOIL_STAGE_HW_ID_READ
    };
    soil_stage stage = SOIL_STAGE_NONE;
    uint8_t id;
    i2c_result io;

    if (!device_usable(dev)) {
        return soil_bad_argument();
    }

    io = i2c_device_write(dev, reset, sizeof reset);
    if (io.status != I2C_STATUS_OK) {
        return soil_failed(io, SOIL_STAGE_RESET, 0);
    }
    io = i2c_device_wait_us(dev, SOIL_RESET_WAIT_US);
    if (io.status != I2C_STATUS_OK) {
        return soil_failed(io, SOIL_STAGE_RESET_WAIT, 0);
    }

    io = register_read(dev, SOIL_STATUS_BASE, SOIL_STATUS_HW_ID,
                       SOIL_HW_ID_WAIT_US, &id, 1, id_stages, &stage);
    if (io.status != I2C_STATUS_OK) {
        return soil_failed(io, stage, 0);
    }
    if (id != SOIL_HW_ID_SAMD_SEESAW) {
        return soil_failed(io, SOIL_STAGE_HW_ID, id);
    }
    return soil_ok();
}

/*
 * soil_measure_capacitance: read touch channel 0, high byte first. *out is
 * written only after a complete read of an in-range value.
 */
soil_status soil_measure_capacitance(i2c_device *dev, soil_reading *out)
{
    static const soil_stage measure_stages[3] = {
        SOIL_STAGE_MEASURE_SELECT, SOIL_STAGE_MEASURE_WAIT,
        SOIL_STAGE_MEASURE_READ
    };
    soil_stage stage = SOIL_STAGE_NONE;
    uint8_t bytes[2];
    uint16_t counts;
    i2c_result io;

    if (!device_usable(dev) || out == NULL) {
        return soil_bad_argument();
    }

    io = register_read(dev, SOIL_TOUCH_BASE, SOIL_TOUCH_CHANNEL_0,
                       SOIL_MEASURE_WAIT_US, bytes, sizeof bytes,
                       measure_stages, &stage);
    if (io.status != I2C_STATUS_OK) {
        return soil_failed(io, stage, 0);
    }

    counts = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    if (counts > SOIL_CAPACITANCE_MAX_VALID_COUNTS) {
        return soil_failed(io, SOIL_STAGE_RANGE, counts);
    }
    out->capacitance_counts = counts;
    out->is_simulated = dev->is_simulated;
    return soil_ok();
}

/* soil_stage_name: stable text for logs and diagnostic output. */
const char *soil_stage_name(soil_stage stage)
{
    switch (stage) {
    case SOIL_STAGE_NONE:
        return "NONE";
    case SOIL_STAGE_ARGUMENT:
        return "ARGUMENT";
    case SOIL_STAGE_RESET:
        return "RESET";
    case SOIL_STAGE_RESET_WAIT:
        return "RESET_WAIT";
    case SOIL_STAGE_HW_ID_SELECT:
        return "HW_ID_SELECT";
    case SOIL_STAGE_HW_ID_WAIT:
        return "HW_ID_WAIT";
    case SOIL_STAGE_HW_ID_READ:
        return "HW_ID_READ";
    case SOIL_STAGE_HW_ID:
        return "HW_ID";
    case SOIL_STAGE_MEASURE_SELECT:
        return "MEASURE_SELECT";
    case SOIL_STAGE_MEASURE_WAIT:
        return "MEASURE_WAIT";
    case SOIL_STAGE_MEASURE_READ:
        return "MEASURE_READ";
    case SOIL_STAGE_RANGE:
        return "RANGE";
    default:
        return "UNKNOWN";
    }
}
