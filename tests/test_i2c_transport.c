#include "sensors/i2c_mock.h"
#include "sensors/i2c_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static const uint8_t cmd_bytes[] = {0x0F, 0x10};
static const uint8_t other_cmd_bytes[] = {0x0F, 0x11};
static const uint8_t data_bytes[] = {0x02, 0x64};

/* Arbitrary non-default mock state, to show a failed open never writes it. */
static const i2c_mock mock_sentinel = {NULL, 7, 3, 11, 1, 2};

/* mock_is_sentinel: true if every field still holds mock_sentinel's value. */
static int mock_is_sentinel(const i2c_mock *mock)
{
    return mock->steps == mock_sentinel.steps &&
           mock->step_count == mock_sentinel.step_count &&
           mock->next_step == mock_sentinel.next_step &&
           mock->waited_us == mock_sentinel.waited_us &&
           mock->mismatch == mock_sentinel.mismatch &&
           mock->mismatch_step == mock_sentinel.mismatch_step;
}

/* test_full_script_succeeds: write, wait, read in scripted order. */
static void test_full_script_succeeds(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
        {.op = I2C_MOCK_WAIT, .min_wait_us = 3000},
        {.op = I2C_MOCK_READ, .bytes = data_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_status open_status;
    i2c_result r;

    open_status = i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps));
    assert(open_status == I2C_STATUS_OK);
    assert(dev.is_simulated == 1);
    assert(dev.address_7bit == 0x36);

    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_OK);
    assert(r.requested_len == 2 && r.transferred_len == 2);

    r = i2c_device_wait_us(&dev, 3000);
    assert(r.status == I2C_STATUS_OK);
    assert(r.requested_len == 0 && r.transferred_len == 0);

    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_OK);
    assert(r.transferred_len == 2);
    assert(memcmp(buf, data_bytes, sizeof buf) == 0);

    assert(mock.waited_us == 3000);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_write_bytes_mismatch: wrong command bytes are rejected. */
static void test_write_bytes_mismatch(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_write(&dev, other_cmd_bytes, sizeof other_cmd_bytes);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(mock.mismatch == 1 && mock.mismatch_step == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_read_os_error_preserves_errno: errno is reported, buffer untouched. */
static void test_read_os_error_preserves_errno(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_READ, .bytes = data_bytes, .len = 2,
         .transfer_len = 2, .fail_errno = ENXIO},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_OS_ERROR);
    assert(r.os_errno == ENXIO);
    assert(r.transferred_len == 0);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_write_os_error_preserves_errno: a failed write reports errno. */
static void test_write_os_error_preserves_errno(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2, .fail_errno = EIO},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_OS_ERROR);
    assert(r.os_errno == EIO);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_short_read: one of two bytes is a SHORT_TRANSFER, not success. */
static void test_short_read(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_READ, .bytes = data_bytes, .len = 2,
         .transfer_len = 1},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.os_errno == 0);
    assert(r.requested_len == 2 && r.transferred_len == 1);
    assert(buf[0] == 0x02 && buf[1] == 0xAA);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_short_write: a partial write is a SHORT_TRANSFER, not success. */
static void test_short_write(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 1},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.requested_len == 2 && r.transferred_len == 1);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_wait_too_short: a wait under the scripted minimum is rejected. */
static void test_wait_too_short(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WAIT, .min_wait_us = 180000},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_wait_us(&dev, 120000);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(mock.waited_us == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_wait_os_error: a failed wait reports errno. */
static void test_wait_os_error(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WAIT, .min_wait_us = 1000, .fail_errno = EINVAL},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_OS_ERROR);
    assert(r.os_errno == EINVAL);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_out_of_order_is_sticky: after one mismatch, everything fails. */
