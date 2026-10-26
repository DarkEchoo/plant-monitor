#define _POSIX_C_SOURCE 200809L

#include "sensors/i2c_linux.h"
#include "sensors/i2c_transport.h"
#include "../src/sensors/i2c_linux_sys.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Two kinds of test, neither of which touches a real I2C bus:
 * - SIMULATED: a fake syscall table scripts every result and records every
 *   call, so failure paths are exercised deterministically.
 * - OS PLUMBING: real syscalls against /dev/null or a missing path. These
 *   check descriptor handling only; /dev/null is not an I2C bus, so no I2C
 *   transaction is verified. No test opens /dev/i2c-N: a development
 *   machine's buses may belong to displays or other hardware.
 *
 * The public i2c_linux_open always builds "/dev/i2c-<bus>", so it cannot be
 * pointed at /dev/null or a temporary path. It is tested only with bus
 * UINT_MAX, whose node i2c-dev cannot create (its minors stop at 2^20), and
 * only after stat() confirms that node is absent.
 *
 * Every fake that fails sets errno (or, for clock_sleep, returns the error
 * number), as the real call does; the backend copies it unchecked.
 */

#define FAKE_MAX_CALLS 8
#define FAKE_FIRST_FD 100

/* Fake syscall state: scripted results, then a record of each call. */
typedef struct {
    int alloc_fail;
    int open_errno;          /* nonzero: open fails with this errno */
    int next_fd;
    int ioctl_errno;         /* nonzero: ioctl_ul fails with this errno */
    int close_errno;         /* nonzero: the next close fails, once */
    ssize_t io_result;       /* read/write return value */
    int io_errno;            /* errno set when io_result < 0 */
    int io_fail_once;        /* after one failure, succeed in full */
    int now_errno;           /* nonzero: clock_now fails */
    struct timespec now;
    int sleep_results[FAKE_MAX_CALLS];  /* returned in order, then 0 */

    unsigned alloc_calls;
    unsigned open_calls;
    unsigned ioctl_calls;
    unsigned read_calls;
    unsigned write_calls;
    unsigned close_calls;
    unsigned now_calls;
    unsigned sleep_calls;
    char open_path[64];
    int ioctl_fd;
    unsigned long ioctl_request;
    unsigned long ioctl_arg;
    clockid_t now_clock;
    clockid_t sleep_clock;
    int sleep_flags;
    int cleanup_errno;       /* open_fake's cleanup_errno output */
    int io_fd;
    size_t io_len;
    int closed_fds[FAKE_MAX_CALLS];
    struct timespec deadlines[FAKE_MAX_CALLS];
} fake_state;

static fake_state fake;

/* fake_reset: clear all scripted results and call records. */
static void fake_reset(void)
{
    memset(&fake, 0, sizeof fake);
    fake.next_fd = FAKE_FIRST_FD;
}

/* fake_total_calls: every syscall made through the fake table. */
static unsigned fake_total_calls(void)
{
    return fake.alloc_calls + fake.open_calls + fake.ioctl_calls +
           fake.read_calls + fake.write_calls + fake.close_calls +
           fake.now_calls + fake.sleep_calls;
}

/* fake_alloc_zeroed: calloc, or NULL when scripted to fail. */
static void *fake_alloc_zeroed(size_t size)
{
    fake.alloc_calls++;
    return fake.alloc_fail ? NULL : calloc(1, size);
}

/* fake_open: record the path and hand out the next fake descriptor. */
static int fake_open(const char *path)
{
    fake.open_calls++;
    snprintf(fake.open_path, sizeof fake.open_path, "%s", path);
    if (fake.open_errno != 0) {
        errno = fake.open_errno;
        return -1;
    }
    return fake.next_fd++;
}

/* fake_ioctl_ul: record the descriptor, request, and argument. */
static int fake_ioctl_ul(int fd, unsigned long request, unsigned long arg)
{
    fake.ioctl_calls++;
    fake.ioctl_fd = fd;
    fake.ioctl_request = request;
    fake.ioctl_arg = arg;
    if (fake.ioctl_errno != 0) {
        errno = fake.ioctl_errno;
        return -1;
    }
    return 0;
}

