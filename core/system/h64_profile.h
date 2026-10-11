// Harissa64 - per-game settings built into the emulator.
//
// A fix that changes how the emulator behaves (not a plain bug fix) is turned
// on only for the games it was made and checked for, so that it cannot change
// any other game. The table is keyed by the cartridge's game code without the
// region letter ("NKT" for every Mario Kart 64).

#ifndef H64_PROFILE_H
#define H64_PROFILE_H

#include "../common/h64_types.h"

struct H64System;

enum
{
    H64_GP_LATE_PARSE = 1,      // options.gfxLateParse
    H64_GP_VIEWPORT_CLIP = 2,   // options.gfxViewportClip
    H64_GP_CPU_WRITTEN = 4      // options.gfxCpuWritten
};

// The flags for a game code (4 characters, the region ignored); *name gets
// the game's name in the table (NULL when not listed: flags 0).
u32 h64_game_profile(const char *gameCode, const char **name);

// Sets sys->options from the loaded ROM's profile (and logs it).
void h64_apply_game_profile(H64System *sys);

#endif
