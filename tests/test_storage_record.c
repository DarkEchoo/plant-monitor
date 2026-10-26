#include "storage/crc32.h"
#include "storage/record.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests rely on assert; do not build them with NDEBUG"
#endif

/*
 * Every record here is built in memory by the test. No sensor or file is
 * used, and no value here is a measurement.
 */

/*
 * Layout vector: a soil record whose fields hold distinct byte patterns so
 * any misplaced or byte-swapped field shows up. The values are chosen for
 * layout coverage, not to look like a real reading. Bytes 0-43 are written
 * by hand from the design.md table. The CRC was computed independently of
 * this code with:
 *   python3 -c "import zlib; print(hex(zlib.crc32(bytes(...bytes 0-43...))))"
 * which printed 0x990f76c6.
 */
static const uint8_t layout_bytes[STORAGE_RECORD_SIZE_BYTES] = {
    0x50, 0x4c, 0x4e, 0x54,                         /* 0-3   "PLNT" */
    0x01,                                           /* 4     version 1 */
    0x02,                                           /* 5     soil */
    0x06, 0x00,                                     /* 6-7   flags */
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, /* 8-15  sequence */
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, /* 16-23 wall clock */
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, /* 24-31 monotonic */
    0x34, 0x33, 0x32, 0x31,                         /* 32-35 value */
    0x02,                                           /* 36    I2C status */
    0x0a,                                           /* 37    stage */
    0x05, 0x04,                                     /* 38-39 errno */
    0x44, 0x43, 0x42, 0x41,                         /* 40-43 source seq */
    0xc6, 0x76, 0x0f, 0x99                          /* 44-47 CRC */
};

/* layout_record: the field values that layout_bytes encodes. */
static storage_record layout_record(void)
{
    storage_record r;

    r.record_type = STORAGE_RECORD_TYPE_SOIL;
    r.flags = STORAGE_FLAG_SIMULATED | STORAGE_FLAG_CLOCK_UNTRUSTED;
    r.sequence = UINT64_C(0x0102030405060708);
    r.wall_clock_ns = UINT64_C(0x1112131415161718);
    r.monotonic_ns = UINT64_C(0x2122232425262728);
    r.sensor_value = UINT32_C(0x31323334);
    r.i2c_status = 0x02;
    r.failure_stage = 0x0a;
    r.os_errno = 0x0405;
    r.source_sequence = UINT32_C(0x41424344);
    return r;
}

/* A record no test input decodes to, to show failures leave *out alone. */
static storage_record sentinel_record(void)
{
    storage_record r;

    r.record_type = 0xEE;
    r.flags = 0xBEEF;
    r.sequence = UINT64_C(0xDEADBEEFDEADBEEF);
    r.wall_clock_ns = UINT64_C(0xDEADBEEFDEADBEEF);
    r.monotonic_ns = UINT64_C(0xDEADBEEFDEADBEEF);
    r.sensor_value = UINT32_C(0xDEADBEEF);
    r.i2c_status = 0xEE;
    r.failure_stage = 0xEE;
    r.os_errno = 0xBEEF;
    r.source_sequence = UINT32_C(0xDEADBEEF);
    return r;
}

/* records_equal: true if every field of a and b matches. */
static int records_equal(const storage_record *a, const storage_record *b)
{
    return a->record_type == b->record_type && a->flags == b->flags &&
           a->sequence == b->sequence &&
           a->wall_clock_ns == b->wall_clock_ns &&
           a->monotonic_ns == b->monotonic_ns &&
           a->sensor_value == b->sensor_value &&
           a->i2c_status == b->i2c_status &&
           a->failure_stage == b->failure_stage &&
           a->os_errno == b->os_errno &&
           a->source_sequence == b->source_sequence;
}

/* is_sentinel: true if r still holds every sentinel field. */
static int is_sentinel(const storage_record *r)
{
    storage_record s = sentinel_record();
    return records_equal(r, &s);
}

/* reseal_crc: rewrite bytes 44-47 with the CRC of bytes 0-43. */
static void reseal_crc(uint8_t buf[STORAGE_RECORD_SIZE_BYTES])
{
    uint32_t crc = storage_crc32(buf, STORAGE_RECORD_CRC_OFFSET);

    buf[44] = (uint8_t)(crc & 0xffu);
    buf[45] = (uint8_t)((crc >> 8) & 0xffu);
    buf[46] = (uint8_t)((crc >> 16) & 0xffu);
    buf[47] = (uint8_t)((crc >> 24) & 0xffu);
}

/* test_crc32_check_value: the published CRC-32/IEEE check values. */
static void test_crc32_check_value(void)
{
    static const uint8_t check[] = "123456789";

    assert(storage_crc32(check, 9) == UINT32_C(0xCBF43926));
    assert(storage_crc32(check, 0) == 0);
    assert(storage_crc32(NULL, 0) == 0);
}

/* test_encode_layout: encode writes exactly the hand-written bytes. */
static void test_encode_layout(void)
{
    storage_record r = layout_record();
    uint8_t buf[STORAGE_RECORD_SIZE_BYTES];

    memset(buf, 0xAA, sizeof(buf));
    assert(storage_record_encode(&r, buf) == STORAGE_RECORD_OK);
    assert(memcmp(buf, layout_bytes, sizeof(buf)) == 0);
}