/* fake_read: return io_result bytes of 0x5A, or fail with io_errno. */
static ssize_t fake_read(int fd, void *buf, size_t len)
{
    fake.read_calls++;
    fake.io_fd = fd;
    fake.io_len = len;
    if (fake.io_result < 0) {
        if (fake.io_fail_once) {
            fake.io_result = (ssize_t)len;
        }
        errno = fake.io_errno;
        return -1;
    }
    assert((size_t)fake.io_result <= len);
    memset(buf, 0x5A, (size_t)fake.io_result);
    return fake.io_result;
}

/* fake_write: accept io_result bytes, or fail with io_errno. */
static ssize_t fake_write(int fd, const void *buf, size_t len)
{
    (void)buf;
    fake.write_calls++;
    fake.io_fd = fd;
    fake.io_len = len;
    if (fake.io_result < 0) {
        if (fake.io_fail_once) {
            fake.io_result = (ssize_t)len;
        }
        errno = fake.io_errno;
        return -1;
    }
    return fake.io_result;
}

/*
 * fake_close: record the descriptor; fail once with close_errno if set. A
 * backend that wrongly retried close would then see success, not loop.
 */
static int fake_close(int fd)
{
    int fail_errno = fake.close_errno;

    if (fake.close_calls < FAKE_MAX_CALLS) {
        fake.closed_fds[fake.close_calls] = fd;
    }
    fake.close_calls++;
    fake.close_errno = 0;
    if (fail_errno != 0) {
        errno = fail_errno;
        return -1;
    }
    return 0;
}

/* fake_clock_now: record the clock and report the scripted time. */
static int fake_clock_now(clockid_t clock, struct timespec *now)
{
    fake.now_calls++;
    fake.now_clock = clock;
    if (fake.now_errno != 0) {
        errno = fake.now_errno;
        return -1;
    }
    *now = fake.now;
    return 0;
}

/*
 * fake_clock_sleep: record the clock, flags and deadline; return the next
 * scripted result.
 */
static int fake_clock_sleep(clockid_t clock, int flags,
                            const struct timespec *deadline)
{
    unsigned i = fake.sleep_calls++;

    fake.sleep_clock = clock;
    fake.sleep_flags = flags;
    if (i >= FAKE_MAX_CALLS) {
        return 0;
    }
    fake.deadlines[i] = *deadline;
    return fake.sleep_results[i];
}

/* fake_sys: the seam table that routes every syscall to the fakes above. */
static const i2c_linux_sys fake_sys = {
    fake_alloc_zeroed,
    fake_open,
    fake_ioctl_ul,
    fake_read,
    fake_write,
    fake_close,
    fake_clock_now,
    fake_clock_sleep
};

/* open_fake: open dev on the fake bus; cleanup errno lands in fake. */
static i2c_result open_fake(i2c_device *dev, uint8_t address_7bit)
{
    return i2c_linux_open_path(dev, "/dev/i2c-1", address_7bit, &fake_sys,
                               &fake.cleanup_errno);
}

/* device_is_zeroed: true if dev still looks never opened. */
static int device_is_zeroed(const i2c_device *dev)
{
    return dev->ops == NULL && dev->ctx == NULL && dev->address_7bit == 0 &&
           dev->is_simulated == 0;
}

/* test_bus_path: bus numbers format as /dev/i2c-N and never truncate. */
static void test_bus_path(void)
{
    char path[32];
    char tiny[9];
    char *end;

    assert(i2c_linux_bus_path(path, sizeof path, 1) == 0);
    assert(strcmp(path, "/dev/i2c-1") == 0);
    assert(i2c_linux_bus_path(path, sizeof path, 0) == 0);
    assert(strcmp(path, "/dev/i2c-0") == 0);
    assert(i2c_linux_bus_path(path, sizeof path, UINT_MAX) == 0);
    assert(strncmp(path, "/dev/i2c-", 9) == 0);
    assert(strtoul(path + 9, &end, 10) == UINT_MAX && *end == '\0');
    assert(i2c_linux_bus_path(tiny, sizeof tiny, 1) == -1);
    assert(i2c_linux_bus_path(NULL, sizeof path, 1) == -1);
}

