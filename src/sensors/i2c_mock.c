#include "sensors/i2c_mock.h"

#include <string.h>

/* mock_result: build the outcome of one scripted operation. */
static i2c_result mock_result(i2c_status status, int os_errno,
                              size_t requested_len, size_t transferred_len)
{
    i2c_result result = {status, os_errno, requested_len, transferred_len};
    return result;
}

/* mock_flag_mismatch: record the first unexpected operation. */
static void mock_flag_mismatch(i2c_mock *mock)
{
    if (!mock->mismatch) {
        mock->mismatch = 1;
        mock->mismatch_step = mock->next_step;
    }
}

/* mock_peek_step: next step if it has the requested op, otherwise NULL. */
static const i2c_mock_step *mock_peek_step(const i2c_mock *mock,
                                           i2c_mock_op op)
{
    const i2c_mock_step *step;

    if (mock->mismatch || mock->next_step >= mock->step_count) {
        return NULL;
    }
    step = &mock->steps[mock->next_step];
    return step->op == op ? step : NULL;
}

/* mock_write: check a write against the scripted WRITE step. */
static i2c_result mock_write(void *ctx, const uint8_t *buf, size_t len)
{
    i2c_mock *mock = ctx;
    const i2c_mock_step *step = mock_peek_step(mock, I2C_MOCK_WRITE);

    if (step == NULL || step->bytes == NULL || step->len != len ||
        step->transfer_len > len || memcmp(step->bytes, buf, len) != 0) {
        mock_flag_mismatch(mock);
        return mock_result(I2C_STATUS_SCRIPT_MISMATCH, 0, len, 0);
    }
    mock->next_step++;
    if (step->fail_errno != 0) {
        return mock_result(I2C_STATUS_OS_ERROR, step->fail_errno, len, 0);
    }
    return mock_result(I2C_STATUS_OK, 0, len, step->transfer_len);
}

/* mock_read: return the scripted READ bytes, possibly fewer than asked. */
static i2c_result mock_read(void *ctx, uint8_t *buf, size_t len)
{
    i2c_mock *mock = ctx;
    const i2c_mock_step *step = mock_peek_step(mock, I2C_MOCK_READ);

    if (step == NULL || step->len != len || step->transfer_len > len ||
        (step->transfer_len > 0 && step->bytes == NULL)) {
        mock_flag_mismatch(mock);
        return mock_result(I2C_STATUS_SCRIPT_MISMATCH, 0, len, 0);
    }
    mock->next_step++;
    if (step->fail_errno != 0) {
        return mock_result(I2C_STATUS_OS_ERROR, step->fail_errno, len, 0);
    }
    if (step->transfer_len > 0) {
        memcpy(buf, step->bytes, step->transfer_len);
    }
    return mock_result(I2C_STATUS_OK, 0, len, step->transfer_len);
}

/* mock_wait_us: record a wait and check it meets the scripted minimum. */
static i2c_result mock_wait_us(void *ctx, uint32_t duration_us)
{
    i2c_mock *mock = ctx;
    const i2c_mock_step *step = mock_peek_step(mock, I2C_MOCK_WAIT);

    if (step == NULL || duration_us < step->min_wait_us) {
        mock_flag_mismatch(mock);
        return mock_result(I2C_STATUS_SCRIPT_MISMATCH, 0, 0, 0);
    }
    mock->next_step++;
    if (step->fail_errno != 0) {
        return mock_result(I2C_STATUS_OS_ERROR, step->fail_errno, 0, 0);
    }
    mock->waited_us += duration_us;
    return mock_result(I2C_STATUS_OK, 0, 0, 0);
}

/* mock_close: fail if the script was not consumed exactly. */
static i2c_result mock_close(void *ctx)
{
    i2c_mock *mock = ctx;

    if (mock->next_step != mock->step_count) {
        mock_flag_mismatch(mock);
    }
    return mock_result(mock->mismatch ? I2C_STATUS_SCRIPT_MISMATCH
                                      : I2C_STATUS_OK,
                       0, 0, 0);
}

static const i2c_device_ops mock_ops = {
    mock_write,
    mock_read,
    mock_wait_us,
    mock_close
};

/*
 * i2c_mock_open: bind a simulated device to a borrowed step script. Refuses
 * a device that is still open, as the Linux backend will, so tests catch a
 * missing close that would leak a descriptor on real hardware.
 */
i2c_status i2c_mock_open(i2c_mock *mock, i2c_device *dev,
                         uint8_t address_7bit,
                         const i2c_mock_step *steps, size_t step_count)
{
    if (mock == NULL || dev == NULL || dev->ops != NULL ||
        (steps == NULL && step_count > 0)) {
        return I2C_STATUS_INVALID_ARGUMENT;
    }
    mock->steps = steps;
    mock->step_count = step_count;
    mock->next_step = 0;
    mock->waited_us = 0;
    mock->mismatch = 0;
    mock->mismatch_step = 0;

    dev->ops = &mock_ops;
    dev->ctx = mock;
    dev->address_7bit = address_7bit;
    dev->is_simulated = 1;
    return I2C_STATUS_OK;
}
