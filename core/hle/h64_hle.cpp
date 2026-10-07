/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - hle.c                                           *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// Harissa64 V2: task detection from mupen64plus-rsp-hle's hle.c (audio
// tasks only; everything else goes to the LLE RSP), plus the memory helpers
// of its memory.c rewritten for guest memory in N64 byte order.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "h64_hle_internal.h"
#include "h64_hle.h"
#include "h64_gfx.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define vsnprintf _vsnprintf
#endif


// ---- messages ----
void HleWarnMessage(void *UNUSED(user_defined), const char *message, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, message);
    vsnprintf(line, sizeof(line), message, ap);
    va_end(ap);
    line[sizeof(line) - 1] = 0;
    H64_WARN("[hle] %s", line);
}

void HleVerboseMessage(void *UNUSED(user_defined), const char *UNUSED(message), ...)
{
}

// ---- memory (memory.c) ----
void load_u8(uint8_t *dst, const unsigned char *buffer, unsigned mask, unsigned address, size_t count)
{
    while (count != 0) { *(dst++) = buffer[address & mask]; address += 1; --count; }
}

void load_u16(uint16_t *dst, const unsigned char *buffer, unsigned mask, unsigned address, size_t count)
{
    while (count != 0) { *(dst++) = h64_load_be16(buffer + (address & mask & ~1u)); address += 2; --count; }
}

void load_u32(uint32_t *dst, const unsigned char *buffer, unsigned mask, unsigned address, size_t count)
{
    while (count != 0) { *(dst++) = h64_load_be32(buffer + (address & mask & ~3u)); address += 4; --count; }
}

void store_u8(unsigned char *buffer, unsigned mask, unsigned address, const uint8_t *src, size_t count)
{
    while (count != 0) { buffer[address & mask] = *(src++); address += 1; --count; }
}

void store_u16(unsigned char *buffer, unsigned mask, unsigned address, const uint16_t *src, size_t count)
{
    while (count != 0) { h64_store_be16(buffer + (address & mask & ~1u), *(src++)); address += 2; --count; }
}

void store_u32(unsigned char *buffer, unsigned mask, unsigned address, const uint32_t *src, size_t count)
{
    while (count != 0) { h64_store_be32(buffer + (address & mask & ~3u), *(src++)); address += 4; --count; }
}

void hle_dram_dirty(struct hle_t *hle, u32 address, size_t count)
{
    u32 end = address + (u32)count;
    if (end > H64_RDRAM_SIZE) { address = 0; end = H64_RDRAM_SIZE; }   // wrapped: be safe
    if (hle->checkMask && hle->dram == hle->checkRam) memset(hle->checkMask + address, 1, end - address);
    if (hle->dirtyLo >= hle->dirtyHi) { hle->dirtyLo = address; hle->dirtyHi = end; return; }
    if (address < hle->dirtyLo) hle->dirtyLo = address;
    if (end > hle->dirtyHi) hle->dirtyHi = end;
}

// HLE buffers are 0x1000 bytes in upstream's native-word layout: the guest
// byte at offset o of the buffer lives at (o ^ HLE_S8).
void hle_dram_to_buffer(struct hle_t *hle, uint8_t *buffer, unsigned offset, uint32_t address, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        buffer[((offset + i) ^ HLE_S8) & 0xfff] = hle->dram[(address + i) & HLE_DRAM_MASK];
}

void hle_buffer_to_dram(struct hle_t *hle, uint32_t address, const uint8_t *buffer, unsigned offset, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        hle->dram[(address + i) & HLE_DRAM_MASK] = buffer[((offset + i) ^ HLE_S8) & 0xfff];
    hle_dram_dirty(hle, address & HLE_DRAM_MASK, count);
}

// ---- task detection (hle.c) ----
void rsp_break(struct hle_t *hle, unsigned int setbits)
{
    hle->sp_status |= setbits | SP_STATUS_BROKE | SP_STATUS_HALT;
}

int HleForwardTask(void *user_defined)
{
    ((struct hle_t *)user_defined)->forwarded = 1;
    return 0;
}

static void forward_task(struct hle_t *hle)
{
    hle->forwarded = 1;
}