/* test_open_binds_address: open, bind, and close through the fake table. */
static void test_open_binds_address(void)
{
    /* Closed (ops NULL) but reused from a simulated device: open must
     * overwrite the stale label and address, not inherit them. */
    i2c_device dev = {NULL, NULL, 0x11, 1};
    i2c_result r;

    fake_reset();
    fake.cleanup_errno = -1;
    r = open_fake(&dev, 0x23);
    assert(r.status == I2C_STATUS_OK && r.os_errno == 0);
    assert(strcmp(fake.open_path, "/dev/i2c-1") == 0);
    assert(fake.alloc_calls == 1 && fake.open_calls == 1);
    assert(fake.ioctl_calls == 1 && fake.ioctl_fd == FAKE_FIRST_FD);
    assert(fake.ioctl_request == I2C_SLAVE);
    assert(fake.ioctl_request != I2C_SLAVE_FORCE);
    assert(fake.ioctl_arg == 0x23);
    assert(fake.cleanup_errno == 0);
    assert(dev.ops != NULL && dev.ctx != NULL);
    assert(dev.address_7bit == 0x23 && dev.is_simulated == 0);
    assert(fake.close_calls == 0);

    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);
    assert(dev.ops == NULL && dev.ctx == NULL);
}

/*
 * test_open_rejects_before_syscalls: bad arguments, addresses above 0x7f,
 * and an already-open device fail with no syscall and no change to dev.
 */
static void test_open_rejects_before_syscalls(void)
{
    static const uint8_t bad_addresses[] = {0x80, 0xff};
    i2c_device dev = {0};
    i2c_device open_dev = {0};
    i2c_device before;
    i2c_result r;
    size_t i;

    fake_reset();
    for (i = 0; i < sizeof bad_addresses; i++) {
        fake.cleanup_errno = -1;
        r = open_fake(&dev, bad_addresses[i]);
        assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
        assert(fake.cleanup_errno == 0);
    }
    fake.cleanup_errno = -1;
    r = i2c_linux_open_path(NULL, "/dev/i2c-1", 0x23, &fake_sys,
                            &fake.cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake.cleanup_errno == 0);
    fake.cleanup_errno = -1;
    r = i2c_linux_open_path(&dev, NULL, 0x23, &fake_sys, &fake.cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake.cleanup_errno == 0);
    fake.cleanup_errno = -1;
    r = i2c_linux_open_path(&dev, "/dev/i2c-1", 0x23, NULL,
                            &fake.cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake.cleanup_errno == 0);
    r = i2c_linux_open_path(&dev, "/dev/i2c-1", 0x23, &fake_sys, NULL);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake_total_calls() == 0);
    assert(device_is_zeroed(&dev));

    assert(open_fake(&open_dev, 0x36).status == I2C_STATUS_OK);
    before = open_dev;
    fake_reset();
    fake.cleanup_errno = -1;
    r = open_fake(&open_dev, 0x23);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake.cleanup_errno == 0);
    assert(fake_total_calls() == 0);
    assert(open_dev.ops == before.ops && open_dev.ctx == before.ctx &&
           open_dev.address_7bit == 0x36);
    assert(i2c_device_close(&open_dev).status == I2C_STATUS_OK);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);

    fake_reset();
    r = open_fake(&dev, 0x7f);
    assert(r.status == I2C_STATUS_OK);
    assert(fake.ioctl_arg == 0x7f && dev.address_7bit == 0x7f);
    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/* test_open_alloc_failure: no context means no descriptor is opened. */
static void test_open_alloc_failure(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    fake.alloc_fail = 1;
    fake.cleanup_errno = -1;
    r = open_fake(&dev, 0x23);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == ENOMEM);
    assert(fake.cleanup_errno == 0);
    assert(fake.alloc_calls == 1 && fake.open_calls == 0);
    assert(device_is_zeroed(&dev));
}

