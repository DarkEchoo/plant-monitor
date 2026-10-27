#define _POSIX_C_SOURCE 200809L

#include "storage/log_writer.h"
#include "log_writer_sys.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define STORAGE_FLAGS_KNOWN                                         \
    (STORAGE_FLAG_VALID | STORAGE_FLAG_SIMULATED |                  \
     STORAGE_FLAG_CLOCK_UNTRUSTED | STORAGE_FLAG_TIME_UNAVAILABLE)
#define LOG_FILE_MODE 0644

/* real_open_dir: open a directory read-only so it can be fsync'd. */
static int real_open_dir(const char *path)
{
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

/* real_open_append_at: create or open dir_fd/name for appending. It is
 * opened read-write only so open can pread the last record; O_APPEND
 * still sends every write to the end. */
static int real_open_append_at(int dir_fd, const char *name)
{
    return openat(dir_fd, name, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC,
                  LOG_FILE_MODE);
}

/* real_fdatasync: fdatasync on Linux, the target. macOS, used only to run
 * the tests, does not declare fdatasync, so it uses the stronger fsync. */
static int real_fdatasync(int fd)
{
#ifdef __APPLE__
    return fsync(fd);
#else
    return fdatasync(fd);
#endif
}

const storage_log_sys storage_log_real_sys = {
    real_open_dir, real_open_append_at, fstat, pread, write,
    real_fdatasync, fsync, close
};

/* log_result: build a writer result. */
static storage_log_result log_result(storage_log_status status,
                                     storage_log_stage stage, int os_errno)
{
    storage_log_result result = {status, stage, os_errno};
    return result;
}

/* log_ok: the success result. */
static storage_log_result log_ok(void)
{
    return log_result(STORAGE_LOG_OK, STORAGE_LOG_STAGE_NONE, 0);
}

/* log_status: a result with no failing system call. */
static storage_log_result log_status(storage_log_status status)
{
    return log_result(status, STORAGE_LOG_STAGE_NONE, 0);
}

/* log_os_error: a failed system call at stage, with its errno. */
static storage_log_result log_os_error(storage_log_stage stage,
                                       int os_errno)
{
    return log_result(STORAGE_LOG_OS_ERROR, stage, os_errno);
}

/* check_open: reject a NULL or closed writer, or one that has failed. */
static storage_log_result check_open(const storage_log_writer *w)
{
    if (w == NULL)
        return log_status(STORAGE_LOG_ARGUMENT);
    if (!w->is_open)
        return log_status(STORAGE_LOG_NOT_OPEN);
    if (w->failed)
        return log_status(STORAGE_LOG_FAILED);
    return log_ok();
}

/* record_is_known: the record type is known and no reserved flag is set. */
static int record_is_known(const storage_record *rec)
{
    return rec->record_type >= STORAGE_RECORD_TYPE_LIGHT &&
           rec->record_type <= STORAGE_RECORD_TYPE_START &&
           (rec->flags & ~STORAGE_FLAGS_KNOWN) == 0;
}

/* name_is_simulated: file_name starts with STORAGE_LOG_SIMULATED_PREFIX. */
static int name_is_simulated(const char *file_name)
{
    return strncmp(file_name, STORAGE_LOG_SIMULATED_PREFIX,
                   sizeof STORAGE_LOG_SIMULATED_PREFIX - 1) == 0;
}

/*
 * check_tail: read and decode the last record of a non-empty file, and
 * check this writer continues it: the same SIMULATED flag, and
 * next_sequence one past its sequence. EINTR is retried.
 */
static storage_log_result check_tail(int fd, off_t size_bytes,
                                     uint64_t next_sequence, int simulated,
                                     const storage_log_sys *sys)
{
    uint8_t tail[STORAGE_RECORD_SIZE_BYTES];
    storage_record last;
    ssize_t n;

    do {
        n = sys->pread(fd, tail, sizeof tail,
                       size_bytes - STORAGE_RECORD_SIZE_BYTES);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return log_os_error(STORAGE_LOG_STAGE_READ_TAIL, errno);
    if ((size_t)n != sizeof tail ||
        storage_record_decode(tail, &last) != STORAGE_RECORD_OK ||
        !record_is_known(&last))
        return log_status(STORAGE_LOG_BAD_TAIL);
    if (((last.flags & STORAGE_FLAG_SIMULATED) != 0) != simulated)
        return log_status(STORAGE_LOG_SIMULATED_MISMATCH);
    if (last.sequence == UINT64_MAX || last.sequence + 1 != next_sequence)
        return log_status(STORAGE_LOG_SEQUENCE);
    return log_ok();
}

/*
 * storage_log_open_sys: open the directory, open or create the file in it,
 * check the file holds whole records and that this writer continues its
 * last record, sync the directory so the name is durable, then close the
 * directory. On failure everything opened here is closed and the first
 * error is returned; cleanup close errors after an earlier error are not
 * reported, because the earlier error is the cause.
 */
storage_log_result storage_log_open_sys(storage_log_writer *w,
                                        const char *dir_path,
                                        const char *file_name,
                                        uint64_t next_sequence,
                                        int simulated,
                                        const storage_log_sys *sys)
{
    storage_log_result result = log_ok();
    struct stat st;
    int dir_fd;
    int file_fd = -1;

    if (w == NULL || dir_path == NULL || file_name == NULL || sys == NULL)
        return log_status(STORAGE_LOG_ARGUMENT);
    if (w->is_open)
        return log_status(STORAGE_LOG_ALREADY_OPEN);
    w->fd = -1;
    if (name_is_simulated(file_name) != (simulated != 0))
        return log_status(STORAGE_LOG_SIMULATED_MISMATCH);

    dir_fd = sys->open_dir(dir_path);
    if (dir_fd < 0)
        return log_os_error(STORAGE_LOG_STAGE_OPEN_DIR, errno);

    file_fd = sys->open_append_at(dir_fd, file_name);
    if (file_fd < 0) {
        result = log_os_error(STORAGE_LOG_STAGE_OPEN_FILE, errno);
        goto close_dir;
    }
    if (sys->fstat(file_fd, &st) != 0) {
        result = log_os_error(STORAGE_LOG_STAGE_STAT, errno);
        goto close_file;
    }
    if (st.st_size < 0 || st.st_size % STORAGE_RECORD_SIZE_BYTES != 0) {
        result = log_status(STORAGE_LOG_MISALIGNED);
        goto close_file;
    }
    if (st.st_size > 0) {
        result = check_tail(file_fd, st.st_size, next_sequence,
                            simulated != 0, sys);
        if (result.status != STORAGE_LOG_OK)
            goto close_file;
    }
    if (sys->fsync(dir_fd) != 0) {
        result = log_os_error(STORAGE_LOG_STAGE_SYNC_DIR, errno);
        goto close_file;
    }
    if (sys->close(dir_fd) != 0) {
        result = log_os_error(STORAGE_LOG_STAGE_CLOSE_DIR, errno);
        (void)sys->close(file_fd);  /* not retried; result has the cause */
        return result;
    }

    w->is_open = 1;
    w->fd = file_fd;
    w->simulated = simulated != 0;
    w->failed = 0;
    w->next_sequence = next_sequence;
    w->file_size_bytes = (uint64_t)st.st_size;
    w->buffered_records = 0;
    w->oldest_buffered_ns = 0;
    w->sys = sys;
    return result;

close_file:
    (void)sys->close(file_fd);
close_dir:
    (void)sys->close(dir_fd);
    return result;
}

/* storage_log_open: storage_log_open_sys with the real system calls. */
storage_log_result storage_log_open(storage_log_writer *w,
                                    const char *dir_path,
                                    const char *file_name,
                                    uint64_t next_sequence, int simulated)
{
    return storage_log_open_sys(w, dir_path, file_name, next_sequence,
                                simulated, &storage_log_real_sys);
}

/*
 * storage_log_flush: write the whole batch, retrying EINTR and continuing
 * after a short write, then fdatasync. Any other failure marks the writer
 * failed; nothing is retried after that (see log_writer.h).
 */
storage_log_result storage_log_flush(storage_log_writer *w)
{
    storage_log_result result = check_open(w);
    size_t len_bytes;
    size_t done_bytes = 0;

    if (result.status != STORAGE_LOG_OK || w->buffered_records == 0)
        return result;

    len_bytes = w->buffered_records * STORAGE_RECORD_SIZE_BYTES;
    while (done_bytes < len_bytes) {
        ssize_t n = w->sys->write(w->fd, w->buffer + done_bytes,
                                  len_bytes - done_bytes);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            /* write() returning 0 for a nonzero length has no errno. */
            w->failed = 1;
            return log_os_error(STORAGE_LOG_STAGE_WRITE, n < 0 ? errno : 0);
        }
        done_bytes += (size_t)n;
        w->file_size_bytes += (uint64_t)n;
    }

    if (w->sys->fdatasync(w->fd) != 0) {
        w->failed = 1;
        return log_os_error(STORAGE_LOG_STAGE_SYNC, errno);
    }
    w->buffered_records = 0;
    return result;
}

/* batch_due: records are buffered and the oldest has waited at least
 * STORAGE_LOG_FLUSH_INTERVAL_NS by now_monotonic_ns. */
static int batch_due(const storage_log_writer *w, uint64_t now_monotonic_ns)
{
    return w->buffered_records > 0 &&
           now_monotonic_ns >= w->oldest_buffered_ns &&
           now_monotonic_ns - w->oldest_buffered_ns >=
               STORAGE_LOG_FLUSH_INTERVAL_NS;
}

/* storage_log_append: check rec, encode it into the batch, and flush the
 * batch when it is full or its oldest record is due. */
storage_log_result storage_log_append(storage_log_writer *w,
                                      const storage_record *rec,
                                      uint64_t now_monotonic_ns)
{
    storage_log_result result;
    int rec_simulated;

    if (w == NULL || rec == NULL)
        return log_status(STORAGE_LOG_ARGUMENT);
    result = check_open(w);
    if (result.status != STORAGE_LOG_OK)
        return result;

    if (!record_is_known(rec))
        return log_status(STORAGE_LOG_BAD_RECORD);
    rec_simulated = (rec->flags & STORAGE_FLAG_SIMULATED) != 0;
    if (rec_simulated != w->simulated)
        return log_status(STORAGE_LOG_SIMULATED_MISMATCH);
    if (rec->sequence != w->next_sequence)
        return log_status(STORAGE_LOG_SEQUENCE);

    /* Cannot fail: both pointers were checked above. */
    (void)storage_record_encode(
        rec, w->buffer + w->buffered_records * STORAGE_RECORD_SIZE_BYTES);
    if (w->buffered_records == 0)
        w->oldest_buffered_ns = now_monotonic_ns;
    w->buffered_records++;
    w->next_sequence++;

    if (w->buffered_records == STORAGE_LOG_BATCH_RECORDS ||
        batch_due(w, now_monotonic_ns))
        return storage_log_flush(w);
    return result;
}

/* storage_log_flush_if_due: flush once the oldest buffered record has
 * waited STORAGE_LOG_FLUSH_INTERVAL_NS. */
storage_log_result storage_log_flush_if_due(storage_log_writer *w,
                                            uint64_t now_monotonic_ns)
{
    storage_log_result result = check_open(w);

    if (result.status != STORAGE_LOG_OK || !batch_due(w, now_monotonic_ns))
        return result;
    return storage_log_flush(w);
}

/* storage_log_close: flush unless failed, then close the file once.
 * close() is not retried on EINTR: on Linux the descriptor is already
 * released, and retrying could close a descriptor reused by another
 * thread or process. */
storage_log_result storage_log_close(storage_log_writer *w)
{
    storage_log_result result;

    if (w == NULL)
        return log_status(STORAGE_LOG_ARGUMENT);
    if (!w->is_open)
        return log_status(STORAGE_LOG_NOT_OPEN);

    result = w->failed ? log_status(STORAGE_LOG_FAILED)
                       : storage_log_flush(w);
    if (w->sys->close(w->fd) != 0 && result.status == STORAGE_LOG_OK)
        result = log_os_error(STORAGE_LOG_STAGE_CLOSE_FILE, errno);
    w->is_open = 0;
    w->fd = -1;
    return result;
}
