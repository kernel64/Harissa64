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
    // Never sets the texture conversion either (F3DLX 1.23): its legal
    // screens' 32-bit letters were read as YUV, almost invisible.
    { "N22", "Ready 2 Rumble Boxing: Round 2", H64_GP_OTHERMODE_INIT },
    // Its map and menus are S2DEX2 2D objects: on the LLE RSP ~100 ms a frame.
    { "NSQ", "StarCraft 64", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    // Its menus read the frame back in small pieces (~500 copy backs a
    // frame, 1-12 VI/s in Xenia): one whole copy instead, 60 VI/s.
    { "NBO", "Bottom of the 9th", H64_GP_COPY_BACK_WHOLE },
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
    // Its intro fades compose the frame with the CPU in another image.
    { "ND4", "Castlevania: Legacy of Darkness", H64_GP_FRAME_TO_RDRAM },
    // Draws Link into a 64-pixel image at the start of its 320-pixel frame on
    // the pause screens and copies his pixels with the CPU (noise without;
    // one slot for both widths showed the pause screens small in a corner).
    { "CZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
    { "NZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
    // ROM hacks keep the game code. Some made on the old PC emulators DMA
    // from odd ROM addresses and expect the data from that byte (SM64 Last
    // Impact's MIO0 blocks: a black screen after the title). SM64 itself
    // never does.
    { "NSM", "Super Mario 64", H64_GP_PI_ODD_CART },
    // Fonts and palettes live in memory once drawn into as a colour image
    // (Vigilante 8: 360-wide image at 0x377EC0, palettes and fonts from
    // 0x397400; 2nd Offense: 320-wide at 0x3B6DC0, font at 0x3BD5C8). Loads
    // there copied the GPU's picture back over them: 2nd Offense's PRESS START
    // came out as dark stripes (GitHub issue #4). Vigilante 8's title text
    // and some picture tiles (YUV) are still missing for another reason.
    { "NV8", "Vigilante 8", H64_GP_COPY_BACK_SAME_WIDTH },
    { "NVG", "Vigilante 8: Second Offense", H64_GP_COPY_BACK_SAME_WIDTH },
    // A 10-pixel border around the picture: scissor x 10-309, the border
    // painted by fills from the edges. In 16:9 widescreen both moved towards
    // the centre and the scene stayed boxed at 4:3 (GitHub issue #2).
    { "NDO", "Donkey Kong 64", H64_GP_WS_BORDER },
    // Its intro draws 560-wide 32-bit frames at 0x234A00 and 0x31A500; the
    // game then keeps textures there. Loads of them (1 texel wide) copied the
    // stale intro frame back over them: ~400 copy backs a frame in the attract
    // race (0.5 VI/s in Xenia) and a black screen.
    { "NRR", "Roadsters Trophy", H64_GP_COPY_BACK_SAME_WIDTH },
    // Keeps textures inside the guessed extent of a 640-wide image at
    // 0x2D4000: ~600 copy backs a frame once in game (1 VI/s in Xenia, the
    // first room's door and Mega Man black); without them 60 VI/s, textured.
    { "NM6", "Mega Man 64", H64_GP_COPY_BACK_SAME_WIDTH },
    // Calls osContInit before its timers exist: a cold boot with Count near
    // 0 armed a timer there and read a null pointer (nothing on screen).
    { "NBX", "BattleTanx", H64_GP_BOOT_TIME },
    // 6106: with the HLE boot only its sound ran, never a graphics task.
    { "NCW", "Cruis'n World", H64_GP_IPL3_BOOT },
    // S2DEX2 2D objects (rectangles, sprites, texture loads) in the graphics
    // HLE with the S2DEX 1.x code; those tasks ran on the LLE RSP.
    { "NBV", "Bomberman 64: The Second Attack!", H64_GP_S2DEX1 },
    { "NDE", "Destruction Derby 64", H64_GP_S2DEX1 },
    // Never sets the texture filter and relies on the microcode's default
    // other modes at each task: from 0 its textures were read as YUV (dark,
    // black ground and trees).
    { "NSV", "Space Station Silicon Valley", H64_GP_OTHERMODE_INIT },
    // Same: grey textures in the levels (its HUD thermometer, banners).
    { "NTT", "Tonic Trouble", H64_GP_OTHERMODE_INIT },
    // Same (h64test --gameflags 10 against no profile, then the LLE RSP):
    // Gex's floors and HUD, Bust-A-Move 2's menu pictures, Clay Fighter's
    // ground, Doom 64's menu icons, Elmo's grey logos.
    { "NX2", "Gex 64: Enter the Gecko", H64_GP_OTHERMODE_INIT },
    { "NBU", "Bust-A-Move 2", H64_GP_OTHERMODE_INIT },
    { "NC2", "Clay Fighter: Sculptor's Cut", H64_GP_OTHERMODE_INIT },
    { "NDM", "Doom 64", H64_GP_OTHERMODE_INIT },
    { "NEL", "Elmo's Letter Adventure", H64_GP_OTHERMODE_INIT },
    // Stopped after its first logo with the graphics worker: a task that
    // keeps the RSP busy 200000 cycles or more (the worker's 400000) and the
    // game never starts another one (host: h64test --async 200000).
    { "NCS", "S.C.A.R.S.", H64_GP_SHORT_GFX_TASK },
    // S2DEX 1.07 for its pictures and pieces (LLE before: garbled on the
    // console's renderer), tasks from the default other modes.
    { "NMT", "Magical Tetris Challenge", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
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
    sys->options.piOddCart = (f & H64_GP_PI_ODD_CART) != 0;
    sys->options.gfxWsBorder = (f & H64_GP_WS_BORDER) != 0;
    sys->options.gfxCopyBackSameWidth = (f & H64_GP_COPY_BACK_SAME_WIDTH) != 0;
    sys->options.gfxShortTask = (f & H64_GP_SHORT_GFX_TASK) != 0;
}