/* test_open_failure_frees_context: a failed open() holds nothing. */
static void test_open_failure_frees_context(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    fake.open_errno = EACCES;
    fake.cleanup_errno = -1;
    r = open_fake(&dev, 0x23);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EACCES);
    assert(fake.ioctl_calls == 0 && fake.close_calls == 0);
    assert(fake.cleanup_errno == 0);
    assert(device_is_zeroed(&dev));
}

/*
 * test_bind_failure_closes_fd: a refused address closes the new descriptor
 * once. The bind errno stays in the result; a cleanup close() failure is
 * reported separately through cleanup_errno, so neither erases the other.
 */
static void test_bind_failure_closes_fd(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    fake.ioctl_errno = EBUSY;
    fake.close_errno = EIO;
    fake.cleanup_errno = -1;
    r = open_fake(&dev, 0x23);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EBUSY);
    assert(fake.cleanup_errno == EIO);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);
    assert(device_is_zeroed(&dev));

    fake_reset();
    fake.ioctl_errno = EBUSY;
    fake.cleanup_errno = -1;
    r = open_fake(&dev, 0x23);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EBUSY);
    assert(fake.cleanup_errno == 0);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);
    assert(device_is_zeroed(&dev));
}

/* test_devices_own_descriptors: two devices never share a descriptor. */
static void test_devices_own_descriptors(void)
{
    static const uint8_t cmd[] = {0x10};
    i2c_device light = {0};
    i2c_device soil = {0};
    i2c_result r;

    fake_reset();
    fake.io_result = 1;
    assert(open_fake(&light, 0x23).status == I2C_STATUS_OK);
    assert(open_fake(&soil, 0x36).status == I2C_STATUS_OK);
    assert(light.ctx != soil.ctx);

    assert(i2c_device_close(&light).status == I2C_STATUS_OK);
    assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);
    r = i2c_device_write(&soil, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_OK);
    assert(fake.io_fd == FAKE_FIRST_FD + 1);
    assert(i2c_device_close(&soil).status == I2C_STATUS_OK);
    assert(fake.close_calls == 2 && fake.closed_fds[1] == FAKE_FIRST_FD + 1);
}

/*
 * test_read_write_results: byte counts pass through for the wrapper to
 * judge, errno is kept, and EINTR is reported after exactly one call.
 */
static void test_read_write_results(void)
{
    static const uint8_t cmd[] = {0x10, 0x20};
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;

    fake_reset();
    assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);

    fake.io_result = 2;
    r = i2c_device_write(&dev, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_OK && r.transferred_len == 2);
    assert(fake.io_fd == FAKE_FIRST_FD && fake.io_len == 2);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_OK && r.transferred_len == 2);
    assert(buf[0] == 0x5A && buf[1] == 0x5A);

    buf[0] = buf[1] = 0xAA;
    fake.io_result = 1;
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.requested_len == 2 && r.transferred_len == 1);
    assert(buf[0] == 0x5A && buf[1] == 0xAA);
    r = i2c_device_write(&dev, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER && r.transferred_len == 1);

    fake.io_result = 0;
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.os_errno == 0 && r.transferred_len == 0);

    fake.io_result = -1;
    fake.io_errno = EIO;
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EIO);
    assert(r.transferred_len == 0);
    r = i2c_device_write(&dev, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EIO);

    /* One-shot EINTR: a backend that retried would succeed and be caught. */
    fake.io_errno = EINTR;
    fake.io_fail_once = 1;
    fake.read_calls = fake.write_calls = 0;
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EINTR);
    assert(fake.read_calls == 1);
    fake.io_result = -1;
    r = i2c_device_write(&dev, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EINTR);
    assert(fake.write_calls == 1);

    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/* timespec_equal: true if both times are identical. */
static int timespec_equal(struct timespec a, long sec, long nsec)
{
    return a.tv_sec == sec && a.tv_nsec == nsec;
}

