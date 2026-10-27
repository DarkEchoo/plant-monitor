#include "storage/record.h"

#include "storage/crc32.h"

#include <stddef.h>

/* put_le: write the low n_bytes of value at p, least significant first. */
static void put_le(uint8_t *p, uint64_t value, size_t n_bytes)
{
    size_t i;

    for (i = 0; i < n_bytes; i++)
        p[i] = (uint8_t)((value >> (8 * i)) & 0xffu);
}

/* get_le: read n_bytes at p, least significant first. */
static uint64_t get_le(const uint8_t *p, size_t n_bytes)
{
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < n_bytes; i++)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

/* storage_record_encode: lay out *in per docs/design.md and seal the CRC. */
storage_record_result storage_record_encode(
    const storage_record *in, uint8_t out[STORAGE_RECORD_SIZE_BYTES])
{
    if (in == NULL || out == NULL)
        return STORAGE_RECORD_ARGUMENT;

    put_le(out + 0, STORAGE_RECORD_MAGIC, 4);
    out[4] = STORAGE_RECORD_VERSION;
    out[5] = in->record_type;
    put_le(out + 6, in->flags, 2);
    put_le(out + 8, in->sequence, 8);
    put_le(out + 16, in->wall_clock_ns, 8);
    put_le(out + 24, in->monotonic_ns, 8);
    put_le(out + 32, in->sensor_value, 4);
    out[36] = in->i2c_status;
    out[37] = in->failure_stage;
    put_le(out + 38, in->os_errno, 2);
    put_le(out + 40, in->source_sequence, 4);
    put_le(out + STORAGE_RECORD_CRC_OFFSET,
           storage_crc32(out, STORAGE_RECORD_CRC_OFFSET), 4);
    return STORAGE_RECORD_OK;
}

/* storage_record_decode: check magic, version, CRC, then fill *out. */
storage_record_result storage_record_decode(
    const uint8_t in[STORAGE_RECORD_SIZE_BYTES], storage_record *out)
{
    if (in == NULL || out == NULL)
        return STORAGE_RECORD_ARGUMENT;
    if (get_le(in + 0, 4) != STORAGE_RECORD_MAGIC)
        return STORAGE_RECORD_BAD_MAGIC;
    if (in[4] != STORAGE_RECORD_VERSION)
        return STORAGE_RECORD_BAD_VERSION;
    if (get_le(in + STORAGE_RECORD_CRC_OFFSET, 4) !=
        storage_crc32(in, STORAGE_RECORD_CRC_OFFSET))
        return STORAGE_RECORD_BAD_CRC;

    out->record_type = in[5];
    out->flags = (uint16_t)get_le(in + 6, 2);
    out->sequence = get_le(in + 8, 8);
    out->wall_clock_ns = get_le(in + 16, 8);
    out->monotonic_ns = get_le(in + 24, 8);
    out->sensor_value = (uint32_t)get_le(in + 32, 4);
    out->i2c_status = in[36];
    out->failure_stage = in[37];
    out->os_errno = (uint16_t)get_le(in + 38, 2);
    out->source_sequence = (uint32_t)get_le(in + 40, 4);
    return STORAGE_RECORD_OK;
}
