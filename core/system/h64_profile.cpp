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
    sys->options.gfxLateParse = (f & H64_GP_LATE_PARSE) != 0;
    sys->options.gfxViewportClip = (f & H64_GP_VIEWPORT_CLIP) != 0;
    sys->options.gfxCpuWritten = (f & H64_GP_CPU_WRITTEN) != 0;
    if (name) H64_INFO("[profile] %s: flags %X", name, f);
}
