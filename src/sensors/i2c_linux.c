#define _POSIX_C_SOURCE 200809L

#include "sensors/i2c_linux.h"
#include "i2c_linux_sys.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NSEC_PER_SEC 1000000000L
#define USEC_PER_SEC 1000000UL
#define NSEC_PER_USEC 1000L

/* One open device: its descriptor and the syscalls that reach it. */
typedef struct {
    int fd;
    const i2c_linux_sys *sys;
} linux_ctx;

/* linux_result: build a backend result. */
static i2c_result linux_result(i2c_status status, int os_errno,
                               size_t requested_len, size_t transferred_len)
{
    i2c_result result = {status, os_errno, requested_len, transferred_len};
    return result;
}

/* transfer_result: map a read()/write() return value and its errno. */
static i2c_result transfer_result(ssize_t n, int saved_errno, size_t len)
{
    if (n < 0) {
        return linux_result(I2C_STATUS_OS_ERROR, saved_errno, len, 0);
    }
    return linux_result(I2C_STATUS_OK, 0, len, (size_t)n);
}

/* linux_write: one write() transaction; EINTR is reported, not retried. */
static i2c_result linux_write(void *ctx, const uint8_t *buf, size_t len)
{
    linux_ctx *c = ctx;
    ssize_t n = c->sys->write(c->fd, buf, len);
    int saved_errno = errno;

    return transfer_result(n, saved_errno, len);
}

/* linux_read: one read() transaction; EINTR is reported, not retried. */
static i2c_result linux_read(void *ctx, uint8_t *buf, size_t len)
{
    linux_ctx *c = ctx;
    ssize_t n = c->sys->read(c->fd, buf, len);
    int saved_errno = errno;

    return transfer_result(n, saved_errno, len);
}

/*
 * deadline_after_us: add duration_us to now. Returns -1 if the result does
 * not fit in time_t. The nanosecond sum is below 2 * 10^9, which fits a
 * 32-bit long (the Raspberry Pi's 32-bit OS).
 */
static int deadline_after_us(struct timespec *deadline,
                             const struct timespec *now,
                             uint32_t duration_us)
{
    time_t add_sec = (time_t)(duration_us / USEC_PER_SEC);
    long nsec = now->tv_nsec +
                (long)(duration_us % USEC_PER_SEC) * NSEC_PER_USEC;

    if (nsec >= NSEC_PER_SEC) {
        nsec -= NSEC_PER_SEC;
        add_sec++;
    }
    if (now->tv_sec > I2C_LINUX_TIME_T_MAX - add_sec) {
        return -1;
    }
    deadline->tv_sec = now->tv_sec + add_sec;
    deadline->tv_nsec = nsec;
    return 0;
}

/*
 * linux_wait_us: sleep until a CLOCK_MONOTONIC deadline fixed before the
 * first sleep. EINTR resumes toward the same deadline, so a wait is never
 * cut short and never grows; once the deadline passes, the sleep returns 0.
 */
static i2c_result linux_wait_us(void *ctx, uint32_t duration_us)
{
    linux_ctx *c = ctx;
    struct timespec now;
    struct timespec deadline;
    int rc;

    if (c->sys->clock_now(CLOCK_MONOTONIC, &now) != 0) {
        return linux_result(I2C_STATUS_OS_ERROR, errno, 0, 0);
    }
    if (deadline_after_us(&deadline, &now, duration_us) != 0) {
        return linux_result(I2C_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }
    do {
        rc = c->sys->clock_sleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                 &deadline);
    } while (rc == EINTR);
    if (rc != 0) {
        return linux_result(I2C_STATUS_OS_ERROR, rc, 0, 0);
    }
    return linux_result(I2C_STATUS_OK, 0, 0, 0);
}

/*
 * linux_close: close the descriptor exactly once and free the context.
 * close() is never retried: Linux releases the descriptor even when close()
 * fails, so a retry could close an unrelated descriptor that reused it.
 */
static i2c_result linux_close(void *ctx)
{
    linux_ctx *c = ctx;
    int rc = c->sys->close(c->fd);
    int saved_errno = errno;

    free(c);
    if (rc != 0) {
        return linux_result(I2C_STATUS_OS_ERROR, saved_errno, 0, 0);
    }
    return linux_result(I2C_STATUS_OK, 0, 0, 0);
}

/* linux_ops: transport callbacks for an open Linux device. */
static const i2c_device_ops linux_ops = {
    linux_write,
    linux_read,
    linux_wait_us,
    linux_close
};

