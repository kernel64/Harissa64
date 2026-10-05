#include "h64_crc32.h"

// Bitwise reference implementation (reflected polynomial 0xEDB88320). Only
// used on small buffers at ROM load, so no table.
u32 h64_crc32(u32 crc, const u8 *data, size_t size)
{
    size_t i;
    int bit;
    crc = ~crc;
    for (i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