/* test_wait_deadline: the deadline is monotonic now plus the duration. */
static void test_wait_deadline(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);

    fake.now.tv_sec = 10;
    fake.now.tv_nsec = 999999999;
    r = i2c_device_wait_us(&dev, 1);
    assert(r.status == I2C_STATUS_OK);
    assert(fake.now_calls == 1 && fake.sleep_calls == 1);
    assert(fake.now_clock == CLOCK_MONOTONIC);
    assert(fake.sleep_clock == CLOCK_MONOTONIC);
    assert(fake.sleep_flags == TIMER_ABSTIME);
    assert(timespec_equal(fake.deadlines[0], 11, 999));

    fake.now.tv_sec = 5;
    fake.now.tv_nsec = 600000000;
    r = i2c_device_wait_us(&dev, 2500000);
    assert(r.status == I2C_STATUS_OK);
    assert(timespec_equal(fake.deadlines[1], 8, 100000000));

    r = i2c_device_wait_us(&dev, 0);
    assert(r.status == I2C_STATUS_OK);
    assert(timespec_equal(fake.deadlines[2], 5, 600000000));

    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/*
 * test_wait_retries_eintr_same_deadline: an interrupted wait resumes toward
 * the deadline computed before the first sleep; the clock is read once.
 */
static void test_wait_retries_eintr_same_deadline(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);
    fake.now.tv_sec = 3;
    fake.now.tv_nsec = 0;
    fake.sleep_results[0] = EINTR;
    fake.sleep_results[1] = EINTR;
    r = i2c_device_wait_us(&dev, 3000);
    assert(r.status == I2C_STATUS_OK && r.os_errno == 0);
    assert(fake.now_calls == 1 && fake.sleep_calls == 3);
    assert(timespec_equal(fake.deadlines[0], 3, 3000000));
    assert(timespec_equal(fake.deadlines[1], 3, 3000000));
    assert(timespec_equal(fake.deadlines[2], 3, 3000000));
    assert(fake.sleep_clock == CLOCK_MONOTONIC);
    assert(fake.sleep_flags == TIMER_ABSTIME);
    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/* test_wait_errors: clock and sleep failures are reported, not hidden. */
static void test_wait_errors(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);

    fake.now_errno = EINVAL;
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EINVAL);
    assert(fake.sleep_calls == 0);

    fake.now_errno = 0;
    fake.sleep_results[0] = EINTR;
    fake.sleep_results[1] = EINVAL;
    r = i2c_device_wait_us(&dev, 1000);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == EINVAL);
    assert(fake.sleep_calls == 2);

    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/* test_wait_deadline_overflow: a deadline past time_t's range is refused. */
static void test_wait_deadline_overflow(void)
{
    i2c_device dev = {0};
    i2c_result r;

    fake_reset();
    assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);

    fake.now.tv_sec = I2C_LINUX_TIME_T_MAX;
    fake.now.tv_nsec = 0;
    r = i2c_device_wait_us(&dev, 1000000);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    fake.now.tv_nsec = 999999999;
    r = i2c_device_wait_us(&dev, 1);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(fake.sleep_calls == 0);

    fake.now.tv_sec = I2C_LINUX_TIME_T_MAX - 1;
    fake.now.tv_nsec = 0;
    r = i2c_device_wait_us(&dev, 1000000);
    assert(r.status == I2C_STATUS_OK);
    assert(fake.deadlines[0].tv_sec == I2C_LINUX_TIME_T_MAX);

    assert(i2c_device_close(&dev).status == I2C_STATUS_OK);
}

/*
 * test_close_never_retried: a failed close is reported once, the device is
 * closed anyway, and nothing afterwards reaches a syscall.
 */
