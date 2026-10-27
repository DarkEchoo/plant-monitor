#include "storage/crc32.h"

#define CRC32_POLY_REFLECTED UINT32_C(0xEDB88320)

/* storage_crc32: bitwise CRC-32/IEEE of len_bytes bytes at buf. */
uint32_t storage_crc32(const uint8_t *buf, size_t len_bytes)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t i;
    int bit;

    for (i = 0; i < len_bytes; i++) {
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u)
                crc = (crc >> 1) ^ CRC32_POLY_REFLECTED;
            else
                crc >>= 1;
        }
    }
    return crc ^ UINT32_C(0xFFFFFFFF);
}
