#ifndef STORAGE_RECORD_H
#define STORAGE_RECORD_H

#include <stdint.h>

/*
 * Fixed 48-byte storage record; see "Record format" in docs/design.md.
 *
 * Every field is written and read byte by byte in little-endian order, so
 * the layout depends on neither host byte order nor struct padding. Bytes
 * 44-47 hold the CRC-32 (storage/crc32.h) of bytes 0-43.
 *
 * Decode checks, in order, the magic, the version, and the CRC, and
 * reports the first that fails. It does not check the record type, the
 * reserved flag bits, or which fields must be 0; those are left to the
 * caller. On any failure *out is left untouched.
 */

#define STORAGE_RECORD_SIZE_BYTES 48
#define STORAGE_RECORD_CRC_OFFSET 44
#define STORAGE_RECORD_MAGIC      UINT32_C(0x544E4C50)  /* "PLNT" in file order */
#define STORAGE_RECORD_VERSION    1

#define STORAGE_RECORD_TYPE_LIGHT 1
#define STORAGE_RECORD_TYPE_SOIL  2
#define STORAGE_RECORD_TYPE_START 3

#define STORAGE_FLAG_VALID            UINT16_C(0x0001)
#define STORAGE_FLAG_SIMULATED        UINT16_C(0x0002)
#define STORAGE_FLAG_CLOCK_UNTRUSTED  UINT16_C(0x0004)
#define STORAGE_FLAG_TIME_UNAVAILABLE UINT16_C(0x0008)

typedef enum {
    STORAGE_RECORD_OK = 0,
    STORAGE_RECORD_ARGUMENT,     /* a NULL pointer; nothing written */
    STORAGE_RECORD_BAD_MAGIC,    /* bytes 0-3 are not "PLNT" */
    STORAGE_RECORD_BAD_VERSION,  /* byte 4 is not STORAGE_RECORD_VERSION */
    STORAGE_RECORD_BAD_CRC       /* bytes 44-47 do not match bytes 0-43 */
} storage_record_result;

/* One record's fields. The magic and version are implied, not stored. */
typedef struct {
    uint8_t record_type;       /* STORAGE_RECORD_TYPE_* */
    uint16_t flags;            /* STORAGE_FLAG_* */
    uint64_t sequence;         /* hub sequence number */
    uint64_t wall_clock_ns;    /* CLOCK_REALTIME, nanoseconds */
    uint64_t monotonic_ns;     /* CLOCK_MONOTONIC, nanoseconds */
    uint32_t sensor_value;     /* millilux, counts, or rejected value */
    uint8_t i2c_status;
    uint8_t failure_stage;     /* decoded using record_type */
    uint16_t os_errno;
    uint32_t source_sequence;  /* capture process counter */
} storage_record;

/* Write in as 48 bytes, including magic, version, and CRC, into out. */
storage_record_result storage_record_encode(
    const storage_record *in, uint8_t out[STORAGE_RECORD_SIZE_BYTES]);

/* Check magic, version, and CRC in that order, then fill *out. */
storage_record_result storage_record_decode(
    const uint8_t in[STORAGE_RECORD_SIZE_BYTES], storage_record *out);

#endif
