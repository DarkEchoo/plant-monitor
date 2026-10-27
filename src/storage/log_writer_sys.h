#ifndef STORAGE_LOG_WRITER_SYS_H
#define STORAGE_LOG_WRITER_SYS_H

/*
 * Internal syscall seam for the log writer. Only log_writer.c and its tests
 * use it; product code calls storage_log_open. Including files must define
 * _POSIX_C_SOURCE 200809L first (for ssize_t and struct stat).
 *
 * A replacement function that fails must set errno exactly as the real
 * call does: the writer copies it into the result without checking it.
 */

#include "storage/log_writer.h"

#include <sys/stat.h>
#include <sys/types.h>

typedef struct storage_log_sys {
    /* open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC) */
    int (*open_dir)(const char *path);
    /* openat(dir_fd, name, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC,
     * 0644) */
    int (*open_append_at)(int dir_fd, const char *name);
    int (*fstat)(int fd, struct stat *st);
    ssize_t (*pread)(int fd, void *buf, size_t len, off_t offset);
    ssize_t (*write)(int fd, const void *buf, size_t len);
    int (*fdatasync)(int fd);
    int (*fsync)(int fd);
    int (*close)(int fd);
} storage_log_sys;

extern const storage_log_sys storage_log_real_sys;

/* storage_log_open with an explicit syscall table. sys must outlive the
 * writer. */
storage_log_result storage_log_open_sys(storage_log_writer *w,
                                        const char *dir_path,
                                        const char *file_name,
                                        uint64_t next_sequence,
                                        int simulated,
                                        const storage_log_sys *sys);

#endif