/* real_alloc_zeroed: calloc one zeroed block. */
static void *real_alloc_zeroed(size_t size)
{
    return calloc(1, size);
}

/* real_open_rdwr_cloexec: open a bus node, close-on-exec. */
static int real_open_rdwr_cloexec(const char *path)
{
    return open(path, O_RDWR | O_CLOEXEC);
}

/* real_ioctl_ul: ioctl with one unsigned long argument. */
static int real_ioctl_ul(int fd, unsigned long request, unsigned long arg)
{
    return ioctl(fd, request, arg);
}

/* real_clock_now: read a clock. */
static int real_clock_now(clockid_t clock, struct timespec *now)
{
    return clock_gettime(clock, now);
}

/* real_clock_sleep: sleep on a clock; returns 0 or an error number. */
static int real_clock_sleep(clockid_t clock, int flags,
                            const struct timespec *deadline)
{
    return clock_nanosleep(clock, flags, deadline, NULL);
}

/* i2c_linux_real_sys: the seam table of real syscalls used in product. */
const i2c_linux_sys i2c_linux_real_sys = {
    real_alloc_zeroed,
    real_open_rdwr_cloexec,
    real_ioctl_ul,
    read,
    write,
    close,
    real_clock_now,
    real_clock_sleep
};

/* i2c_linux_bus_path: format the device node for a bus number. */
int i2c_linux_bus_path(char *buf, size_t size, unsigned bus_number)
{
    int n;

    if (buf == NULL || size == 0) {
        return -1;
    }
    n = snprintf(buf, size, "/dev/i2c-%u", bus_number);
    if (n < 0 || (size_t)n >= size) {
        return -1;
    }
    return 0;
}

/*
 * i2c_linux_open_path: validate, allocate the context, open the node, bind
 * the address with I2C_SLAVE (never I2C_SLAVE_FORCE), and only then fill
 * dev. Each failure releases what was acquired, in reverse order, and
 * leaves dev untouched. If binding fails, the result keeps the bind errno
 * and a failure of the cleanup close() goes to *cleanup_errno instead, so
 * neither error hides the other. That close() is never retried.
 */
i2c_result i2c_linux_open_path(i2c_device *dev, const char *path,
                               uint8_t address_7bit,
                               const i2c_linux_sys *sys, int *cleanup_errno)
{
    i2c_result result;
    linux_ctx *ctx;

    if (cleanup_errno != NULL) {
        *cleanup_errno = 0;
    }
    if (dev == NULL || dev->ops != NULL || path == NULL || sys == NULL ||
        cleanup_errno == NULL || address_7bit > I2C_ADDRESS_7BIT_MAX) {
        return linux_result(I2C_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }

    ctx = sys->alloc_zeroed(sizeof *ctx);
    if (ctx == NULL) {
        return linux_result(I2C_STATUS_OS_ERROR, ENOMEM, 0, 0);
    }
    ctx->sys = sys;

    ctx->fd = sys->open_rdwr_cloexec(path);
    if (ctx->fd < 0) {
        result = linux_result(I2C_STATUS_OS_ERROR, errno, 0, 0);
        goto free_ctx;
    }

    if (sys->ioctl_ul(ctx->fd, I2C_SLAVE, address_7bit) != 0) {
        result = linux_result(I2C_STATUS_OS_ERROR, errno, 0, 0);
        goto close_fd;
    }

    dev->ops = &linux_ops;
    dev->ctx = ctx;
    dev->address_7bit = address_7bit;
    dev->is_simulated = 0;
    return linux_result(I2C_STATUS_OK, 0, 0, 0);

close_fd:
    if (sys->close(ctx->fd) != 0) {
        *cleanup_errno = errno;
    }
free_ctx:
    free(ctx);
    return result;
}

/*
 * i2c_linux_open: open /dev/i2c-<bus_number> for one address. path holds
 * "/dev/i2c-" plus any unsigned value, so the format check is defensive.
 */
i2c_result i2c_linux_open(i2c_device *dev, unsigned bus_number,
                          uint8_t address_7bit, int *cleanup_errno)
{
    char path[32];

    if (i2c_linux_bus_path(path, sizeof path, bus_number) != 0) {
        if (cleanup_errno != NULL) {
            *cleanup_errno = 0;
        }
        return linux_result(I2C_STATUS_INVALID_ARGUMENT, 0, 0, 0);
    }
    return i2c_linux_open_path(dev, path, address_7bit, &i2c_linux_real_sys,
                               cleanup_errno);
}