static int is_task(struct hle_t *hle)
{
    return (*dmem_u32(hle, TASK_UCODE_BOOT_SIZE) <= 0x1000);
}

static ucode_func_t try_audio_task_detection(struct hle_t *hle)
{
    /* identify audio ucode by using the content of ucode_data */
    uint32_t ucode_data = *dmem_u32(hle, TASK_UCODE_DATA);
    uint32_t v;

    if (*dram_u32(hle, ucode_data) == 0x00000001) {
        if (*dram_u32(hle, ucode_data + 0x30) == 0xf0000f00) {
            v = *dram_u32(hle, ucode_data + 0x28);
            switch(v)
            {
            case 0x1e24138c: /* audio ABI (most common) */
                return &alist_process_audio;
            case 0x1dc8138c: /* GoldenEye */
                return &alist_process_audio_ge;
            case 0x1e3c1390: /* BlastCorp, DiddyKongRacing */
                return &alist_process_audio_bc;
            default:
                HleWarnMessage(hle->user_defined, "ABI1 identification regression: v=%08x", v);
            }
        } else {
            v = *dram_u32(hle, ucode_data + 0x10);
            switch(v)
            {
            case 0x11181350: /* MarioKart, WaveRace (E) */
                return &alist_process_nead_mk;
            case 0x111812e0: /* StarFox (J) */
                return &alist_process_nead_sfj;
            case 0x110412ac: /* WaveRace (J RevB) */
                return &alist_process_nead_wrjb;
            case 0x110412cc: /* StarFox/LylatWars (except J) */
                return &alist_process_nead_sf;
            case 0x1cd01250: /* FZeroX */
                return &alist_process_nead_fz;
            case 0x1f08122c: /* YoshisStory */
                return &alist_process_nead_ys;
            case 0x1f38122c: /* 1080° Snowboarding */
                return &alist_process_nead_1080;
            case 0x1f681230: /* Zelda OoT / Zelda MM (J, J RevA) */
                return &alist_process_nead_oot;
            case 0x1f801250: /* Zelda MM (except J, J RevA, E Beta), PokemonStadium 2 */
                return &alist_process_nead_mm;
            case 0x109411f8: /* Zelda MM (E Beta) */
                return &alist_process_nead_mmb;
            case 0x1eac11b8: /* AnimalCrossing */
                return &alist_process_nead_ac;
            case 0x00010010: /* MusyX v2 (IndianaJones, BattleForNaboo) */
                return &musyx_v2_task;
            case 0x1f701238: /* Mario Artist Talent Studio */
                return &alist_process_nead_mats;
            case 0x1f4c1230: /* FZeroX Expansion */
                return &alist_process_nead_efz;
            default:
                HleWarnMessage(hle->user_defined, "ABI2 identification regression: v=%08x", v);
            }
        }
    } else {
        v = *dram_u32(hle, ucode_data + 0x10);
        switch(v)
        {
        case 0x00000001: /* MusyX v1
            RogueSquadron, ResidentEvil2, PolarisSnoCross,
            TheWorldIsNotEnough, RugratsInParis, NBAShowTime,
            HydroThunder, Tarzan, GauntletLegend, Rush2049 */
            return &musyx_v1_task;
        case 0x0000127c: /* naudio (many games) */
            return &alist_process_naudio;
        case 0x00001280: /* BanjoKazooie */
            return &alist_process_naudio_bk;
        case 0x1c58126c: /* DonkeyKong */
            return &alist_process_naudio_dk;
        case 0x1ae8143c: /* BanjoTooie, JetForceGemini, MickeySpeedWayUSA, PerfectDark */
            return &alist_process_naudio_mp3;
        case 0x1ab0140c: /* ConkerBadFurDay */
            return &alist_process_naudio_cbfd;

        default:
            HleWarnMessage(hle->user_defined, "ABI3 identification regression: v=%08x", v);
        }
    }

    return NULL;
}

static ucode_func_t task_detection(struct hle_t *hle)
{
    if (is_task(hle) && *dmem_u32(hle, TASK_TYPE) == 2) {
        ucode_func_t f = try_audio_task_detection(hle);
        if (f)
            return f;
    }
    return &forward_task;
}

