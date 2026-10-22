#ifndef SENSORS_I2C_LINUX_H
#define SENSORS_I2C_LINUX_H

#include "sensors/i2c_transport.h"

/*
 * Linux /dev/i2c-N backend. Each open device owns one heap context holding
 * one descriptor, opened O_RDWR | O_CLOEXEC and bound to its address with
 * ioctl(I2C_SLAVE), never I2C_SLAVE_FORCE: an address claimed by a kernel
 * driver fails with OS_ERROR/EBUSY. i2c_device_close closes the descriptor
 * once and frees the context. Devices are marked not simulated.
 *
 * The descriptor is close-on-exec but not close-on-fork: do not fork()
 * without exec() while a device is open.
 *
 * Interruption policies (see the backend contract in i2c_transport.h):
 * - read/write: EINTR is reported as OS_ERROR/EINTR and not retried. The
 *   backend cannot tell whether the transaction reached the bus, so the
 *   caller decides whether to retry.
 * - wait_us: EINTR is retried toward the same absolute CLOCK_MONOTONIC
 *   deadline, so a wait is never cut short and retries never add time.
 * - close: never retried. Linux releases the descriptor even when close()
 *   fails, so a retry could close an unrelated, reused descriptor.
 */

/*
 * dev must be closed. On any failure dev is untouched and nothing is held:
 * no descriptor stays open and no memory stays allocated.
 *
 * cleanup_errno is required. It is set to 0 on every return, unless
 * binding the address failed and the close() that released the new
 * descriptor also failed: then the result still carries the bind errno
 * (the cause) and *cleanup_errno holds close()'s errno, so the caller can
 * log both. That close() is not retried.
 */
i2c_result i2c_linux_open(i2c_device *dev, unsigned bus_number,
                          uint8_t address_7bit, int *cleanup_errno);

#endif
