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
    // Also: when Yoshi moves, the scenery is redrawn every frame into three
    // 336-wide 8-bit images (16x16 texture tiles) then loaded ~200 times as
    // textures: drawn by the GPU, each load copied a piece back (~120 copy
    // backs a frame, 12-23 VI/s). The software RDP draws them in memory
    // instead (~38000 pixels a frame), nothing to copy back.
    { "NYS", "Yoshi's Story", H64_GP_IPL3_BOOT | H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT | H64_GP_SOFT_8BIT },
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
    // Loads its textures from memory where the intro's 640x480 frames were:
    // copying those old frames back over them drew the snow canyons black.
    { "NRS", "Star Wars: Rogue Squadron", H64_GP_COPY_BACK_SAME_WIDTH },
    // Never sets the texture conversion (F3DEX2 2.08): its textures were
    // read as YUV, a grey picture.
    { "NXO", "Cruis'n Exotica", H64_GP_OTHERMODE_INIT },
    // Its rink and crowd are 32-texel textures repeated over 512x512 masks:
    // ~35 textures of 1 MB a frame, more than the 24 MB texture arena holds,
    // so every frame decoded them again (~750 ms a frame in Xenia, 1 VI/s).
    { "NHO", "NHL Blades of Steel '99", H64_GP_BIG_TEX_POOL },
    // Copies its background into the frame's memory through 512-pixel
    // images with copy-mode rectangles, then draws in a 328-pixel frame at
    // the same address (one slot for both showed horizontal streaks).
    { "NRE", "Resident Evil 2", H64_GP_RDRAM_COPY | H64_GP_SLOT_BY_WIDTH },
    // Reads its background back six lines at a time (~40 copy backs a frame,
    // ~47 VI/s in a match): one whole copy instead, 60 VI/s.
    { "NN6", "Dr. Mario 64", H64_GP_COPY_BACK_WHOLE },
    // The magnifying glass on the character select enlarges what is on
    // screen: it reads the frame shown with the CPU (empty without).
    // Both also need each task to start from the microcode's default other
    // modes: a 2D title card sets the depth source to the primitive's, the
    // real microcode drops it at the next task and the HLE kept it, so the
    // whole 3D was drawn at one depth (0x7FFF), a mesh of triangles through
    // the walls on the console until a state load reset the modes.
    { "ND3", "Castlevania", H64_GP_FRAME_TO_RDRAM | H64_GP_OTHERMODE_INIT },
    // Its intro fades compose the frame with the CPU in another image.
    { "ND4", "Castlevania: Legacy of Darkness", H64_GP_FRAME_TO_RDRAM | H64_GP_OTHERMODE_INIT },
    // Draws Link into a 64-pixel image at the start of its 320-pixel frame on
    // the pause screens and copies his pixels with the CPU (noise without;
    // one slot for both widths showed the pause screens small in a corner).
    { "CZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
    { "NZL", "The Legend of Zelda: Ocarina of Time", H64_GP_SMALL_TO_RDRAM | H64_GP_SLOT_BY_WIDTH },
    // ROM hacks keep the game code. Some made on the old PC emulators DMA
    // from odd ROM addresses and expect the data from that byte (SM64 Last
    // Impact's MIO0 blocks: a black screen after the title). SM64 itself
    // never does. Some also run into enabled FPU exceptions the old
    // emulators never raised (Sunshine Secret Book 64: 0 times an infinite
    // distance at its star doors, the game stopped); SM64 itself never traps.
    // Super Mario 64 Land's own code leaves the stack only 4-byte aligned,
    // then saves FPU registers with SDC1 (an address error on the console;
    // the PC emulators it was made on write the two words): the game thread
    // stopped near 1-3's rail platforms (GitHub issue #8). Super Mario
    // Odyssey 64 adds two floats' bit patterns with ADD (0x802ACF8C) and
    // overflows: the game thread stopped in Cap Kingdom (GitHub issue #10).
    { "NSM", "Super Mario 64", H64_GP_PI_ODD_CART | H64_GP_FPU_NO_TRAPS | H64_GP_LAX_ALIGN64 | H64_GP_NO_OV_TRAP },
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
    // Swaps the VI to a buffer before the deferred renderer has drawn its
    // new frame there: every other VI showed that buffer's frame from two
    // frames before (shown order 600, 598, 602, 600...), the picture
    // jerking back while moving. Its camera reads the depth image with the
    // CPU (pc 80700BB8, around Kong) to tell whether he is hidden: the GPU
    // keeps the depth, RDRAM held stale values and the camera moved in and
    // out in a loop, even with Kong standing still; its depth clears now
    // fill RDRAM too.
    { "NDO", "Donkey Kong 64", H64_GP_WS_BORDER | H64_GP_NO_OLD_FRAME | H64_GP_DEPTH_FILL_RDRAM },
    // Its intro draws 560-wide 32-bit frames at 0x234A00 and 0x31A500; the
    // game then keeps textures there. Loads of them (1 texel wide) copied the
    // stale intro frame back over them: ~400 copy backs a frame in the attract
    // race (0.5 VI/s in Xenia) and a black screen.
    { "NRR", "Roadsters Trophy", H64_GP_COPY_BACK_SAME_WIDTH },
    // Keeps textures inside the guessed extent of a 640-wide image at
    // 0x2D4000: ~600 copy backs a frame once in game (1 VI/s in Xenia, the
    // first room's door and Mega Man black); without them 60 VI/s, textured.
    { "NM6", "Mega Man 64", H64_GP_COPY_BACK_SAME_WIDTH },
    // Smash Remix (a hack with the same code) loads character textures from
    // memory its intro screens drew into: copied back, the old frames
    // replaced them (Sonic dark with black stripes, his stock icons blue
    // bars) until a state load dropped the frames. The original game made
    // no copy back from its character select to a match.
    { "NAL", "Super Smash Bros.", H64_GP_COPY_BACK_SAME_WIDTH },
    // Calls osContInit before its timers exist: a cold boot with Count near
    // 0 armed a timer there and read a null pointer (nothing on screen).
    { "NBX", "BattleTanx", H64_GP_BOOT_TIME },
    // Same race: with Count near 0 its osContInit waits 0.5 s for the
    // controllers while another thread writes the EEPROM through libultra's
    // timer queue, which osContInit creates last: a null thread queue, the
    // game stopped after 16 frames (black screen).
    // Its 400-wide frames are 448 lines tall: the GPU renderer takes the
    // height from the scissor (only the top part showed, zoomed in).
    { "NIJ", "Indiana Jones and the Infernal Machine", H64_GP_BOOT_TIME | H64_GP_TALL_FRAME },
    // 6106: with the HLE boot only its sound ran, never a graphics task.
    { "NCW", "Cruis'n World", H64_GP_IPL3_BOOT },
    // With the HLE boot, whole parts of rooms were missing depending on the
    // camera (Banjo's house: rug, floor, bed and armchair gone towards the
    // fireplace; the LLE microcode drew the same, so the game chose them):
    // it relies on something the real IPL3 leaves behind. V1 (mupen64plus)
    // and the real IPL3 boot draw the whole room.
    { "NBK", "Banjo-Kazooie", H64_GP_IPL3_BOOT },
    // S2DEX2 2D objects (rectangles, sprites, texture loads) in the graphics
    // HLE with the S2DEX 1.x code; those tasks ran on the LLE RSP.
    { "NBV", "Bomberman 64: The Second Attack!", H64_GP_S2DEX1 },
    // Its dialogue boxes (G_OBJ_MOVEMEM) sent the tasks to the LLE RSP; the
    // same images with the HLE (h64test, 4000 frames from boot).
    { "NOB", "Ogre Battle 64: Person of Lordly Caliber", H64_GP_S2DEX1 },
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
    { "NSP", "Superman", H64_GP_OTHERMODE_INIT },   // its title pictures came out grey
    // Sets the primitive depth source for a few draws and leaves it set at
    // the end of a task: with no reset, the next frames drew everything at
    // depth 0 and the first surface drawn won (Jim hidden on the Xbox, HUD
    // missing with the software RDP). GitHub issue #7.
    { "NJM", "Earthworm Jim 3D", H64_GP_OTHERMODE_INIT },
    // Relies on the default perspective-corrected textures: without them the
    // balls' round outline came out as squares.
    { "NVR", "Virtual Pool 64", H64_GP_OTHERMODE_INIT },
    // Copies the frame on screen with the CPU into another image the VI
    // shows while a level loads: that frame is only on the GPU (noise).
    { "NFF", "Fighting Force 64", H64_GP_KEEP_SHOWN },
    // Its animated 24x24 32-bit textures are declared with 256x256 masks: each
    // was decoded as 65536 texels and again every frame (30-35 VI/s in races).
    { "NML", "Mickey's Speedway USA", H64_GP_MASK_TO_TILE },
    // Its backgrounds and sprites are filtered rectangles drawn 1:1 with an
    // alpha test, over magenta transparent texels: the N64 samples whole
    // texels there, a higher resolution filtered magenta into their edges.
    // Started from the ROM list, its game background lies where its menu
    // frames were drawn: ~400 copy backs a second wrote those old frames
    // over it (stone menu texture in the scene, 14 VI/s in Xenia, sound
    // cuts); after a state load (slots dropped) it was right. It never
    // samples its frames: no copy backs, 60 VI/s.
    { "NRI", "The New Tetris", H64_GP_RECT_POINT | H64_GP_NO_COPY_BACK },
    // Fast3D with no name string (now drawn here, ~20 ms a frame on the LLE
    // RSP before): its tasks rely on the default other modes (texture
    // conversion), else its panels' texel alpha came out as YUV.
    { "NT6", "Tetris 64", H64_GP_OTHERMODE_INIT },
    // Alpha test against 0 with alpha = texel alpha * fog factor: the cars,
    // the road and near scenery (no fog) vanished on the Xbox. GitHub issue #7.
    { "NHW", "Hot Wheels Turbo Racing", H64_GP_ALPHA_REF0_PASS },
    // Stopped after its first logo with the graphics worker: a task that
    // keeps the RSP busy 200000 cycles or more (the worker's 400000) and the
    // game never starts another one (host: h64test --async 200000).
    { "NCS", "S.C.A.R.S.", H64_GP_SHORT_GFX_TASK },
    // Writes its display list while the RSP runs it: the list branches to
    // itself until the CPU writes more. The HLE ran into it and left every
    // such task to the LLE RSP (~12 M RSP instructions a second). The lists
    // are parsed on the CPU thread (~31 waits a frame in game) and each part
    // is drawn on the graphics worker (h64_hle.cpp stream_submit).
    { "NGX", "Gauntlet Legends", H64_GP_GFX_STREAM },
    // S2DEX 1.07 for its pictures and pieces (LLE before: garbled on the
    // console's renderer), tasks from the default other modes.
    { "NMT", "Magical Tetris Challenge", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    // S2DEX2 2D objects (G_OBJ_LOADTXTR 05 and the rest) went to the LLE RSP:
    // Kirby's menus and stage screens 7-10 VI/s in Xenia (53-61 with this),
    // Command & Conquer's briefings 6-7 (35-60). V-Rally 99 (G_OBJ_MOVEMEM
    // DC) also keeps textures inside a 640-wide image's guessed extent:
    // ~40 copy backs a frame once its 2D objects are drawn here.
    { "NK4", "Kirby 64: The Crystal Shards", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    { "NCC", "Command & Conquer", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    { "NVL", "V-Rally Edition 99", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT | H64_GP_COPY_BACK_SAME_WIDTH },
    // S2DEX 1.07 (Puzzle League, Star Soldier) and 1.03 (GT 64) went to the
    // LLE RSP. Star Soldier also keeps textures inside a wide image's guessed
    // extent: ~600 copy backs a frame in its stages (dark garbled floor,
    // 12-20 VI/s in Xenia); without them right and 60 VI/s.
    { "NPN", "Pokemon Puzzle League", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
    { "NS6", "Star Soldier: Vanishing Earth", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT | H64_GP_COPY_BACK_SAME_WIDTH },
    { "NGC", "GT 64: Championship Edition", H64_GP_S2DEX1 | H64_GP_OTHERMODE_INIT },
};

static void apply_idle_yield(H64System *sys);

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
    apply_idle_yield(sys);
    if (name) H64_INFO("[profile] %s: flags %X", name, f);
}

// Idle threads that call osYieldThread in a loop instead of spinning: when
// no other thread is ready, the recompiler lets the time pass up to the next
// event there (h64_jit.cpp). The loop's head and where __osRunQueue lives.
struct IdleYield { const char *code; u32 pc, runQueue; };
static const IdleYield s_idleYield[] = {
    // ~190 instructions a turn, 2700 turns a frame in its logos and menus
    // (COP0 moves, an ERET, MI_INTR_MASK writes): 34 VI/s on the console.
    { "NIJ", 0x40036DA0u, 0x000479F8u },
};

// Loops that only read Count until enough time has passed (frame limiters)
// or poll a message queue: the recompiler moves time on there (h64_jit.cpp).
// The loop's head, the most CPU cycles skipped at a time (0: up to the next
// event), the physical address of the queue (skipped only while empty).
struct CountWait { const char *code; u32 pc, step, queue; };
static const CountWait s_countWait[] = {
    // while (osGetCount() - start < limit) {}: half of the CPU time in
    // game, 42-44 VI/s on the console.
    { "NFF", 0x80022948u, 4096, 0 },
    // Its intro and menus poll a queue with osRecvMesg(NOBLOCK) (interrupts
    // off and on each turn): 23 ms of CPU a frame, 42 VI/s on the console.
    { "NVR", 0x800062ACu, 0, 0x00271C78u },
};

static void apply_idle_yield(H64System *sys)
{
    u32 i;
    sys->options.idleYieldPc = sys->options.idleRunQueue = 0;
    sys->options.countWaitPc = sys->options.countWaitStep = sys->options.countWaitQueue = 0;
    for (i = 0; i < sizeof(s_countWait) / sizeof(s_countWait[0]); i++)
        if (!memcmp(sys->rom.gameCode, s_countWait[i].code, 3))
        {
            sys->options.countWaitPc = s_countWait[i].pc;
            sys->options.countWaitStep = s_countWait[i].step;
            sys->options.countWaitQueue = s_countWait[i].queue;
        }
    for (i = 0; i < sizeof(s_idleYield) / sizeof(s_idleYield[0]); i++)
        if (!memcmp(sys->rom.gameCode, s_idleYield[i].code, 3))
        {
            sys->options.idleYieldPc = s_idleYield[i].pc;
            sys->options.idleRunQueue = s_idleYield[i].runQueue;
        }
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
    sys->options.gfxBigTexPool = (f & H64_GP_BIG_TEX_POOL) != 0;
    sys->options.gfxStream = (f & H64_GP_GFX_STREAM) != 0;
    sys->options.gfxNoOldFrame = (f & H64_GP_NO_OLD_FRAME) != 0;
    sys->options.gfxSoft8bit = (f & H64_GP_SOFT_8BIT) != 0;
    sys->options.gfxDepthFillToRdram = (f & H64_GP_DEPTH_FILL_RDRAM) != 0;
    sys->options.gfxAlphaRef0Pass = (f & H64_GP_ALPHA_REF0_PASS) != 0;
    sys->options.gfxTallFrame = (f & H64_GP_TALL_FRAME) != 0;
    sys->options.gfxRectPoint = (f & H64_GP_RECT_POINT) != 0;
    sys->options.gfxKeepShown = (f & H64_GP_KEEP_SHOWN) != 0;
    sys->options.gfxMaskToTile = (f & H64_GP_MASK_TO_TILE) != 0;
    if (f & H64_GP_FPU_NO_TRAPS) sys->cpu.fpuNoTraps = 1;
    sys->cpu.laxAlign64 = (f & H64_GP_LAX_ALIGN64) != 0;
    sys->cpu.noOvTrap = (f & H64_GP_NO_OV_TRAP) != 0;
    if (!sys->options.strictCpu)
    {
        // ROM hacks are made on PC emulators (Project64, mupen64plus) that
        // let through what stops the console: a ROM DMA from an odd address,
        // FPU exceptions, doubleword accesses 4 bytes off, integer overflows.
        // Games made for the console never do these (they would stop there),
        // so every game gets that tolerance; strictCpu keeps the console's way.
        sys->options.piOddCart = 1;
        sys->cpu.fpuNoTraps = 1;
        sys->cpu.laxAlign64 = 1;
        sys->cpu.noOvTrap = 1;
    }
}
