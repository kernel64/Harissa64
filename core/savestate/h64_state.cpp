// Harissa64 V2 - save states (see h64_state.h).
//
// Layout: "H64S", format version, ROM header CRCs, then the sections below in
// a fixed order, each opened by a 4-character marker. A single visitor walks
// the machine for the three passes: save, verify (reads everything, changes
// nothing) and load, so the field lists cannot drift apart.
#include "h64_state.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../hle/h64_hle.h"
#include "../rdp/h64_rdp_state.h"
#include "../system/h64_system.h"
#include "../../render/api.h"

enum { IO_SAVE = 0, IO_VERIFY, IO_LOAD };

struct StateIO
{
    int mode;
    std::vector<u8> *out;
    const u8 *in;
    u32 size, pos;
    int error;
    std::vector<u8> scratch;   // verify pass: blocks are decoded here
};

// ---- Run-length coding ----
// Control byte c: c < 128 -> c + 1 literal bytes follow; c >= 128 -> the next
// byte repeated c - 125 times (3..130).
void h64_rle_encode(const u8 *in, u32 size, std::vector<u8> *out)
{
    u32 i = 0;
    while (i < size)
    {
        u32 run = 1;
        while (i + run < size && run < 130 && in[i + run] == in[i]) run++;
        if (run >= 3)
        {
            out->push_back((u8)(run + 125));
            out->push_back(in[i]);
            i += run;
        }
        else
        {
            u32 start = i, n = 0;
            while (i < size && n < 128)
            {
                if (i + 2 < size && in[i] == in[i + 1] && in[i] == in[i + 2]) break;
                i++;
                n++;
            }
            out->push_back((u8)(n - 1));
            out->insert(out->end(), in + start, in + start + n);
        }
    }
}

u32 h64_rle_decode(const u8 *in, u32 inSize, u8 *out, u32 outSize)
{
    u32 i = 0, o = 0;
    while (o < outSize)
    {
        u32 c, n;
        if (i >= inSize) return 0;
        c = in[i++];
        if (c < 128)
        {
            n = c + 1;
            if (i + n > inSize || o + n > outSize) return 0;
            memcpy(out + o, in + i, n);
            i += n;
        }
        else
        {
            n = c - 125;
            if (i >= inSize || o + n > outSize) return 0;
            memset(out + o, in[i++], n);
        }
        o += n;
    }
    return i;
}

// ---- Scalars (big-endian) ----
static void io_raw(StateIO *io, u8 *bytes, u32 n)
{
    if (io->error) return;
    if (io->mode == IO_SAVE)
    {
        io->out->insert(io->out->end(), bytes, bytes + n);
        return;
    }
    if (io->pos + n > io->size) { io->error = 1; return; }
    if (io->mode == IO_LOAD) memcpy(bytes, io->in + io->pos, n);
    io->pos += n;
}

// n-byte big-endian integer (n = 1..8).
static void io_be(StateIO *io, u64 *v, int n)
{
    u8 b[8];
    int i;
    for (i = 0; i < n; i++) b[i] = (u8)(*v >> (8 * (n - 1 - i)));
    io_raw(io, b, (u32)n);
    if (io->mode == IO_LOAD && !io->error)
        for (*v = 0, i = 0; i < n; i++) *v = (*v << 8) | b[i];
}

static void io_u64(StateIO *io, u64 *v) { io_be(io, v, 8); }
static void io_u32(StateIO *io, u32 *v) { u64 x = *v; io_be(io, &x, 4); if (io->mode == IO_LOAD) *v = (u32)x; }
static void io_u16(StateIO *io, u16 *v) { u64 x = *v; io_be(io, &x, 2); if (io->mode == IO_LOAD) *v = (u16)x; }
static void io_u8(StateIO *io, u8 *v) { io_raw(io, v, 1); }
static void io_s32(StateIO *io, s32 *v) { u32 x = (u32)*v; io_u32(io, &x); if (io->mode == IO_LOAD) *v = (s32)x; }
static void io_s16(StateIO *io, s16 *v) { u16 x = (u16)*v; io_u16(io, &x); if (io->mode == IO_LOAD) *v = (s16)x; }
static void io_int(StateIO *io, int *v) { u32 x = (u32)*v; io_u32(io, &x); if (io->mode == IO_LOAD) *v = (int)(s32)x; }

static void io_u32s(StateIO *io, u32 *v, u32 n) { u32 i; for (i = 0; i < n; i++) io_u32(io, &v[i]); }
static void io_u64s(StateIO *io, u64 *v, u32 n) { u32 i; for (i = 0; i < n; i++) io_u64(io, &v[i]); }
static void io_u16s(StateIO *io, u16 *v, u32 n) { u32 i; for (i = 0; i < n; i++) io_u16(io, &v[i]); }

