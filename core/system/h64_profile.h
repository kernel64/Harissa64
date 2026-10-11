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
    H64_GP_CPU_WRITTEN = 4,     // options.gfxCpuWritten
    H64_GP_IPL3_BOOT = 8,       // boot through the cartridge's real IPL3 (h64_hle_boot)
    H64_GP_OTHERMODE_INIT = 16, // options.gfxOtherModeInit
    H64_GP_NO_COPY_BACK = 32,   // options.gfxNoCopyBack
    H64_GP_S2DEX1 = 64,         // options.gfxS2dex1
    H64_GP_COPY_BACK_FULL = 128,// options.gfxCopyBackFull
    H64_GP_RDRAM_COPY = 256,    // options.gfxRdramCopy
    H64_GP_COPY_BACK_WHOLE = 512, // options.gfxCopyBackWhole
    H64_GP_FRAME_TO_RDRAM = 1024, // options.gfxFrameToRdram
    H64_GP_SMALL_TO_RDRAM = 2048  // options.gfxSmallToRdram
};

// The flags for a game code (4 characters, the region ignored); *name gets
// the game's name in the table (NULL when not listed: flags 0).
u32 h64_game_profile(const char *gameCode, const char **name);

// Sets sys->options from the loaded ROM's profile (and logs it).
void h64_apply_game_profile(H64System *sys);

// Sets sys->options from H64_GP_* flags (ini gameflags= for checks; the boot
// flag H64_GP_IPL3_BOOT is read from the table at boot only).
void h64_apply_profile_flags(H64System *sys, u32 flags);

#endif
