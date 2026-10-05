// Harissa64 V2 - CRC-32 (IEEE 802.3, the zlib polynomial), used to identify
// a ROM's IPL3 boot code and hence its CIC chip.
#ifndef H64_CRC32_H
#define H64_CRC32_H

#include "h64_types.h"

u32 h64_crc32(u32 crc, const u8 *data, size_t size);

#endif