static void io_marker(StateIO *io, const char *tag)
{
    u8 b[4];
    memcpy(b, tag, 4);
    io_raw(io, b, 4);
    if (io->mode != IO_SAVE && !io->error && memcmp(io->in + io->pos - 4, tag, 4))
    {
        H64_WARN("[state] section %.4s expected", tag);
        io->error = 1;
    }
}

// A large block in guest byte order: size, coded length, run-length coded bytes.
static void io_block(StateIO *io, u8 *data, u32 size)
{
    u32 stored = size, coded = 0;
    int mode = io->mode;
    if (mode == IO_VERIFY) io->mode = IO_LOAD;   // the two lengths are locals: read them in both passes
    io_u32(io, &stored);
    if (mode == IO_SAVE)
    {
        size_t at = io->out->size();
        io_u32(io, &coded);
        h64_rle_encode(data, size, io->out);
        coded = (u32)(io->out->size() - at - 4);
        h64_store_be32(&(*io->out)[at], coded);
        return;
    }
    io_u32(io, &coded);
    io->mode = mode;
    if (io->error) return;
    if (stored != size || io->pos + coded > io->size) { io->error = 1; return; }
    if (io->mode == IO_VERIFY)
    {
        io->scratch.resize(size);
        data = &io->scratch[0];
    }
    if (h64_rle_decode(io->in + io->pos, coded, data, size) != coded) { io->error = 1; return; }
    io->pos += coded;
}

// ---- The machine ----
static void visit_cpu(StateIO *io, H64Cpu *c)
{
    int i;
    io_marker(io, "CPU ");
    io_u64s(io, c->gpr, 32);
    io_u64(io, &c->hi);
    io_u64(io, &c->lo);
    io_u32(io, &c->lastOp);
    io_u64(io, &c->pc);
    io_u64(io, &c->nextPc);
    io_int(io, &c->branchPending);
    io_u64(io, &c->curPc);
    io_int(io, &c->curInDelaySlot);
    io_u64s(io, c->cop0, 32);
    io_u32(io, &c->countOffset);
    io_u64(io, &c->cop0Latch);
    io_u64(io, &c->cop2Latch);
    io_int(io, &c->llbit);
    io_u64s(io, c->fgr, 32);
    io_u32(io, &c->fcr31);
    for (i = 0; i < 32; i++)
    {
        io_u32(io, &c->tlb[i].pageMask);
        io_u64(io, &c->tlb[i].entryHi);
        io_u32(io, &c->tlb[i].entryLo0);
        io_u32(io, &c->tlb[i].entryLo1);
    }
    io_u64(io, &c->cycles);
    io_u32(io, &c->cpi);
    io_u64(io, &c->instructions);
    io_int(io, &c->exceptionRaised);
}

static void visit_rsp(StateIO *io, H64Rsp *r)
{
    int i;
    io_marker(io, "RSP ");
    io_u32s(io, r->r, 32);
    io_u32(io, &r->pc);
    io_u32(io, &r->nextPc);
    for (i = 0; i < 32; i++) io_u16s(io, r->vr[i], 8);
    io_u16s(io, r->acch, 8);
    io_u16s(io, r->accm, 8);
    io_u16s(io, r->accl, 8);
    io_u16s(io, r->vcoh, 8);
    io_u16s(io, r->vcol, 8);
    io_u16s(io, r->vcch, 8);
    io_u16s(io, r->vccl, 8);
    io_u16s(io, r->vce, 8);
    io_s16(io, &r->divin);
    io_s16(io, &r->divout);
    io_int(io, &r->divdp);
    io_u32(io, &r->status);
    io_u32(io, &r->semaphore);
    for (i = 0; i < 2; i++)
    {
        H64SpDma *d = i ? &r->current : &r->pending;
        io_u32(io, &d->memAddr);
        io_u32(io, &d->dramAddr);
        io_u32(io, &d->length);
        io_u32(io, &d->count);
        io_u32(io, &d->skip);
        io_int(io, &d->toRdram);
    }
    io_int(io, &r->dmaFull);
    io_int(io, &r->dmaBusy);
    io_u32(io, &r->cycleFrac);
    io_u64(io, &r->syncedCycles);
    io_u64(io, &r->instructions);
    io_u32(io, &r->tasks);
    io_int(io, &r->hleBusy);
    io_u32(io, &r->hleStatus);
    io_int(io, &r->hleDpInterrupt);
    io_u32(io, &r->hleTasks);
}

