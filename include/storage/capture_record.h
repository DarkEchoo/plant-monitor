#ifndef STORAGE_CAPTURE_RECORD_H
#define STORAGE_CAPTURE_RECORD_H

#include "sensors/capture.h"
#include "storage/record.h"

#include <stdint.h>

/*
 * Build storage records from capture results, following "Record format"
 * in docs/design.md:
 * - record_type: LIGHT or SOIL from the sample's sensor.
 * - VALID only when the driver reported overall success (sample.valid).
 * - SIMULATED from the sample.
 * - CLOCK_UNTRUSTED unless the clock trust is TRUSTED (UNKNOWN counts as
 *   untrusted).
 * - TIME_UNAVAILABLE when either timestamp could not be read (or is
 *   negative, which a u64 cannot hold); that timestamp is stored as 0.
 * - sensor_value: the value when valid; when invalid, the soil
 *   rejected_value if there is one, else 0.
 * - i2c_status, failure_stage, os_errno: 0 for a valid record; otherwise
 *   from the sample, with an errno outside 0-65535 stored as 0xFFFF.
 * - source_sequence: the capture counter.
 * sequence is the storage sequence the caller assigns.
 */

#define STORAGE_ERRNO_UNREPRESENTABLE UINT16_C(0xFFFF)

/* storage_record_from_sample: fill *rec from *s. */
void storage_record_from_sample(const capture_sample *s, uint64_t sequence,
                                storage_record *rec);

/*
 * storage_record_start: fill *rec with a START record. The timestamps and
 * trust follow the same rules as a sample's; bytes 32-39 are 0.
 */
void storage_record_start(uint64_t sequence, int simulated,
                          int64_t wall_ns, int wall_errno,
                          int64_t mono_ns, int mono_errno,
                          capture_clock_trust trust, storage_record *rec);

#endif