static void test_close_never_retried(void)
{
    static const int close_errnos[] = {EINTR, EIO};
    static const uint8_t cmd[] = {0x10};
    uint8_t buf[1];
    size_t i;

    for (i = 0; i < sizeof close_errnos / sizeof close_errnos[0]; i++) {
        i2c_device dev = {0};
        i2c_result r;
        unsigned calls;

        fake_reset();
        assert(open_fake(&dev, 0x23).status == I2C_STATUS_OK);
        fake.close_errno = close_errnos[i];
        r = i2c_device_close(&dev);
        assert(r.status == I2C_STATUS_OS_ERROR);
        assert(r.os_errno == close_errnos[i]);
        assert(fake.close_calls == 1 && fake.closed_fds[0] == FAKE_FIRST_FD);
        assert(dev.ops == NULL && dev.ctx == NULL);

        calls = fake_total_calls();
        assert(i2c_device_close(&dev).status == I2C_STATUS_INVALID_ARGUMENT);
        assert(i2c_device_write(&dev, cmd, sizeof cmd).status ==
               I2C_STATUS_INVALID_ARGUMENT);
        assert(i2c_device_read(&dev, buf, sizeof buf).status ==
               I2C_STATUS_INVALID_ARGUMENT);
        assert(i2c_device_wait_us(&dev, 1000).status ==
               I2C_STATUS_INVALID_ARGUMENT);
        assert(fake_total_calls() == calls);
    }
}

/* count_open_fds: number of descriptors this process holds right now. */
static int count_open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    assert(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') {
            count++;
        }
    }
    assert(closedir(dir) == 0);
    return count;
}

/* test_os_missing_bus: OS PLUMBING. A missing node is ENOENT, no leak. */
static void test_os_missing_bus(void)
{
    char dir[] = "/tmp/plant-monitor-i2c-XXXXXX";
    char path[64];
    i2c_device dev = {0};
    i2c_result r;
    int cleanup_errno = -1;
    int fds_before;

    assert(mkdtemp(dir) != NULL);
    assert(snprintf(path, sizeof path, "%s/i2c-1", dir) < (int)sizeof path);
    fds_before = count_open_fds();
    r = i2c_linux_open_path(&dev, path, 0x23, &i2c_linux_real_sys,
                            &cleanup_errno);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == ENOENT);
    assert(cleanup_errno == 0);
    assert(device_is_zeroed(&dev));
    assert(count_open_fds() == fds_before);
    assert(rmdir(dir) == 0);
}

/*
 * test_os_dev_null_refuses_slave: OS PLUMBING. Real I2C_SLAVE on /dev/null
 * fails with ENOTTY, and the descriptor opened for it is closed.
 */
static void test_os_dev_null_refuses_slave(void)
{
    i2c_device dev = {0};
    i2c_result r;
    int cleanup_errno = -1;
    int fds_before = count_open_fds();

    r = i2c_linux_open_path(&dev, "/dev/null", 0x23, &i2c_linux_real_sys,
                            &cleanup_errno);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == ENOTTY);
    assert(cleanup_errno == 0);
    assert(device_is_zeroed(&dev));
    assert(count_open_fds() == fds_before);
}

/* plumbing_fd: the real /dev/null descriptor seen by plumbing_ioctl_ul. */
static int plumbing_fd = -1;

/* plumbing_ioctl_ul: pretend /dev/null accepted the I2C_SLAVE request. */
static int plumbing_ioctl_ul(int fd, unsigned long request,
                             unsigned long arg)
{
    assert(request == I2C_SLAVE && arg == 0x23);
    plumbing_fd = fd;
    return 0;
}