// ---- V2 entry points ----
struct hle_t *h64_hle_create(H64System *sys)
{
    struct hle_t *hle = new hle_t;
    memset(hle, 0, sizeof(*hle));
    hle->sys = sys;
    hle->user_defined = hle;
    hle->dram = sys->rdram;
    hle->dmem = sys->spMem;
    hle->imem = sys->spMem + 0x1000;
    return hle;
}

void h64_hle_free(struct hle_t *hle)
{
    if (hle->checkRam)
    {
        H64_INFO("[hle] audio check: %u tasks compared, %u with differences; differing halfwords by size: "
                 "1: %u, 2-16: %u, 17-256: %u, 257-4096: %u, more: %u", hle->checkTasks, hle->checkBad,
                 hle->checkHist[0], hle->checkHist[1], hle->checkHist[2], hle->checkHist[3], hle->checkHist[4]);
        free(hle->checkRam);
        free(hle->checkMask);
    }
    if (hle->gfx) h64_gfx_free(hle->gfx);
    delete hle;
}

void h64_hle_check_end(H64System *sys)
{
    struct hle_t *hle = sys->hle;
    u32 a, runs = 0, bad = 0;
    char line[512];
    size_t len = 0;
    if (!hle || !hle->checkPending)
        return;
    hle->checkPending = 0;
    hle->checkTasks++;
    line[0] = 0;
    for (a = hle->checkLo & ~1u; a + 1 < hle->checkHi; a += 2)
    {
        int d;
        if (!(hle->checkMask[a] | hle->checkMask[a + 1])) continue;
        d = (int)(s16)h64_load_be16(sys->rdram + a) - (int)(s16)h64_load_be16(hle->checkRam + a);
        if (d < 0) d = -d;
        if (d) hle->checkHist[d == 1 ? 0 : d <= 16 ? 1 : d <= 256 ? 2 : d <= 4096 ? 3 : 4]++;
    }
    for (a = hle->checkLo; a < hle->checkHi; a++)
    {
        if (!hle->checkMask[a] || sys->rdram[a] == hle->checkRam[a])
            continue;
        u32 start = a;
        while (a < hle->checkHi && hle->checkMask[a] && sys->rdram[a] != hle->checkRam[a]) a++;
        bad += a - start;
        if (runs++ < 8 && len < sizeof(line) - 64)
        {
            u32 h = start & ~1u;
            len += sprintf(line + len, " %06X+%X(lle %04X hle %04X)", start, a - start, h64_load_be16(sys->rdram + h),
                           h64_load_be16(hle->checkRam + h));
        }
    }
    if (bad)
    {
        hle->checkBad++;
        if (hle->checkBad <= 40)
            H64_INFO("[hle] audio check: task %u (frame %u) differs in %u bytes, %u runs:%s", hle->checkTasks,
                     sys->vi.frames, bad, runs, line);
    }
}

// Worker job 1: the display list (reads RDRAM, snapshots what texture loads read).
static void gfx_parse_job(void *arg)
{
    H64System *sys = (H64System *)arg;
    struct hle_t *hle = sys->hle;
    int fullSync = 0, mustSync = 0, ran;
    u64 t0 = h64_prof_now(sys);
    ran = h64_gfx_parse_task(sys, hle->gfx, &fullSync, &mustSync);
    sys->prof[H64_PROF_GFX_HLE] += h64_prof_now(sys) - t0;
    hle->gfxAsyncRan = ran;
    hle->gfxAsyncFullSync = fullSync;
    hle->gfxAsyncMustSync = mustSync;
}

// Worker job 2: the rendering, which may still run after the task ended.
static void gfx_render_job(void *arg)
{
    H64System *sys = (H64System *)arg;
    struct hle_t *hle = sys->hle;
    u64 t0;
    if (!hle->gfxAsyncRan) return;   // fell back to LLE: nothing to draw
    t0 = h64_prof_now(sys);
    h64_gfx_render(sys, hle->gfx);
    sys->prof[H64_PROF_GFX_HLE] += h64_prof_now(sys) - t0;
}

void h64_hle_async_wait(H64System *sys)
{
    // Every queued job (presents too): the renderer is the worker's until then.
    if (sys->asyncWait)
    {
        u64 t0 = h64_prof_now(sys);
        sys->asyncWait(sys->asyncUser);
        sys->prof[H64_PROF_ASYNC_WAIT] += h64_prof_now(sys) - t0;
    }
}

