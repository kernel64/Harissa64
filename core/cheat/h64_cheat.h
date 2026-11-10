// Harissa64 V2 - cheats (GameShark codes).
//
// A list holds the cheats of one game: from mupen64plus's database
// (mupencheat.txt: "crc CRC1-CRC2-C:CC" starts a game, " cn " a cheat, "  cd "
// its note, then one code a line, "AAAAAAAA VVVV"; a value of "????" comes
// with a list of choices, VVVV:"name",...) and from the user's own file (the
// same lines without the crc line; any other line is a cheat's name).
//
// Codes (the first byte of the address; after mupen64plus's cheat.c):
//   80 / 81   write 8 / 16 bits, every frame          A0 / A1  the same
//   88 / 89   the same while the GS button is held
//   D0 / D1   run the next code if 8 / 16 bits equal  D2 / D3  if they differ
//   50NNSS IIII  repeat the next code NN times, the address stepping by SS,
//                the value by IIII
//   F0 / F1   write 8 / 16 bits once, when the cheat is turned on
// Other codes are ignored. Turning a cheat off puts back the values its
// plain writes replaced (as mupen64plus does).
//
// h64_cheat_frame runs the active cheats once a frame (the VI event); with no
// active cheat it does nothing.
#ifndef H64_CHEAT_H
#define H64_CHEAT_H

#include <stddef.h>

#include "../common/h64_types.h"

struct H64System;
struct H64CheatList;

struct H64CheatInfo
{
    const char *name;      // "Infinite\Lives" as the database writes it: '\' separates a group from the cheat
    const char *note;      // "" when none
    u32 id;                // CRC-32 of the name: what the settings file keeps
    int options;           // choices of a "????" cheat (0: plain on/off)
    int state;             // 0 off, 1 on (plain), or 1 + the choice's index
};

// The memory the codes act on (addresses 0..0x7FFFFF, big-endian values).
struct H64CheatMem
{
    void *user;
    u32 (*read)(void *user, u32 addr, int bytes);                // bytes: 1 or 2
    void (*write)(void *user, u32 addr, int bytes, u32 value);
};

H64CheatList *h64_cheat_new(void);
void h64_cheat_free(H64CheatList *list);
// Adds the cheats of the game crc1-crc2-country from a database text. Returns how many.
int h64_cheat_load_db(H64CheatList *list, const char *text, size_t len, u32 crc1, u32 crc2, u8 country);
// Adds the cheats of a user's file (no crc line). Returns how many.
int h64_cheat_load_plain(H64CheatList *list, const char *text, size_t len);
int h64_cheat_count(const H64CheatList *list);
int h64_cheat_active(const H64CheatList *list);   // cheats that are on
void h64_cheat_info(const H64CheatList *list, int index, H64CheatInfo *out);
const char *h64_cheat_option_name(const H64CheatList *list, int index, int option);
// 0 off; 1..options (or 1 for a plain cheat) on. Out-of-range states turn it off.
void h64_cheat_set(H64CheatList *list, int index, int state);
// The states as "id:state,id:state" (hexadecimal ids), and back. Unknown ids are ignored.
void h64_cheat_states_write(const H64CheatList *list, char *out, size_t size);
void h64_cheat_states_read(H64CheatList *list, const char *text);
// One frame: runs the cheats that are on, undoes those just turned off.
void h64_cheat_run(H64CheatList *list, const H64CheatMem *mem, int gsButton);
// The same on a machine's RDRAM (sys->cheats; recompiled code over a patched
// address is dropped).
void h64_cheat_frame(H64System *sys);

#endif