static void test_out_of_order_is_sticky(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
        {.op = I2C_MOCK_READ, .bytes = data_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2];
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(mock.mismatch_step == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/*
 * test_unused_steps_fail_close: close reports an unfinished script, and the
 * device is closed anyway so a second close cannot release it twice.
 */
static void test_unused_steps_fail_close(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(dev.ops == NULL && dev.ctx == NULL);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
}

/* test_operation_after_script_end: extra operations are rejected. */
static void test_operation_after_script_end(void)
{
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, NULL, 0) ==
           I2C_STATUS_OK);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_invalid_arguments: bad calls fail without reaching the backend. */
static void test_invalid_arguments(void)
{
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2];
    i2c_result r;

    assert(i2c_mock_open(NULL, &dev, 0x36, NULL, 0) ==
           I2C_STATUS_INVALID_ARGUMENT);
    mock = mock_sentinel;
    assert(i2c_mock_open(&mock, NULL, 0x36, NULL, 0) ==
           I2C_STATUS_INVALID_ARGUMENT);
    assert(mock_is_sentinel(&mock));
    assert(i2c_mock_open(&mock, &dev, 0x36, NULL, 1) ==
           I2C_STATUS_INVALID_ARGUMENT);
    assert(mock_is_sentinel(&mock));
    assert(dev.ops == NULL && dev.ctx == NULL && dev.is_simulated == 0);

    r = i2c_device_write(NULL, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(NULL, buf, sizeof buf);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_wait_us(NULL, 1000);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_close(NULL);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);

    assert(i2c_mock_open(&mock, &dev, 0x36, NULL, 0) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, NULL, 2);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(&dev, buf, 0);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_write(&dev, NULL, 2);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_write(&dev, cmd_bytes, 0);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(mock.mismatch == 0);

    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
}

/*
 * test_open_refuses_open_device: reopening an open device is rejected and
 * leaves the first binding intact, so a forgotten close cannot silently
 * orphan a backend resource (a file descriptor on Linux).
 */
static void test_open_refuses_open_device(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock first;
    i2c_mock second;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&first, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    assert(i2c_mock_open(&second, &dev, 0x23, NULL, 0) ==
           I2C_STATUS_INVALID_ARGUMENT);
    assert(dev.ctx == &first && dev.address_7bit == 0x36);
    assert(first.next_step == 0 && first.mismatch == 0);

    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_OK);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);

    assert(i2c_mock_open(&second, &dev, 0x23, NULL, 0) == I2C_STATUS_OK);
    assert(dev.ctx == &second && dev.address_7bit == 0x23);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/*
 * test_open_rejects_non_7bit_address: 0x80 and 0xff are refused without
 * touching the device or the mock; 0x7f, the top 7-bit address, opens and
 * runs its script from the first step.
 */