int h64_hle_idle(H64System *sys)
{
    struct hle_t *hle = sys->hle;
    return !hle || (!hle->gfxAsyncPending && !hle->audioAsyncPending);
}

static void audio_async_job(void *arg)
{
    H64System *sys = (H64System *)arg;
    struct hle_t *hle = sys->hle;
    u64 t0 = h64_prof_now(sys);
    hle->audioAsyncFunc(hle);
    sys->prof[H64_PROF_AUDIO_HLE] += h64_prof_now(sys) - t0;
}

int h64_hle_async_finish(H64System *sys, int *ran, int *fullSync, u32 *statusBits)
{
    struct hle_t *hle = sys->hle;
    if (!hle) return 0;
    if (hle->audioAsyncPending)
    {
        u64 t0 = h64_prof_now(sys);
        if (sys->asyncAudioWait) sys->asyncAudioWait(sys->asyncUser);
        sys->prof[H64_PROF_ASYNC_WAIT] += h64_prof_now(sys) - t0;
        hle->audioAsyncPending = 0;
        *fullSync = 0;
        *statusBits = hle->sp_status;
        *ran = !hle->forwarded;
        // RDRAM written by the task: the recompiler hears of it here, on the CPU thread.
        if (*ran && hle->dirtyHi > hle->dirtyLo)
            h64_jit_notify_write(sys, hle->dirtyLo, hle->dirtyHi - hle->dirtyLo);
        return 2;
    }
    if (!hle->gfxAsyncPending) return 0;
    {
        // The task ends once its display list ran; its rendering may go on,
        // except when it reads a colour image as a texture.
        u64 t0 = h64_prof_now(sys);
        sys->asyncWaitTicket(sys->asyncUser, hle->gfxParseTicket);
        if (hle->gfxAsyncMustSync || !hle->gfxAsyncRan) sys->asyncWaitTicket(sys->asyncUser, hle->gfxRenderTicket);
        sys->prof[H64_PROF_ASYNC_WAIT] += h64_prof_now(sys) - t0;
    }
    hle->gfxAsyncPending = 0;
    *ran = hle->gfxAsyncRan;
    *fullSync = hle->gfxAsyncFullSync;
    return 1;
}

