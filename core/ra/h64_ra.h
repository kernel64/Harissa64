// Harissa64 V2 - what RetroAchievements (third_party/rcheevos) needs from the
// machine: the game's hash and its memory as the achievements address it.
#ifndef H64_RA_H
#define H64_RA_H

#include "../common/h64_types.h"

struct H64System;
struct H64Rom;

// The RetroAchievements hash of an N64 game: the MD5 of the whole ROM in
// big-endian (.z64) order, as 32 lowercase hexadecimal digits.
void h64_ra_rom_hash(const H64Rom *rom, char hash[33]);

// `count` bytes of N64 memory at a RetroAchievements address (0..0x7FFFFF:
// RDRAM). The achievements were written on emulators that keep RDRAM as
// host-endian 32-bit words, so their byte addresses are ours XOR 3. Returns
// the bytes read (0 outside RDRAM).
u32 h64_ra_read(const H64System *sys, u32 address, u8 *buffer, u32 count);

#endif