static void test_open_rejects_non_7bit_address(void)
{
    static const uint8_t bad_addresses[] = {0x80, 0xff};
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WRITE, .bytes = cmd_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    size_t i;
    i2c_result r;

    for (i = 0; i < COUNT_OF(bad_addresses); i++) {
        mock = mock_sentinel;
        assert(i2c_mock_open(&mock, &dev, bad_addresses[i], steps,
                             COUNT_OF(steps)) ==
               I2C_STATUS_INVALID_ARGUMENT);
        assert(dev.ops == NULL && dev.ctx == NULL);
        assert(dev.address_7bit == 0 && dev.is_simulated == 0);
        assert(mock_is_sentinel(&mock));
    }

    assert(i2c_mock_open(&mock, &dev, 0x7f, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    assert(dev.address_7bit == 0x7f);
    assert(mock.next_step == 0 && mock.mismatch == 0);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_OK);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_read_length_mismatch: a read of the wrong length is rejected. */
static void test_read_length_mismatch(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_READ, .bytes = data_bytes, .len = 2,
         .transfer_len = 2},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[1] = {0xAA};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(buf[0] == 0xAA);
    assert(mock.mismatch == 1 && mock.mismatch_step == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_script_over_transfer_rejected: a script moving too much is invalid. */
static void test_script_over_transfer_rejected(void)
{
    static const uint8_t three_bytes[] = {0x01, 0x02, 0x03};
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_READ, .bytes = three_bytes, .len = 2,
         .transfer_len = 3},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_empty_read_copies_nothing: a 0-byte read never touches the buffer. */
static void test_empty_read_copies_nothing(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_READ, .bytes = NULL, .len = 2, .transfer_len = 0},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x23, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.requested_len == 2 && r.transferred_len == 0);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/*
 * expect_step_rejected: run one operation against a one-step script and
 * check it is a SCRIPT_MISMATCH that moves no bytes, leaves the buffer
 * untouched, and fails close.
 */
static void expect_step_rejected(const i2c_mock_step *step, i2c_mock_op op,
                                 size_t len)
{
    i2c_mock mock;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    assert(len <= sizeof buf && len <= sizeof cmd_bytes);
    assert(i2c_mock_open(&mock, &dev, 0x36, step, 1) == I2C_STATUS_OK);
    if (op == I2C_MOCK_WRITE) {
        r = i2c_device_write(&dev, cmd_bytes, len);
    } else {
        r = i2c_device_read(&dev, buf, len);
    }
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
    assert(r.os_errno == 0 && r.transferred_len == 0);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
    assert(mock.mismatch == 1 && mock.mismatch_step == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_SCRIPT_MISMATCH);
}

/* test_inconsistent_steps_rejected: malformed steps never run. */
static void test_inconsistent_steps_rejected(void)
{
    const i2c_mock_step bad_op = {.op = (i2c_mock_op)99, .bytes = cmd_bytes,
                                  .len = 2, .transfer_len = 2};
    const i2c_mock_step write_null_bytes = {.op = I2C_MOCK_WRITE,
                                            .len = 2, .transfer_len = 2,
                                            .fail_errno = EIO};
    const i2c_mock_step write_over = {.op = I2C_MOCK_WRITE,
                                      .bytes = cmd_bytes, .len = 2,
                                      .transfer_len = 3};
    const i2c_mock_step write_len = {.op = I2C_MOCK_WRITE,
                                     .bytes = cmd_bytes, .len = 2,
                                     .transfer_len = 1};
    const i2c_mock_step read_null_bytes = {.op = I2C_MOCK_READ, .len = 2,
                                           .transfer_len = 2};

    expect_step_rejected(&bad_op, I2C_MOCK_WRITE, 2);
    expect_step_rejected(&bad_op, I2C_MOCK_READ, 2);
    expect_step_rejected(&write_null_bytes, I2C_MOCK_WRITE, 2);
    expect_step_rejected(&write_over, I2C_MOCK_WRITE, 2);
    expect_step_rejected(&write_len, I2C_MOCK_WRITE, 1);
    expect_step_rejected(&read_null_bytes, I2C_MOCK_READ, 2);
}

/* test_long_wait_recorded: a wait above the minimum counts in full. */
static void test_long_wait_recorded(void)
{
    const i2c_mock_step steps[] = {
        {.op = I2C_MOCK_WAIT, .min_wait_us = 3000},
    };
    i2c_mock mock;
    i2c_device dev = {0};
    i2c_result r;

    assert(i2c_mock_open(&mock, &dev, 0x36, steps, COUNT_OF(steps)) ==
           I2C_STATUS_OK);
    r = i2c_device_wait_us(&dev, 5000);
    assert(r.status == I2C_STATUS_OK);
    assert(mock.waited_us == 5000);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/*
 * Canned backend: every operation returns the same preset result. It lets
 * tests reach wrapper paths a well-formed mock script never produces, such
 * as a backend that misreports its byte count. It never touches buffers,
 * and it counts calls so tests can prove the wrapper skipped it.
 */
typedef struct {
    i2c_result result;
    unsigned calls;
} canned_backend;

/* canned_called: count one backend call and return the preset result. */
static i2c_result canned_called(void *ctx)
{
    canned_backend *backend = ctx;

    backend->calls++;
    return backend->result;
}

/* canned_write: count the call and return the preset result. */
static i2c_result canned_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return canned_called(ctx);
}

/* canned_read: count the call and return the preset result. */
static i2c_result canned_read(void *ctx, uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return canned_called(ctx);
}

/* canned_wait_us: count the call and return the preset result. */
static i2c_result canned_wait_us(void *ctx, uint32_t duration_us)
{
    (void)duration_us;
    return canned_called(ctx);
}

/* canned_close: count the call and return the preset result. */
static i2c_result canned_close(void *ctx)
{
    return canned_called(ctx);
}

static const i2c_device_ops canned_ops = {
    canned_write,
    canned_read,
    canned_wait_us,
    canned_close
};

/* canned_open: bind a simulated device to a canned backend. */
static void canned_open(canned_backend *backend, i2c_device *dev,
                        i2c_status status, int os_errno,
                        size_t transferred_len)
{
    i2c_result preset = {status, os_errno, 99, transferred_len};

    backend->result = preset;
    backend->calls = 0;
    dev->ops = &canned_ops;
    dev->ctx = backend;
    dev->address_7bit = 0x23;
    dev->is_simulated = 1;
}

/* test_backend_over_transfer: OK with too many bytes is not success. */
static void test_backend_over_transfer(void)
{
    canned_backend backend;
    i2c_device dev = {0};
    uint8_t buf[2];
    i2c_result r;

    canned_open(&backend, &dev, I2C_STATUS_OK, 0, 3);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.requested_len == 2 && r.transferred_len == 3);
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.requested_len == 2 && r.transferred_len == 3);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_wait_has_no_byte_counts: the wrapper clears backend byte counts. */
static void test_wait_has_no_byte_counts(void)
{
    canned_backend backend;
    i2c_device dev = {0};
    i2c_result r;

    canned_open(&backend, &dev, I2C_STATUS_OK, 0, 77);
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_OK);
    assert(r.requested_len == 0 && r.transferred_len == 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
}

/* test_failed_backend_close_still_closes: close(2) semantics. */
static void test_failed_backend_close_still_closes(void)
{
    canned_backend backend;
    i2c_device dev = {0};
    i2c_result r;

    canned_open(&backend, &dev, I2C_STATUS_OS_ERROR, EIO, 0);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OS_ERROR);
    assert(r.os_errno == EIO);
    assert(dev.ops == NULL && dev.ctx == NULL);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
}