static void visit_rdp_state(StateIO *io, H64RdpState *s)
{
    int i;
    io_marker(io, "RDPS");
    io_raw(io, s->tmem, sizeof(s->tmem));
    for (i = 0; i < 8; i++)
    {
        H64RdpTile *t = &s->tiles[i];
        io_u32(io, &t->slo);
        io_u32(io, &t->shi);
        io_u32(io, &t->tlo);
        io_u32(io, &t->thi);
        io_u32(io, &t->offset);
        io_u32(io, &t->stride);
        io_u8(io, &t->fmt);
        io_u8(io, &t->size);
        io_u8(io, &t->palette);
        io_u8(io, &t->maskS);
        io_u8(io, &t->shiftS);
        io_u8(io, &t->maskT);
        io_u8(io, &t->shiftT);
        io_u8(io, &t->flags);
    }
    io_u32(io, &s->rasterFlags);
    io_u32(io, &s->dither);
    io_u32(io, &s->depthBlendFlags);
    io_u8(io, &s->coverageMode);
    io_u8(io, &s->zMode);
    io_raw(io, &s->blend[0][0], sizeof(s->blend));
    io_int(io, &s->usePrimDepth);
    for (i = 0; i < 2; i++) io_raw(io, &s->combiner[i].rgbMulAdd, 8);
    io_u32(io, &s->primColor);
    io_u32(io, &s->envColor);
    io_u32(io, &s->fogColor);
    io_u32(io, &s->blendColor);
    io_u32(io, &s->fillColor);
    io_u8(io, &s->primMinLevel);
    io_u8(io, &s->primLodFrac);
    io_s32(io, &s->primDepth);
    io_u32(io, &s->primDz);
    for (i = 0; i < 6; i++) io_s32(io, &s->convert[i]);
    io_u32s(io, s->keyWidth, 3);
    io_u32s(io, s->keyCenter, 3);
    io_u32s(io, s->keyScale, 3);
    io_u32(io, &s->scissorXlo);
    io_u32(io, &s->scissorYlo);
    io_u32(io, &s->scissorXhi);
    io_u32(io, &s->scissorYhi);
    io_u32(io, &s->texAddr);
    io_u32(io, &s->texWidth);
    io_u8(io, &s->texFmt);
    io_u8(io, &s->texSize);
    io_u32(io, &s->colorAddr);
    io_u32(io, &s->colorWidth);
    io_int(io, &s->colorFmt);
    io_u32(io, &s->depthAddr);
    io_u16(io, &s->noise);
    io_u32(io, &s->primitives);
}

static void visit_save_media(StateIO *io, H64SaveMem *s)
{
    io_marker(io, "SAVE");
    io_int(io, &s->type);
    io_int(io, &s->pak);
    io_int(io, &s->domain2);
    io_block(io, s->eeprom, sizeof(s->eeprom));
    io_block(io, s->sram, sizeof(s->sram));
    io_block(io, s->flash, sizeof(s->flash));
    io_block(io, s->pakData, sizeof(s->pakData));
    io_int(io, &s->flashMode);
    io_u32(io, &s->flashOffset);
    io_u64(io, &s->flashStatus);
    io_raw(io, s->flashPage, sizeof(s->flashPage));
}

static void visit(StateIO *io, H64System *sys, u8 *hidden)
{
    int i;
    visit_cpu(io, &sys->cpu);
    io_marker(io, "SCHD");
    io_u64s(io, sys->sched.when, H64_EV_COUNT);
    io_u64(io, &sys->sched.next);

    io_marker(io, "MEM ");
    io_block(io, sys->rdram, H64_RDRAM_SIZE);
    io_block(io, hidden, H64_RDRAM_SIZE / 2);
    io_raw(io, sys->spMem, sizeof(sys->spMem));
    io_raw(io, sys->pifRam, sizeof(sys->pifRam));

    io_marker(io, "RCP ");
    io_u32(io, &sys->mi.mode);
    io_u32(io, &sys->mi.version);
    io_u32(io, &sys->mi.intr);
    io_u32(io, &sys->mi.mask);
    io_u32s(io, sys->vi.regs, 14);
    io_u32(io, &sys->vi.vIntr);
    io_u64(io, &sys->vi.frameStart);
    io_u64(io, &sys->vi.frameCycles);
    io_u32(io, &sys->vi.frames);
    io_u32(io, &sys->ai.dramAddr);
    io_u32(io, &sys->ai.len);
    io_u32(io, &sys->ai.control);
    io_u32(io, &sys->ai.status);
    io_u32(io, &sys->ai.dacrate);
    io_u32(io, &sys->ai.bitrate);
    io_u32s(io, sys->ai.fifoLen, 2);
    io_u32(io, &sys->ai.fifoCount);
    io_u64(io, &sys->ai.bufferCycles);
    io_u32s(io, sys->pi.regs, 13);
    io_u32(io, &sys->pi.latch);
    io_u64(io, &sys->pi.latchUntil);
    io_u32s(io, sys->ri.regs, 8);
    io_u32(io, &sys->si.dramAddr);
    io_u32(io, &sys->si.pifAddrRd);
    io_u32(io, &sys->si.pifAddrWr);
    io_u32(io, &sys->si.status);
    io_u32s(io, sys->miRaised, 6);
    io_int(io, &sys->tvType);

    visit_rsp(io, &sys->rsp);
    io_marker(io, "DP  ");
    io_u32(io, &sys->dp.start);
    io_u32(io, &sys->dp.end);
    io_u32(io, &sys->dp.current);
    io_u32(io, &sys->dp.status);
    io_u32(io, &sys->dp.clock);
    io_u32(io, &sys->dp.bufBusy);
    io_u32(io, &sys->dp.pipeBusy);
    io_u32(io, &sys->dp.tmemCounter);
    for (i = 0; i < 22; i++) io_u64(io, &sys->dpCommand[i]);
    io_u32(io, &sys->dpPendingWords);
    io_u64(io, &sys->dpCommands);
    visit_rdp_state(io, h64_rdp_state(sys));
    visit_save_media(io, sys->save);
    io_marker(io, "END ");
}

