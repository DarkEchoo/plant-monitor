#ifndef STORAGE_CRC32_H
#define STORAGE_CRC32_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32/IEEE: reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF. Check value for ASCII "123456789" is 0xCBF43926.
 * buf may be NULL only when len_bytes is 0.
 */
uint32_t storage_crc32(const uint8_t *buf, size_t len_bytes);

#endif