/*
 * test_closed_device_skips_backend: after close, every operation is refused
 * by the wrapper and the backend is never called again.
 */
static void test_closed_device_skips_backend(void)
{
    canned_backend backend;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    canned_open(&backend, &dev, I2C_STATUS_OK, 0, 2);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
    assert(backend.calls == 1);

    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(backend.calls == 1);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
}

/*
 * test_missing_callbacks_rejected: an ops table with NULL callbacks is
 * refused instead of called, and the device is left as it was, so a
 * resource the backend holds is not dropped by a close it cannot perform.
 */
static void test_missing_callbacks_rejected(void)
{
    static const i2c_device_ops no_ops = {NULL, NULL, NULL, NULL};
    canned_backend backend;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    dev.ops = &no_ops;
    dev.ctx = &backend;
    r = i2c_device_write(&dev, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(dev.ops == &no_ops && dev.ctx == &backend);
}

/* test_status_names: every status has stable log text. */
static void test_status_names(void)
{
    assert(strcmp(i2c_status_name(I2C_STATUS_OK), "OK") == 0);
    assert(strcmp(i2c_status_name(I2C_STATUS_INVALID_ARGUMENT),
                  "INVALID_ARGUMENT") == 0);
    assert(strcmp(i2c_status_name(I2C_STATUS_OS_ERROR), "OS_ERROR") == 0);
    assert(strcmp(i2c_status_name(I2C_STATUS_SHORT_TRANSFER),
                  "SHORT_TRANSFER") == 0);
    assert(strcmp(i2c_status_name(I2C_STATUS_SCRIPT_MISMATCH),
                  "SCRIPT_MISMATCH") == 0);
}

/*
 * test_stored_status_codes: these numbers are stored on disk (byte 36 of
 * the storage record proposed in PR #5), so they must never change. A
 * renumbered or reordered enum fails here.
 */
static void test_stored_status_codes(void)
{
    assert(I2C_STATUS_OK == 0);
    assert(I2C_STATUS_INVALID_ARGUMENT == 1);
    assert(I2C_STATUS_OS_ERROR == 2);
    assert(I2C_STATUS_SHORT_TRANSFER == 3);
    assert(I2C_STATUS_SCRIPT_MISMATCH == 4);
}

int main(void)
{
    test_full_script_succeeds();
    test_write_bytes_mismatch();
    test_read_os_error_preserves_errno();
    test_write_os_error_preserves_errno();
    test_short_read();
    test_short_write();
    test_wait_too_short();
    test_wait_os_error();
    test_out_of_order_is_sticky();
    test_unused_steps_fail_close();
    test_operation_after_script_end();
    test_invalid_arguments();
    test_open_refuses_open_device();
    test_open_rejects_non_7bit_address();
    test_read_length_mismatch();
    test_script_over_transfer_rejected();
    test_empty_read_copies_nothing();
    test_inconsistent_steps_rejected();
    test_long_wait_recorded();
    test_backend_over_transfer();
    test_wait_has_no_byte_counts();
    test_failed_backend_close_still_closes();
    test_closed_device_skips_backend();
    test_missing_callbacks_rejected();
    test_status_names();
    test_stored_status_codes();

    puts("SIMULATED i2c transport tests passed");
    return 0;
}
