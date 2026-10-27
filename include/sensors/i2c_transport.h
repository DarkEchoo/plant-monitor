#ifndef SENSORS_I2C_TRANSPORT_H
#define SENSORS_I2C_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * One i2c_device is one 7-bit address on one bus. Each write or read is a
 * single I2C transaction that ends in STOP. There is no combined
 * repeated-start operation: the BH1750 datasheet requires a STOP after every
 * opcode, and the seesaw protocol needs a delay between the register write
 * and the data read, so neither sensor uses one.
 *
 * The device owns whatever its backend holds (a file descriptor for the Linux
 * backend). Drivers borrow a device and never close it.
 *
 * A device starts closed: zero-initialize it (i2c_device dev = {0};) before
 * its first open. Every backend's open refuses a device that is still open,
 * returning INVALID_ARGUMENT and leaving it untouched, so a missing close
 * cannot overwrite and orphan the resource it holds. Close it first.
 *
 * Addresses are 7-bit: every backend's open rejects an address above
 * I2C_ADDRESS_7BIT_MAX (0x7f) with INVALID_ARGUMENT, before changing the
 * device or acquiring any resource (the Linux backend checks it before
 * open() or ioctl(I2C_SLAVE)). Only the range is checked; reserved addresses
 * inside it are not refused here.
 *
 * Backend contract:
 * - The ops table provides all four callbacks. A wrapper whose callback is
 *   NULL returns INVALID_ARGUMENT without calling it and leaves the device
 *   unchanged (a close cannot release what it cannot call).
 * - OS_ERROR always carries the nonzero errno of the failed call. A
 *   condition with no errno (such as read() returning 0) is reported as
 *   OK with the actual byte count, which the wrapper turns into
 *   SHORT_TRANSFER.
 * - wait_us returns only after the full duration has elapsed, retrying
 *   toward the same deadline when interrupted (EINTR); it never reports OK
 *   for a partial wait. A duration whose deadline the backend's clock
 *   cannot represent is INVALID_ARGUMENT. Wait results carry no byte
 *   counts: the wrapper sets requested_len and transferred_len to 0.
 * - Interruption policies differ by operation. A read or write interrupted
 *   by a signal (EINTR) is reported as OS_ERROR/EINTR and not retried,
 *   because the backend cannot tell whether the transaction reached the
 *   bus; the caller decides. A wait is retried toward the deadline fixed
 *   before it first slept. A close is never retried.
 */

#define I2C_ADDRESS_7BIT_MAX 0x7f

/*
 * Stored codes: the storage record proposed in Michael's storage design
 * (PR #5, docs/design.md) writes these values to disk in byte 36, so
 * existing values never change. Add new statuses only at the end, with the
 * next unused number.
 */
typedef enum {
    I2C_STATUS_OK = 0,
    I2C_STATUS_INVALID_ARGUMENT = 1,
    I2C_STATUS_OS_ERROR = 2,        /* a system call failed; see os_errno */
    I2C_STATUS_SHORT_TRANSFER = 3,  /* bytes moved differ from bytes
                                       requested, fewer or (backend bug)
                                       more */
    I2C_STATUS_SCRIPT_MISMATCH = 4  /* mock only: operation not in the
                                       script */
} i2c_status;

typedef struct {
    i2c_status status;
    int os_errno;           /* errno captured right after the failed call */
    size_t requested_len;
    size_t transferred_len;
} i2c_result;

typedef struct {
    i2c_result (*write)(void *ctx, const uint8_t *buf, size_t len);
    i2c_result (*read)(void *ctx, uint8_t *buf, size_t len);
    i2c_result (*wait_us)(void *ctx, uint32_t duration_us);
    i2c_result (*close)(void *ctx);
} i2c_device_ops;

typedef struct {
    const i2c_device_ops *ops;  /* NULL once closed */
    void *ctx;
    uint8_t address_7bit;
    int is_simulated;
} i2c_device;

i2c_result i2c_device_write(i2c_device *dev, const uint8_t *buf, size_t len);
i2c_result i2c_device_read(i2c_device *dev, uint8_t *buf, size_t len);
i2c_result i2c_device_wait_us(i2c_device *dev, uint32_t duration_us);
i2c_result i2c_device_close(i2c_device *dev);
const char *i2c_status_name(i2c_status status);

#endif
