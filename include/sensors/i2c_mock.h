#ifndef SENSORS_I2C_MOCK_H
#define SENSORS_I2C_MOCK_H

#include "sensors/i2c_transport.h"

/*
 * Scripted, hardware-free I2C device for tests. Every device it opens is
 * marked simulated. Waits are recorded and checked, never slept, so protocol
 * timing is testable without slowing the suite. Uses no heap: the mock lives
 * in caller storage and borrows the step array, which must outlive it.
 *
 * A step is validated before its fail_errno applies: a WRITE step needs
 * expected bytes to compare, so a WRITE step with NULL bytes is a
 * SCRIPT_MISMATCH even when fail_errno is set. A READ step may have NULL
 * bytes only when transfer_len is 0.
 */

typedef enum {
    I2C_MOCK_WRITE,
    I2C_MOCK_READ,
    I2C_MOCK_WAIT
} i2c_mock_op;

typedef struct {
    i2c_mock_op op;
    const uint8_t *bytes;   /* WRITE: expected bytes; READ: bytes returned */
    size_t len;             /* WRITE/READ: expected request length */
    size_t transfer_len;    /* WRITE/READ: bytes the fake bus moves */
    int fail_errno;         /* nonzero: fail with OS_ERROR and this errno */
    uint32_t min_wait_us;   /* WAIT: shortest acceptable wait */
} i2c_mock_step;

typedef struct {
    const i2c_mock_step *steps;
    size_t step_count;
    size_t next_step;
    uint64_t waited_us;     /* total simulated wait time */
    int mismatch;           /* sticky once any operation is unexpected */
    size_t mismatch_step;   /* index of the first unexpected operation */
} i2c_mock;

/* dev must be closed (zeroed or closed by i2c_device_close) and
 * address_7bit must be at most 0x7f; see i2c_transport.h. Returns
 * INVALID_ARGUMENT and leaves dev and mock untouched otherwise. */
i2c_status i2c_mock_open(i2c_mock *mock, i2c_device *dev,
                         uint8_t address_7bit,
                         const i2c_mock_step *steps, size_t step_count);

#endif
