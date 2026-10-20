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
    assert(i2c_mock_open(&mock, &dev, 0x36, NULL, 1) ==
           I2C_STATUS_INVALID_ARGUMENT);

    r = i2c_device_write(NULL, cmd_bytes, sizeof cmd_bytes);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);

    assert(i2c_mock_open(&mock, &dev, 0x36, NULL, 0) ==
           I2C_STATUS_OK);
    r = i2c_device_read(&dev, NULL, 2);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    r = i2c_device_read(&dev, buf, 0);
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
 * as a backend that misreports its byte count. It never touches buffers.
 */
typedef struct {
    i2c_result result;
} canned_backend;

/* canned_write: return the preset result. */
static i2c_result canned_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return ((canned_backend *)ctx)->result;
}

/* canned_read: return the preset result. */
static i2c_result canned_read(void *ctx, uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return ((canned_backend *)ctx)->result;
}

/* canned_wait_us: return the preset result. */
static i2c_result canned_wait_us(void *ctx, uint32_t duration_us)
{
    (void)duration_us;
    return ((canned_backend *)ctx)->result;
}

/* canned_close: return the preset result. */
static i2c_result canned_close(void *ctx)
{
    return ((canned_backend *)ctx)->result;
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
    test_read_length_mismatch();
    test_script_over_transfer_rejected();
    test_long_wait_recorded();
    test_backend_over_transfer();
    test_wait_has_no_byte_counts();
    test_failed_backend_close_still_closes();
    test_status_names();

    puts("SIMULATED i2c transport tests passed");
    return 0;
}
