// Harissa64 V2 - RetroAchievements glue (see h64_ra.h).
#include "h64_ra.h"

#include <stdio.h>

#include "../cart/h64_rom.h"
#include "../system/h64_system.h"

extern "C" {
#include "../../third_party/rcheevos/src/util/md5.h"
}

void h64_ra_rom_hash(const H64Rom *rom, char hash[33])
{
    md5_state_t md5;
    md5_byte_t digest[16];
    int i;
    md5_init(&md5);
    md5_append(&md5, (const md5_byte_t *)rom->data, (int)rom->size);
    md5_finish(&md5, digest);
    for (i = 0; i < 16; i++) sprintf(hash + 2 * i, "%02x", digest[i]);
    hash[32] = 0;
}

u32 h64_ra_read(const H64System *sys, u32 address, u8 *buffer, u32 count)
{
    u32 i;
    if (address >= H64_RDRAM_SIZE || count > H64_RDRAM_SIZE - address) return 0;
    for (i = 0; i < count; i++) buffer[i] = sys->rdram[(address + i) ^ 3u];
    return count;
}
