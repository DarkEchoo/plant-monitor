#include "sensors/bh1750.h"

#include <stddef.h>

/* bh1750_ok: the status of a call that completed every step. */
static bh1750_status bh1750_ok(void)
{
    bh1750_status s = {{I2C_STATUS_OK, 0, 0, 0}, BH1750_STAGE_NONE};
    return s;
}

/* bh1750_failed: the status for a step that failed with io. */
static bh1750_status bh1750_failed(i2c_result io, bh1750_stage stage)
{
    bh1750_status s;

    s.io = io;
    s.stage = stage;
    return s;
}

/* bh1750_bad_argument: the status for a call refused before any I/O. */
static bh1750_status bh1750_bad_argument(void)
{
    i2c_result io = {I2C_STATUS_INVALID_ARGUMENT, 0, 0, 0};
    return bh1750_failed(io, BH1750_STAGE_ARGUMENT);
}

/*
 * device_usable: true if dev is open at one of the two addresses the
 * BH1750 can have, so its opcodes never go to another device on the bus.
 */
static int device_usable(const i2c_device *dev)
{
    return dev != NULL && dev->ops != NULL &&
           (dev->address_7bit == BH1750_ADDRESS_ADDR_LOW ||
            dev->address_7bit == BH1750_ADDRESS_ADDR_HIGH);
}

/* send_command: write one opcode as its own STOP-terminated transaction. */
static i2c_result send_command(i2c_device *dev, uint8_t opcode)
{
    return i2c_device_write(dev, &opcode, 1);
}

/*
 * bh1750_init: Power On, then set MTreg to 69 in two writes (high bits,
 * low bits). Stops at the first write that does not fully succeed.
 */
bh1750_status bh1750_init(i2c_device *dev)
{
    static const struct {
        uint8_t opcode;
        bh1750_stage stage;
    } commands[] = {
        {BH1750_CMD_POWER_ON, BH1750_STAGE_POWER_ON},
        {BH1750_CMD_MTREG_HIGH_69, BH1750_STAGE_MTREG_HIGH},
        {BH1750_CMD_MTREG_LOW_69, BH1750_STAGE_MTREG_LOW}
    };
    size_t i;

    if (!device_usable(dev)) {
        return bh1750_bad_argument();
    }
    for (i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        i2c_result io = send_command(dev, commands[i].opcode);

        if (io.status != I2C_STATUS_OK) {
            return bh1750_failed(io, commands[i].stage);
        }
    }
    return bh1750_ok();
}

/*
 * bh1750_measure_once_h: command, wait the documented maximum, read two
 * bytes. *out is written only after the read fully succeeds.
 */
bh1750_status bh1750_measure_once_h(i2c_device *dev, bh1750_reading *out)
{
    uint8_t bytes[2];
    uint16_t raw_count;
    i2c_result io;

    if (!device_usable(dev) || out == NULL) {
        return bh1750_bad_argument();
    }

    io = send_command(dev, BH1750_CMD_ONE_TIME_H_RES);
    if (io.status != I2C_STATUS_OK) {
        return bh1750_failed(io, BH1750_STAGE_MEASURE_COMMAND);
    }
    io = i2c_device_wait_us(dev, BH1750_H_RES_MAX_WAIT_US);
    if (io.status != I2C_STATUS_OK) {
        return bh1750_failed(io, BH1750_STAGE_WAIT);
    }
    io = i2c_device_read(dev, bytes, sizeof bytes);
    if (io.status != I2C_STATUS_OK) {
        return bh1750_failed(io, BH1750_STAGE_READ);
    }

    raw_count = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    out->raw_count = raw_count;
    out->illuminance_mlx = bh1750_raw_to_mlx(raw_count);
    out->is_simulated = dev->is_simulated;
    return bh1750_ok();
}

/* bh1750_raw_to_mlx: counts / 1.2 in millilux, rounded to nearest. */
uint32_t bh1750_raw_to_mlx(uint16_t raw_count)
{
    return ((uint32_t)raw_count * 2500u + 1u) / 3u;
}

/* bh1750_stage_name: stable text for logs and diagnostic output. */
const char *bh1750_stage_name(bh1750_stage stage)
{
    switch (stage) {
    case BH1750_STAGE_NONE:
        return "NONE";
    case BH1750_STAGE_ARGUMENT:
        return "ARGUMENT";
    case BH1750_STAGE_POWER_ON:
        return "POWER_ON";
    case BH1750_STAGE_MTREG_HIGH:
        return "MTREG_HIGH";
    case BH1750_STAGE_MTREG_LOW:
        return "MTREG_LOW";
    case BH1750_STAGE_MEASURE_COMMAND:
        return "MEASURE_COMMAND";
    case BH1750_STAGE_WAIT:
        return "WAIT";
    case BH1750_STAGE_READ:
        return "READ";
    default:
        return "UNKNOWN";
    }
}