static void visit_header(StateIO *io, H64System *sys)
{
    u8 magic[4] = { 'H', '6', '4', 'S' };
    u32 version = H64_STATE_VERSION, crc1 = sys->rom.crc1, crc2 = sys->rom.crc2;
    int mode = io->mode;
    if (mode == IO_VERIFY) io->mode = IO_LOAD;   // only locals here: read them in both passes
    io_raw(io, magic, 4);
    io_u32(io, &version);
    io_u32(io, &crc1);
    io_u32(io, &crc2);
    io->mode = mode;
    if (mode == IO_SAVE || io->error) return;
    if (memcmp(magic, "H64S", 4)) { H64_WARN("[state] not a save state"); io->error = 1; }
    else if (version != H64_STATE_VERSION) { H64_WARN("[state] format version %u, this build reads %u", version, H64_STATE_VERSION); io->error = 1; }
    else if (crc1 != sys->rom.crc1 || crc2 != sys->rom.crc2) { H64_WARN("[state] made with another ROM (CRC %08X %08X)", crc1, crc2); io->error = 1; }
}

int h64_state_quiet(H64System *sys)
{
    return !sys->rsp.hleBusy && h64_hle_idle(sys);
}

int h64_state_save(H64System *sys, std::vector<u8> *out)
{
    StateIO io;
    std::vector<u8> hidden;
    if (!h64_state_quiet(sys)) return -1;
    h64_hle_async_wait(sys);   // deferred rendering may still update the RDP state and RDRAM
    io.mode = IO_SAVE;
    io.out = out;
    io.in = 0;
    io.size = io.pos = 0;
    io.error = 0;
    if (!sys->rdramHidden)
    {
        hidden.assign(H64_RDRAM_SIZE / 2, 3);   // never drawn by the software RDP: the reset value
    }
    visit_header(&io, sys);
    visit(&io, sys, sys->rdramHidden ? sys->rdramHidden : &hidden[0]);
    H64_INFO("[state] saved at frame %u: %u bytes", sys->vi.frames, (u32)out->size());
    return 0;
}

int h64_state_load(H64System *sys, const u8 *data, u32 size)
{
    StateIO io;
    io.out = 0;
    io.in = data;
    io.size = size;
    io.error = 0;
    // Verify everything first: a bad state leaves the machine as it was.
    io.mode = IO_VERIFY;
    io.pos = 0;
    visit_header(&io, sys);
    visit(&io, sys, 0);
    if (io.error || io.pos != size)
    {
        H64_WARN("[state] rejected (bad or truncated data)");
        return -1;
    }
    io.scratch.clear();

    // Nothing may run beside the machine while it changes.
    h64_hle_async_wait(sys);
    if (sys->asyncAudioWait) sys->asyncAudioWait(sys->asyncUser);
    if (!sys->rdramHidden) sys->rdramHidden = (u8 *)malloc(H64_RDRAM_SIZE / 2);
    if (!sys->rdramHidden) return -1;
    io.mode = IO_LOAD;
    io.pos = 0;
    visit_header(&io, sys);
    visit(&io, sys, sys->rdramHidden);
    if (io.error) return -1;   // cannot happen after the verify pass

    // Derived state: recompiled code, RSP HLE, renderer caches.
    h64_jit_reset(sys);
    if (sys->hle) h64_hle_free(sys->hle);
    sys->hle = h64_hle_create(sys);
    if (sys->renderer && sys->renderer->reset) sys->renderer->reset(sys->renderer->user);
    H64_INFO("[state] loaded: frame %u, pc %08X", sys->vi.frames, (u32)sys->cpu.pc);
    return 0;
}
