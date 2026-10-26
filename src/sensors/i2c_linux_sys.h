#ifndef SENSORS_I2C_LINUX_SYS_H
#define SENSORS_I2C_LINUX_SYS_H

/*
 * Internal syscall seam for the Linux I2C backend. Only i2c_linux.c and its
 * tests use it; product code calls i2c_linux_open. Including files must
 * define _POSIX_C_SOURCE 200809L first (for ssize_t and struct timespec).
 *
 * The backend passes the ioctl request, clock and sleep flags through the
 * seam, so tests can check them. A replacement function that fails must
 * report its error exactly as the real call does (set errno, or for
 * clock_sleep return the error number): the backend copies it into the
 * result without checking it.
 */

#include "sensors/i2c_transport.h"

#include <limits.h>
#include <sys/types.h>
#include <time.h>

/* Largest time_t; Linux time_t is a signed 32- or 64-bit integer. */
#define I2C_LINUX_TIME_T_MAX \
    ((time_t)(((uintmax_t)1 << (sizeof(time_t) * CHAR_BIT - 1)) - 1))

typedef struct {
    /* calloc(1, size); the result is released with free(). */
    void *(*alloc_zeroed)(size_t size);
    /* open(path, O_RDWR | O_CLOEXEC); -1 and errno on failure. */
    int (*open_rdwr_cloexec)(const char *path);
    /* ioctl(fd, request, arg); -1 and errno on failure. */
    int (*ioctl_ul)(int fd, unsigned long request, unsigned long arg);
    ssize_t (*read)(int fd, void *buf, size_t len);
    ssize_t (*write)(int fd, const void *buf, size_t len);
    int (*close)(int fd);
    /* clock_gettime(clock, now); -1 and errno on failure. */
    int (*clock_now)(clockid_t clock, struct timespec *now);
    /* clock_nanosleep(clock, flags, deadline, NULL): returns 0 or an error
     * number directly, not through errno. */
    int (*clock_sleep)(clockid_t clock, int flags,
                       const struct timespec *deadline);
} i2c_linux_sys;

extern const i2c_linux_sys i2c_linux_real_sys;

/* Write "/dev/i2c-<bus_number>" into buf. Returns 0, or -1 if it does not
 * fit. */
int i2c_linux_bus_path(char *buf, size_t size, unsigned bus_number);

/* i2c_linux_open with an explicit path and syscall table. sys must outlive
 * the device. cleanup_errno is as for i2c_linux_open. */
i2c_result i2c_linux_open_path(i2c_device *dev, const char *path,
                               uint8_t address_7bit,
                               const i2c_linux_sys *sys, int *cleanup_errno);

#endif
