#ifndef STORAGE_LOG_WRITER_H
#define STORAGE_LOG_WRITER_H

#include "storage/record.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Append-only log writer; see "Batching and fsync" in docs/design.md.
 *
 * Records are encoded with storage_record_encode into an in-memory batch.
 * The batch is written and fdatasync'd when it holds
 * STORAGE_LOG_BATCH_RECORDS records, when storage_log_append or
 * storage_log_flush_if_due finds the oldest buffered record has waited
 * STORAGE_LOG_FLUSH_INTERVAL_NS, on storage_log_flush, and on
 * storage_log_close. The writer has no timer: while no record arrives, the
 * caller must call storage_log_flush_if_due to meet the 60 s limit.
 *
 * Opening a file syncs its directory, so a newly created file's name is
 * durable. Simulated and live records never share a file: a simulated
 * writer's file name must start with STORAGE_LOG_SIMULATED_PREFIX and a
 * live writer's must not, and an existing file's last record must carry
 * the same SIMULATED flag. An existing file is appended to only if its
 * size is a whole number of records and next_sequence follows its last
 * record; otherwise recovery must run first.
 *
 * A write or fdatasync failure is sticky: the writer reports
 * STORAGE_LOG_FAILED from then on and never retries, because after a
 * failed fdatasync the kernel may already have dropped the unwritten pages,
 * so a retry could report success for data that is not on disk. The caller
 * logs the error, closes the writer, and runs recovery before writing to
 * that file again. Records still buffered at that point are not written.
 *
 * The writer does not log. Every result carries the failing stage and
 * errno so the caller (the hub) can log it.
 *
 * File naming, rotation, retention, and recovery are not part of this
 * writer.
 */

#define STORAGE_LOG_BATCH_RECORDS 32
#define STORAGE_LOG_BATCH_BYTES \
    (STORAGE_LOG_BATCH_RECORDS * STORAGE_RECORD_SIZE_BYTES)
#define STORAGE_LOG_FLUSH_INTERVAL_NS UINT64_C(60000000000)  /* 60 s */
#define STORAGE_LOG_SIMULATED_PREFIX "SIMULATED-"

typedef enum {
    STORAGE_LOG_OK = 0,
    STORAGE_LOG_ARGUMENT,            /* a NULL pointer; nothing changed */
    STORAGE_LOG_NOT_OPEN,            /* the writer has no open file */
    STORAGE_LOG_MISALIGNED,          /* existing file is not whole records */
    STORAGE_LOG_BAD_RECORD,          /* unknown type or reserved flag bit */
    STORAGE_LOG_SIMULATED_MISMATCH,  /* SIMULATED flag disagrees with file */
    STORAGE_LOG_SEQUENCE,            /* sequence is not next_sequence */
    STORAGE_LOG_OS_ERROR,            /* a system call failed; see stage */
    STORAGE_LOG_FAILED,              /* an earlier write or sync failed */
    STORAGE_LOG_ALREADY_OPEN,        /* open on a writer that is open */
    STORAGE_LOG_BAD_TAIL             /* last record unreadable; recover */
} storage_log_status;

/* The system call that failed, for STORAGE_LOG_OS_ERROR. */
typedef enum {
    STORAGE_LOG_STAGE_NONE = 0,
    STORAGE_LOG_STAGE_OPEN_DIR,
    STORAGE_LOG_STAGE_OPEN_FILE,
    STORAGE_LOG_STAGE_STAT,
    STORAGE_LOG_STAGE_SYNC_DIR,
    STORAGE_LOG_STAGE_CLOSE_DIR,
    STORAGE_LOG_STAGE_WRITE,
    STORAGE_LOG_STAGE_SYNC,
    STORAGE_LOG_STAGE_CLOSE_FILE,
    STORAGE_LOG_STAGE_READ_TAIL
} storage_log_stage;

typedef struct {
    storage_log_status status;
    storage_log_stage stage;  /* STORAGE_LOG_STAGE_NONE unless OS_ERROR */
    int os_errno;             /* errno of the failing call, or 0 */
} storage_log_result;

struct storage_log_sys;

/*
 * One log file. Read the fields; change them only through the API. A
 * writer that is all zeros (static or {0}) is closed and needs no init.
 */
typedef struct {
    int is_open;                  /* nonzero between open and close */
    int fd;                       /* the file; -1 after a failed open
                                   * or close; 0 is a valid descriptor */
    int simulated;                /* nonzero: every record is SIMULATED */
    int failed;                   /* nonzero after a write or sync error */
    uint64_t next_sequence;       /* sequence the next record must have */
    uint64_t file_size_bytes;     /* bytes written to the file so far */
    size_t buffered_records;      /* encoded but not yet written */
    uint64_t oldest_buffered_ns;  /* monotonic time of first buffered */
    uint8_t buffer[STORAGE_LOG_BATCH_BYTES];
    const struct storage_log_sys *sys;
} storage_log_writer;

/*
 * Open or create dir_path/file_name for appending, then sync dir_path.
 * next_sequence is the sequence the first appended record must carry.
 * simulated selects whether every record must have, or must not have,
 * STORAGE_FLAG_SIMULATED, and must match the file name's prefix.
 *
 * If the file is not empty, its last record must decode with a known type
 * (else STORAGE_LOG_BAD_TAIL), have the same SIMULATED flag (else
 * STORAGE_LOG_SIMULATED_MISMATCH), and be followed by next_sequence (else
 * STORAGE_LOG_SEQUENCE).
 *
 * On ARGUMENT or ALREADY_OPEN, *w is unchanged. On any other failure no
 * descriptor stays open, w->is_open is 0, and w->fd is -1.
 */
storage_log_result storage_log_open(storage_log_writer *w,
                                    const char *dir_path,
                                    const char *file_name,
                                    uint64_t next_sequence, int simulated);

/*
 * Check *rec, encode it into the batch, and write the batch if it is now
 * full or its oldest record has waited STORAGE_LOG_FLUSH_INTERVAL_NS.
 * now_monotonic_ns is the hub's CLOCK_MONOTONIC time, used only for the
 * flush deadline. A rejected record is not buffered.
 */
storage_log_result storage_log_append(storage_log_writer *w,
                                      const storage_record *rec,
                                      uint64_t now_monotonic_ns);

/* Write and fdatasync the batch if its oldest record has waited at least
 * STORAGE_LOG_FLUSH_INTERVAL_NS by now_monotonic_ns. */
storage_log_result storage_log_flush_if_due(storage_log_writer *w,
                                            uint64_t now_monotonic_ns);

/* Write and fdatasync the batch now. An empty batch makes no call. */
storage_log_result storage_log_flush(storage_log_writer *w);

/*
 * Flush unless the writer has failed, then close the file. The descriptor
 * is closed, w->is_open set to 0, and w->fd set to -1 whatever happens.
 * Returns the first error; STORAGE_LOG_FAILED if the writer had already
 * failed.
 */
storage_log_result storage_log_close(storage_log_writer *w);

#endif
