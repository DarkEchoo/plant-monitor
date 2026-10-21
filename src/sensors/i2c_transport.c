#include "sensors/i2c_transport.h"

#include <stddef.h>

/* make_result: build a result for an operation that moved no bytes. */
static i2c_result make_result(i2c_status status, size_t requested_len)
{
    i2c_result result = {status, 0, requested_len, 0};
    return result;
}

/*
 * finish_transfer: refuse a backend "success" whose byte count differs from
 * the request, so a caller can never mistake partial data for a full read.
 */
static i2c_result finish_transfer(i2c_result result, size_t requested_len)
{
    result.requested_len = requested_len;
    if (result.status == I2C_STATUS_OK &&
        result.transferred_len != requested_len) {
        result.status = I2C_STATUS_SHORT_TRANSFER;
    }
    return result;
}

/* device_is_open: true while the device can still accept operations. */
static int device_is_open(const i2c_device *dev)
{
    return dev != NULL && dev->ops != NULL;
}

/* i2c_device_write: send one write transaction ending in STOP. */
i2c_result i2c_device_write(i2c_device *dev, const uint8_t *buf, size_t len)
{
    if (!device_is_open(dev) || dev->ops->write == NULL || buf == NULL ||
        len == 0) {
        return make_result(I2C_STATUS_INVALID_ARGUMENT, len);
    }
    return finish_transfer(dev->ops->write(dev->ctx, buf, len), len);
}

/* i2c_device_read: receive one read transaction ending in STOP. */
i2c_result i2c_device_read(i2c_device *dev, uint8_t *buf, size_t len)
{
    if (!device_is_open(dev) || dev->ops->read == NULL || buf == NULL ||
        len == 0) {
        return make_result(I2C_STATUS_INVALID_ARGUMENT, len);
    }
    return finish_transfer(dev->ops->read(dev->ctx, buf, len), len);
}

/* i2c_device_wait_us: wait at least duration_us between transactions. */
i2c_result i2c_device_wait_us(i2c_device *dev, uint32_t duration_us)
{
    i2c_result result;

    if (!device_is_open(dev) || dev->ops->wait_us == NULL) {
        return make_result(I2C_STATUS_INVALID_ARGUMENT, 0);
    }
    result = dev->ops->wait_us(dev->ctx, duration_us);
    result.requested_len = 0;
    result.transferred_len = 0;
    return result;
}

/*
 * i2c_device_close: release the backend. The device is marked closed even if
 * the backend reports an error, matching close(2), which releases the
 * descriptor on Linux even when it fails. A device with no close callback
 * is refused and left open: marking it closed would orphan its resource.
 */
i2c_result i2c_device_close(i2c_device *dev)
{
    i2c_result result;

    if (!device_is_open(dev) || dev->ops->close == NULL) {
        return make_result(I2C_STATUS_INVALID_ARGUMENT, 0);
    }
    result = dev->ops->close(dev->ctx);
    dev->ops = NULL;
    dev->ctx = NULL;
    return result;
}

/* i2c_status_name: stable text for logs and diagnostic output. */
const char *i2c_status_name(i2c_status status)
{
    switch (status) {
    case I2C_STATUS_OK:
        return "OK";
    case I2C_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case I2C_STATUS_OS_ERROR:
        return "OS_ERROR";
    case I2C_STATUS_SHORT_TRANSFER:
        return "SHORT_TRANSFER";
    case I2C_STATUS_SCRIPT_MISMATCH:
        return "SCRIPT_MISMATCH";
    default:
        return "UNKNOWN";
    }
}
