// Harissa64 - per-game settings built into the emulator (see h64_profile.h).

#include "h64_profile.h"
#include "h64_system.h"
#include "../common/h64_log.h"
#include <string.h>

struct GameProfile
{
    const char *code;   // game code without the region letter
    const char *name;
    u32 flags;
};

static const GameProfile s_profiles[] = {
    // Late display list parse: Conker's cutscenes 50 -> 60 VI/s. SM64, MK64
    // and Majora's Mask rewrite their display lists right after a task ends:
    // never on for a game without a console check.
    { "NFU", "Conker's Bad Fur Day", H64_GP_LATE_PARSE },
    // Each player's view is drawn with a scissor wider than the view: the
    // microcode's viewport rejection and guard band clipping keep them apart.
    { "NKT", "Mario Kart 64", H64_GP_VIEWPORT_CLIP },
    // Textures and palettes kept where small images were drawn earlier.
    { "NMQ", "Paper Mario", H64_GP_CPU_WRITTEN },
    // A null pointer read at boot with the HLE boot: the game stopped. Its
    // sprites and backgrounds use S2DEX 1.06 (LLE: ~11 VI/s on the console),
    // whose tasks start from the default other modes.
    { "NYS", "Yoshi's Story", H64_GP_IPL3_BOOT | H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    // Relies on the microcode's default other modes (texture conversion):
    // grey courses. Keeps its course picture in a full-size image and copies the
    // frame into narrow strips: copying those strips back garbled the HUD,
    // and no copy back at all left the course black once the camera stops.
    { "NMF", "Mario Golf", H64_GP_OTHERMODE_INIT | H64_GP_COPY_BACK_FULL },
    // Missing players and net with copy backs; right without.
    { "NM8", "Mario Tennis", H64_GP_NO_COPY_BACK },
    // Copies its background into the frame's memory through 512-pixel
    // images with copy-mode rectangles, then draws in a 328-pixel frame at
    // the same address (one slot for both showed horizontal streaks).
    { "NRE", "Resident Evil 2", H64_GP_RDRAM_COPY | H64_GP_SLOT_BY_WIDTH },
    // Reads its background back six lines at a time (~40 copy backs a frame,
    // ~47 VI/s in a match): one whole copy instead, 60 VI/s.
    { "NN6", "Dr. Mario 64", H64_GP_COPY_BACK_WHOLE },
    // The magnifying glass on the character select enlarges what is on
    // screen: it reads the frame shown with the CPU (empty without).
    { "ND3", "Castlevania", H64_GP_FRAME_TO_RDRAM },
    // Draws Link into a 64-pixel image at the start of its 320-pixel frame on
    // the pause screens and copies his pixels with the CPU (noise without;
    // one slot for both widths showed the pause screens small in a corner).
    { "CZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
    { "NZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
};

u32 h64_game_profile(const char *gameCode, const char **name)
{
    u32 i;
    if (name) *name = NULL;
    if (!gameCode) return 0;
    for (i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
        if (!memcmp(gameCode, s_profiles[i].code, 3))
        {
            if (name) *name = s_profiles[i].name;
            return s_profiles[i].flags;
        }
    return 0;
}

void h64_apply_game_profile(H64System *sys)
{
    const char *name;
    u32 f = h64_game_profile(sys->rom.gameCode, &name);
    h64_apply_profile_flags(sys, f);
    if (name) H64_INFO("[profile] %s: flags %X", name, f);
}

void h64_apply_profile_flags(H64System *sys, u32 f)
{
    sys->options.gfxLateParse = (f & H64_GP_LATE_PARSE) != 0;
    sys->options.gfxViewportClip = (f & H64_GP_VIEWPORT_CLIP) != 0;
    sys->options.gfxCpuWritten = (f & H64_GP_CPU_WRITTEN) != 0;
    sys->options.gfxOtherModeInit = (f & H64_GP_OTHERMODE_INIT) != 0;
    sys->options.gfxNoCopyBack = (f & H64_GP_NO_COPY_BACK) != 0;
    sys->options.gfxS2dex1 = (f & H64_GP_S2DEX1) != 0;
    sys->options.gfxCopyBackFull = (f & H64_GP_COPY_BACK_FULL) != 0;
    sys->options.gfxRdramCopy = (f & H64_GP_RDRAM_COPY) != 0;
    sys->options.gfxCopyBackWhole = (f & H64_GP_COPY_BACK_WHOLE) != 0;
    sys->options.gfxFrameToRdram = (f & H64_GP_FRAME_TO_RDRAM) != 0;
    sys->options.gfxSmallToRdram = (f & H64_GP_SMALL_TO_RDRAM) != 0;
    sys->options.gfxSlotByWidth = (f & H64_GP_SLOT_BY_WIDTH) != 0;
}