/* test_decode_layout: decode reads every field from the hand-written bytes. */
static void test_decode_layout(void)
{
    storage_record expected = layout_record();
    storage_record out = sentinel_record();

    assert(storage_record_decode(layout_bytes, &out) == STORAGE_RECORD_OK);
    assert(records_equal(&out, &expected));
}

/* round_trip: encode r, decode it back, and require identical fields. */
static void round_trip(const storage_record *r)
{
    uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
    storage_record out = sentinel_record();

    assert(storage_record_encode(r, buf) == STORAGE_RECORD_OK);
    assert(storage_record_decode(buf, &out) == STORAGE_RECORD_OK);
    assert(records_equal(&out, r));
}

/* test_round_trips: light, soil, and START records at boundary values. */
static void test_round_trips(void)
{
    storage_record light = {0};
    storage_record soil = {0};
    storage_record start = {0};

    light.record_type = STORAGE_RECORD_TYPE_LIGHT;
    light.flags = STORAGE_FLAG_VALID;
    light.sequence = UINT64_MAX;
    light.wall_clock_ns = UINT64_MAX;
    light.monotonic_ns = UINT64_MAX;
    light.sensor_value = UINT32_MAX;
    light.source_sequence = UINT32_MAX;
    round_trip(&light);

    soil.record_type = STORAGE_RECORD_TYPE_SOIL;
    soil.flags = STORAGE_FLAG_TIME_UNAVAILABLE;
    soil.sequence = 1;
    soil.i2c_status = 0xFF;
    soil.failure_stage = 0xFF;
    soil.os_errno = 0xFFFF;
    round_trip(&soil);

    start.record_type = STORAGE_RECORD_TYPE_START;
    round_trip(&start);
}

/* test_bad_magic: any corrupt magic byte is reported as BAD_MAGIC. */
static void test_bad_magic(void)
{
    size_t i;

    for (i = 0; i < 4; i++) {
        uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
        storage_record out = sentinel_record();

        memcpy(buf, layout_bytes, sizeof(buf));
        buf[i] ^= 0x20u;
        assert(storage_record_decode(buf, &out) ==
               STORAGE_RECORD_BAD_MAGIC);
        assert(is_sentinel(&out));

        /* Still BAD_MAGIC when the CRC matches the corrupt bytes. */
        reseal_crc(buf);
        assert(storage_record_decode(buf, &out) ==
               STORAGE_RECORD_BAD_MAGIC);
        assert(is_sentinel(&out));
    }
}

/* test_bad_version: any version other than 1 is reported as BAD_VERSION. */
static void test_bad_version(void)
{
    static const uint8_t bad_versions[] = {0, 2, 255};
    size_t i;

    for (i = 0; i < sizeof(bad_versions); i++) {
        uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
        storage_record out = sentinel_record();

        memcpy(buf, layout_bytes, sizeof(buf));
        buf[4] = bad_versions[i];
        reseal_crc(buf);
        assert(storage_record_decode(buf, &out) ==
               STORAGE_RECORD_BAD_VERSION);
        assert(is_sentinel(&out));
    }
}

/* test_bad_crc: a single flipped bit in bytes 5-47 is reported as BAD_CRC. */
static void test_bad_crc(void)
{
    size_t byte;
    unsigned bit;

    for (byte = 5; byte < STORAGE_RECORD_SIZE_BYTES; byte++) {
        for (bit = 0; bit < 8; bit++) {
            uint8_t buf[STORAGE_RECORD_SIZE_BYTES];
            storage_record out = sentinel_record();

            memcpy(buf, layout_bytes, sizeof(buf));
            buf[byte] ^= (uint8_t)(1u << bit);
            assert(storage_record_decode(buf, &out) ==
                   STORAGE_RECORD_BAD_CRC);
            assert(is_sentinel(&out));
        }
    }
}

/* test_arguments_rejected: NULL pointers are refused without writing. */
static void test_arguments_rejected(void)
{
    storage_record r = layout_record();
    storage_record out = sentinel_record();
    uint8_t buf[STORAGE_RECORD_SIZE_BYTES];

    memset(buf, 0xAA, sizeof(buf));
    assert(storage_record_encode(NULL, buf) == STORAGE_RECORD_ARGUMENT);
    assert(buf[0] == 0xAA && buf[47] == 0xAA);
    assert(storage_record_encode(&r, NULL) == STORAGE_RECORD_ARGUMENT);
    assert(storage_record_decode(NULL, &out) == STORAGE_RECORD_ARGUMENT);
    assert(is_sentinel(&out));
    assert(storage_record_decode(layout_bytes, NULL) ==
           STORAGE_RECORD_ARGUMENT);
}

/* main: run every storage record test; abort on the first failure. */
int main(void)
{
    test_crc32_check_value();
    test_encode_layout();
    test_decode_layout();
    test_round_trips();
    test_bad_magic();
    test_bad_version();
    test_bad_crc();
    test_arguments_rejected();

    puts("storage record tests passed (in-memory records; "
         "no sensor or file used)");
    return 0;
}