/* monotonic_us: microseconds on CLOCK_MONOTONIC. */
static long long monotonic_us(void)
{
    struct timespec now;

    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (long long)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/*
 * test_os_dev_null_plumbing: OS PLUMBING. Real open, read, write, wait and
 * close on /dev/null, with only I2C_SLAVE faked. It shows the descriptor is
 * close-on-exec, that write() and a 0-byte read() map to OK and
 * SHORT_TRANSFER, that a real wait lasts at least its duration, and that
 * close releases the descriptor. It does not show an I2C transaction works.
 */
static void test_os_dev_null_plumbing(void)
{
    static const uint8_t cmd[] = {0x10, 0x20};
    i2c_linux_sys sys = i2c_linux_real_sys;
    i2c_device dev = {0};
    uint8_t buf[2] = {0xAA, 0xAA};
    i2c_result r;
    long long start_us;
    int cleanup_errno = -1;
    int fds_before = count_open_fds();

    sys.ioctl_ul = plumbing_ioctl_ul;
    r = i2c_linux_open_path(&dev, "/dev/null", 0x23, &sys, &cleanup_errno);
    assert(r.status == I2C_STATUS_OK && cleanup_errno == 0);
    assert(plumbing_fd >= 0);
    assert((fcntl(plumbing_fd, F_GETFD) & FD_CLOEXEC) != 0);
    assert(count_open_fds() == fds_before + 1);

    r = i2c_device_write(&dev, cmd, sizeof cmd);
    assert(r.status == I2C_STATUS_OK && r.transferred_len == 2);
    r = i2c_device_read(&dev, buf, sizeof buf);
    assert(r.status == I2C_STATUS_SHORT_TRANSFER);
    assert(r.os_errno == 0 && r.transferred_len == 0);
    assert(buf[0] == 0xAA && buf[1] == 0xAA);

    start_us = monotonic_us();
    r = i2c_device_wait_us(&dev, 2000);
    assert(r.status == I2C_STATUS_OK);
    assert(monotonic_us() - start_us >= 2000);

    r = i2c_device_close(&dev);
    assert(r.status == I2C_STATUS_OK);
    assert(fcntl(plumbing_fd, F_GETFD) == -1 && errno == EBADF);
    assert(count_open_fds() == fds_before);
}

/*
 * test_public_open_absent_bus: OS PLUMBING through the public entry point.
 * Bus UINT_MAX has no node, so the real open() fails with ENOENT and nothing
 * is held; bad arguments are refused before any syscall. The stat() guard
 * makes the test fail, not open anything, if such a node ever existed.
 */
static void test_public_open_absent_bus(void)
{
    char path[32];
    struct stat st;
    i2c_device dev = {0};
    i2c_device open_dev = {0};
    i2c_result r;
    int cleanup_errno = -1;
    int fds_before;

    assert(i2c_linux_bus_path(path, sizeof path, UINT_MAX) == 0);
    assert(stat(path, &st) == -1 && errno == ENOENT);
    fds_before = count_open_fds();

    r = i2c_linux_open(&dev, UINT_MAX, 0x23, &cleanup_errno);
    assert(r.status == I2C_STATUS_OS_ERROR && r.os_errno == ENOENT);
    assert(cleanup_errno == 0);
    assert(device_is_zeroed(&dev));

    cleanup_errno = -1;
    r = i2c_linux_open(&dev, UINT_MAX, 0x80, &cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT && cleanup_errno == 0);
    cleanup_errno = -1;
    r = i2c_linux_open(NULL, UINT_MAX, 0x23, &cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT && cleanup_errno == 0);
    r = i2c_linux_open(&dev, UINT_MAX, 0x23, NULL);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT);
    assert(device_is_zeroed(&dev));

    fake_reset();
    assert(open_fake(&open_dev, 0x36).status == I2C_STATUS_OK);
    cleanup_errno = -1;
    r = i2c_linux_open(&open_dev, UINT_MAX, 0x23, &cleanup_errno);
    assert(r.status == I2C_STATUS_INVALID_ARGUMENT && cleanup_errno == 0);
    assert(open_dev.address_7bit == 0x36 && open_dev.ops != NULL);
    assert(i2c_device_close(&open_dev).status == I2C_STATUS_OK);

    assert(count_open_fds() == fds_before);
}

/* main: run SIMULATED tests, then OS PLUMBING tests; abort on failure. */
int main(void)
{
    test_bus_path();
    test_open_binds_address();
    test_open_rejects_before_syscalls();
    test_open_alloc_failure();
    test_open_failure_frees_context();
    test_bind_failure_closes_fd();
    test_devices_own_descriptors();
    test_read_write_results();
    test_wait_deadline();
    test_wait_retries_eintr_same_deadline();
    test_wait_errors();
    test_wait_deadline_overflow();
    test_close_never_retried();
    puts("SIMULATED linux i2c backend tests passed (fake syscalls)");

    test_os_missing_bus();
    test_os_dev_null_refuses_slave();
    test_os_dev_null_plumbing();
    test_public_open_absent_bus();
    puts("OS PLUMBING linux i2c tests passed (/dev/null and missing paths; "
         "no I2C transaction verified, no I2C hardware used)");
    return 0;
}
