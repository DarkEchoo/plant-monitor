#ifndef CAPTURE_STORE_H
#define CAPTURE_STORE_H

#include "sensors/capture.h"
#include "storage/log_writer.h"

#include <stdint.h>
#include <stdio.h>

/*
 * capture_store: a capture_sink that writes each capture result to the
 * text log (sensors/capture_log.h) AND appends it as a storage record
 * (storage/capture_record.h, storage/log_writer.h).
 *
 * TEMPORARY INTEGRATION, until the hub exists. docs/design.md has one hub
 * that numbers every record and writes one log. Here each capture process
 * owns its own writer instead:
 * - one storage directory per sensor; the light and soil logs are
 *   separate files;
 * - sequence numbers are per process (1, 2, 3 ... in that file), NOT
 *   hub-wide, and say nothing about ordering between the two sensors;
 * - every run starts a fresh file at sequence 1 with a START record. A
 *   previous run's log in the same directory is refused by the writer
 *   (STORAGE_LOG_SEQUENCE), because recovery, rotation and retention are
 *   not implemented.
 * Every storage line in the text log says storage=temporary-per-sensor.
 *
 * Flushing follows the writer's batch rule. On each heartbeat the batch is
 * also flushed if it would be overdue by the next tick, so no record waits
 * longer than STORAGE_LOG_FLUSH_INTERVAL_NS even though the writer has no
 * timer. Every storage failure is logged with its operation, status,
 * stage and errno, and returned as an errno, which makes capture_run stop
 * with CAPTURE_EXIT_SINK_FAILED (a writer failure is sticky by design).
 */

#define CAPTURE_STORE_FILE_NAME "plant-0000000000000001.log"
#define CAPTURE_STORE_SIMULATED_FILE_NAME \
    STORAGE_LOG_SIMULATED_PREFIX CAPTURE_STORE_FILE_NAME

typedef struct {
    FILE *log;                  /* the text log */
    const capture_env *env;     /* clocks and trust, for START and flush */
    capture_sensor sensor;
    int simulated;
    int64_t tick_period_ns;
    storage_log_writer writer;
    uint64_t records_appended;  /* including START */
} capture_store;

/*
 * Open dir_path's log for cfg->sensor at sequence 1, then append and flush
 * a START record. env and log must outlive the store. Returns 0, or an
 * errno after logging the failure; on failure the writer is closed.
 */
int capture_store_open(capture_store *st, FILE *log, const char *dir_path,
                       const capture_config *cfg, const capture_env *env);

/* capture_sink callbacks; ctx is the capture_store. */
int capture_store_emit_sample(void *ctx, const capture_sample *sample);
int capture_store_emit_heartbeat(void *ctx,
                                 const capture_heartbeat *heartbeat);

/* Flush and close the writer, logging the result. Returns 0 or an errno. */
int capture_store_close(capture_store *st);

#endif