int h64_hle_try_task(H64System *sys, u32 *statusBits, u32 *busyCycles, int *dpInterrupt)
{
    struct hle_t *hle = sys->hle;
    uint32_t uc_start, uc_dstart, uc_dsize;
    struct cached_ucodes_t *cached_ucodes;
    struct ucode_info_t *info = NULL;
    int i, match = 0;

    *dpInterrupt = 0;
    if (!hle || !(sys->options.hleAudio || sys->options.hleAudioCheck || sys->options.hleGfx))
        return 0;
    // A task always starts at the boot microcode (osSpTaskStartGo sets PC 0).
    if (sys->rsp.pc != 0)
        return 0;

    if (is_task(hle) && *dmem_u32(hle, TASK_TYPE) == 1)
    {
        int fullSync = 0;
        if (!sys->options.hleGfx)
            return 0;
        if (!hle->gfx) hle->gfx = h64_gfx_create(sys);
        if (sys->asyncStart)
        {
            // On the worker: the CPU goes on; the task ends (or falls back
            // to LLE) after asyncGfxCycles, where the CPU waits for it.
            if (!h64_gfx_task_known(sys, hle->gfx)) return 0;
            h64_hle_async_wait(sys);   // never two at once (cannot happen: the RSP is busy)
            hle->gfxAsyncPending = 1;
            hle->gfxAsyncRan = 0;
            hle->gfxAsyncFullSync = 0;
            hle->gfxAsyncMustSync = 0;
            hle->gfxParseTicket = sys->asyncStart(sys->asyncUser, gfx_parse_job, sys);
            hle->gfxRenderTicket = sys->asyncStart(sys->asyncUser, gfx_render_job, sys);
            *statusBits = SP_STATUS_TASKDONE | SP_STATUS_BROKE | SP_STATUS_HALT;
            *busyCycles = sys->asyncGfxCycles ? sys->asyncGfxCycles : H64_HLE_GFX_CYCLES;
            return 1;
        }
        u64 t0 = h64_prof_now(sys);
        int ran = h64_gfx_run_task(sys, hle->gfx, &fullSync);
        sys->prof[H64_PROF_GFX_HLE] += h64_prof_now(sys) - t0;
        if (!ran)
            return 0;
        *statusBits = SP_STATUS_TASKDONE | SP_STATUS_BROKE | SP_STATUS_HALT;
        *busyCycles = H64_HLE_GFX_CYCLES;
        *dpInterrupt = fullSync;
        return 1;
    }
    if (!(sys->options.hleAudio || sys->options.hleAudioCheck))
        return 0;

    uc_start = *dmem_u32(hle, TASK_UCODE);
    uc_dstart = *dmem_u32(hle, TASK_UCODE_DATA);
    uc_dsize = *dmem_u32(hle, TASK_UCODE_DATA_SIZE);

    cached_ucodes = &hle->cached_ucodes;
    for (i = 0; i < cached_ucodes->count; i++)
    {
        info = &cached_ucodes->infos[i];
        if (info->uc_start == uc_start && info->uc_dstart == uc_dstart && info->uc_dsize == (uint16_t)uc_dsize)
        {
            match = 1;
            break;
        }
    }
    if (!match)
    {
        if (cached_ucodes->count >= CACHED_UCODES_MAX_SIZE)
            cached_ucodes->count = 0;
        info = &cached_ucodes->infos[cached_ucodes->count++];
        info->uc_start = uc_start;
        info->uc_dstart = uc_dstart;
        info->uc_dsize = (uint16_t)uc_dsize;
        info->uc_pfunc = task_detection(hle);
    }

    hle->forwarded = 0;
    hle->sp_status = 0;
    hle->dirtyLo = hle->dirtyHi = 0;
    if (sys->asyncAudioStart && !sys->options.hleAudioCheck && info->uc_pfunc != &forward_task)
    {
        // On the audio worker; the end (status bits, RDRAM output) is taken at
        // the end of the busy time, where the CPU waits for it.
        hle->audioAsyncPending = 1;
        hle->audioAsyncFunc = info->uc_pfunc;
        sys->asyncAudioStart(sys->asyncUser, audio_async_job, sys);
        *statusBits = 0;
        *busyCycles = sys->asyncAudioCycles ? sys->asyncAudioCycles : H64_HLE_AUDIO_CYCLES;
        return 1;
    }
    if (sys->options.hleAudioCheck)
    {
        // Run the HLE on a copy of RDRAM, then let the LLE run the task.
        if (!hle->checkRam) hle->checkRam = (u8 *)malloc(H64_RDRAM_SIZE);
        if (!hle->checkMask) hle->checkMask = (u8 *)calloc(1, H64_RDRAM_SIZE);
        if (!hle->checkRam || !hle->checkMask) return 0;
        if (hle->checkPending) h64_hle_check_end(sys);
        if (hle->checkHi > hle->checkLo) memset(hle->checkMask + hle->checkLo, 0, hle->checkHi - hle->checkLo);
        memcpy(hle->checkRam, sys->rdram, H64_RDRAM_SIZE);
        hle->dram = hle->checkRam;
        info->uc_pfunc(hle);
        hle->dram = sys->rdram;
        if (!hle->forwarded && hle->dirtyHi > hle->dirtyLo)
        {
            hle->checkPending = 1;
            hle->checkLo = hle->dirtyLo;
            hle->checkHi = hle->dirtyHi;
        }
        return 0;
    }
    {
        u64 t0 = h64_prof_now(sys);
        info->uc_pfunc(hle);
        sys->prof[H64_PROF_AUDIO_HLE] += h64_prof_now(sys) - t0;
    }
    if (hle->forwarded)
        return 0;

    if (hle->dirtyHi > hle->dirtyLo)
        h64_jit_notify_write(sys, hle->dirtyLo, hle->dirtyHi - hle->dirtyLo);
    *statusBits = hle->sp_status;
    *busyCycles = H64_HLE_AUDIO_CYCLES;
    return 1;
}
