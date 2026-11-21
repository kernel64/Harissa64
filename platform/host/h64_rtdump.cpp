// Harissa64 V2 - render test scenes: recording and software-RDP replay.
// Format: h64_rtdump.h (ParaLLEl-RDP's RDPDUMP2 layout, MIT).
#include "h64_rtdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/system/h64_system.h"
#include "../../render/api.h"

enum
{
    REC_UPDATE_DRAM = 1, REC_RDP_COMMAND = 2, REC_SET_VI = 3, REC_END_FRAME = 4, REC_SIGNAL_COMPLETE = 5,
    REC_EOF = 6, REC_UPDATE_DRAM_FLUSH = 7, REC_UPDATE_HIDDEN = 8, REC_UPDATE_HIDDEN_FLUSH = 9
};

static const u32 HIDDEN_SIZE = H64_RDRAM_SIZE / 2;

// ---- Recording -------------------------------------------------------------

struct RtRec
{
    FILE *f;
    u32 first, count, frames;
    int started, done;
    u8 *cache;            // RDRAM as the scene last gave it (N64 byte order)
    u64 lastCycles;
    int haveCycles;
    // The last command of each kind that sets state (opcode 0x20..0x3F; SET_TILE and SET_TILE_SIZE per tile).
    u64 state[64];
    u8 stateSet[64];
    u64 tile[8], tileSize[8];
    u8 tileSet[8], tileSizeSet[8];
    u32 commands, triangles, ramBytes;
};
static RtRec s_rec;

static void put(u32 v) { fwrite(&v, 4, 1, s_rec.f); }

// N64-order bytes as host 32-bit words (what both oracles keep), little-endian PC.
static void put_ram(const u8 *src, u32 size)
{
    u8 tmp[4096];
    while (size)
    {
        u32 n = size < sizeof(tmp) ? size : (u32)sizeof(tmp), i;
        for (i = 0; i < n; i += 4) { tmp[i] = src[i + 3]; tmp[i + 1] = src[i + 2]; tmp[i + 2] = src[i + 1]; tmp[i + 3] = src[i]; }
        fwrite(tmp, 1, n, s_rec.f);
        src += n;
        size -= n;
    }
}

static void put_command(const u64 *w, u32 n)
{
    u32 k;
    put(REC_RDP_COMMAND);
    put((u32)(w[0] >> 56) & 0x3F);
    put(n * 2);
    for (k = 0; k < n; k++) { put((u32)(w[k] >> 32)); put((u32)w[k]); }
    s_rec.commands++;
}

// RDRAM changed since the cache: runs of changed 32-bit words (gaps under 32
// bytes merged), then a flush marker.
static void put_ram_changes(const u8 *ram)
{
    const u32 block = 4096;
    u32 b, any = 0;
    for (b = 0; b < H64_RDRAM_SIZE; b += block)
    {
        u32 i, start = 0, last = 0;
        int open = 0;
        if (!memcmp(ram + b, s_rec.cache + b, block)) continue;
        for (i = b; i < b + block; i += 4)
        {
            if (memcmp(ram + i, s_rec.cache + i, 4))
            {
                if (open && i - last > 32) { put(REC_UPDATE_DRAM); put(start); put(last + 4 - start); put_ram(ram + start, last + 4 - start); s_rec.ramBytes += last + 4 - start; open = 0; }
                if (!open) { start = i; open = 1; }
                last = i;
            }
        }
        if (open) { put(REC_UPDATE_DRAM); put(start); put(last + 4 - start); put_ram(ram + start, last + 4 - start); s_rec.ramBytes += last + 4 - start; }
        memcpy(s_rec.cache + b, ram + b, block);
        any = 1;
    }
    if (any) put(REC_UPDATE_DRAM_FLUSH);
}

static void put_state_commands(void)
{
    // Images and modes first, tiles after (SET_TILE_SIZE after its SET_TILE).
    static const u8 order[] = { 0x3F, 0x3E, 0x3D, 0x2D, 0x2F, 0x3C, 0x2E, 0x2C, 0x2A, 0x2B, 0x37, 0x38, 0x39, 0x3A, 0x3B };
    u32 k;
    for (k = 0; k < sizeof(order); k++)
        if (s_rec.stateSet[order[k]]) put_command(&s_rec.state[order[k]], 1);
    for (k = 0; k < 8; k++)
    {
        if (s_rec.tileSet[k]) put_command(&s_rec.tile[k], 1);
        if (s_rec.tileSizeSet[k]) put_command(&s_rec.tileSize[k], 1);
    }
}

static void start(H64System *sys)
{
    H64RdpState *st = h64_rdp_state(sys);
    u64 w;
    s_rec.started = 1;
    // Hidden RDRAM (coverage and the low depth bits) as it is now.
    put(REC_UPDATE_HIDDEN); put(0); put(HIDDEN_SIZE);
    if (sys->rdramHidden) fwrite(sys->rdramHidden, 1, HIDDEN_SIZE, s_rec.f);
    else { std::vector<u8> h(HIDDEN_SIZE, 3); fwrite(&h[0], 1, HIDDEN_SIZE, s_rec.f); }
    put(REC_UPDATE_HIDDEN_FLUSH);
    // TMEM: its 4 KB put in RDRAM block 0 and loaded with one 16-bit LOAD_BLOCK
    // (dxt 0: one line, no odd-line swap), then block 0 restored below.
    put(REC_UPDATE_DRAM); put(0); put(4096); put_ram(st->tmem, 4096);
    put(REC_UPDATE_DRAM_FLUSH);
    w = (u64)((0x3Du << 24) | (0u << 21) | (2u << 19)) << 32;            // SET_TEXTURE_IMAGE RGBA 16-bit, width 1, address 0
    put_command(&w, 1);
    w = (u64)((0x35u << 24) | (0u << 21) | (2u << 19)) << 32 | (7u << 24); // SET_TILE 7: RGBA 16-bit, line 0, TMEM 0
    put_command(&w, 1);
    w = (u64)(0x33u << 24) << 32 | (7u << 24) | (2047u << 12);           // LOAD_BLOCK tile 7, texels 0..2047, dxt 0
    put_command(&w, 1);
    w = (u64)(0x27u << 24) << 32;                                        // SYNC_PIPE
    put_command(&w, 1);
    // The whole RDRAM (block 0 included), against an empty cache.
    memset(s_rec.cache, 0, H64_RDRAM_SIZE);
    memcpy(s_rec.cache, st->tmem, 4096);   // block 0 holds the TMEM image: rewritten where it differs
    put_ram_changes(sys->rdram);
    put_state_commands();
    s_rec.lastCycles = sys->cpu.cycles;
    s_rec.haveCycles = 1;
    printf("[rtdump] recording from VI frame %u\n", sys->vi.frames);
}

int h64_rtdump_open(const char *path, u32 first, u32 count)
{
    u32 v;
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.f = fopen(path, "wb");
    if (!s_rec.f) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    s_rec.cache = (u8 *)calloc(1, H64_RDRAM_SIZE);
    s_rec.first = first;
    s_rec.count = count ? count : 1;
    fwrite("RDPDUMP2", 1, 8, s_rec.f);
    v = H64_RDRAM_SIZE; fwrite(&v, 4, 1, s_rec.f);
    v = HIDDEN_SIZE; fwrite(&v, 4, 1, s_rec.f);
    return 0;
}

int h64_rtdump_active(void) { return s_rec.f != 0; }

static void before_command(H64System *sys)
{
    if (s_rec.haveCycles && sys->cpu.cycles == s_rec.lastCycles) return;
    put_ram_changes(sys->rdram);
    s_rec.lastCycles = sys->cpu.cycles;
    s_rec.haveCycles = 1;
}

static void cache_state(const u64 *w)
{
    u32 op = (u32)(w[0] >> 56) & 0x3F, t = (u32)(w[0] >> 24) & 7;
    if (op == 0x35) { s_rec.tile[t] = w[0]; s_rec.tileSet[t] = 1; }
    else if (op == 0x32) { s_rec.tileSize[t] = w[0]; s_rec.tileSizeSet[t] = 1; }
    else if ((op >= 0x2A && op <= 0x2F) || (op >= 0x37 && op <= 0x3F)) { s_rec.state[op] = w[0]; s_rec.stateSet[op] = 1; }
}

void h64_rtdump_rdp(H64System *sys, const u64 *words, u32 n)
{
    if (!s_rec.f) return;
    if (s_rec.started && !s_rec.done)
    {
        before_command(sys);
        put_command(words, n);
    }
    cache_state(words);
}

void h64_rtdump_triangle(H64System *sys, const H64RenderVertex *a, const H64RenderVertex *b,
                         const H64RenderVertex *c, u32 flags, u32 tile, u32 levels)
{
    u64 w[22];
    u32 n;
    if (!s_rec.f || !s_rec.started || s_rec.done) return;
    before_command(sys);
    put(H64_RT_HLE_TRIANGLE);
    put(flags); put(tile); put(levels);
    fwrite(a, 4, 10, s_rec.f);
    fwrite(b, 4, 10, s_rec.f);
    fwrite(c, 4, 10, s_rec.f);
    n = h64_rdp_build_triangle(a, b, c, flags, tile, levels, w);
    put_command(w, n);
    s_rec.triangles++;
}

void h64_rtdump_vi(H64System *sys)
{
    u32 k;
    if (!s_rec.f || s_rec.done) return;
    if (!s_rec.started)
    {
        if (sys->vi.frames >= s_rec.first) start(sys);
        return;
    }
    before_command(sys);
    for (k = 0; k < 14; k++) { put(REC_SET_VI); put(k); put(sys->vi.regs[k]); }
    put(REC_END_FRAME);
    if (++s_rec.frames >= s_rec.count) h64_rtdump_close();
}

void h64_rtdump_close(void)
{
    if (!s_rec.f || s_rec.done) return;
    put(REC_EOF);
    fclose(s_rec.f);
    s_rec.f = 0;
    s_rec.done = 1;
    free(s_rec.cache);
    s_rec.cache = 0;
    printf("[rtdump] %u frames, %u commands (%u HLE triangles), %u RDRAM bytes changed\n",
           s_rec.frames, s_rec.commands, s_rec.triangles, s_rec.ramBytes);
}

// ---- Replay with the software RDP --------------------------------------------

static int rd(FILE *f, u32 *v) { return fread(v, 4, 1, f) == 1; }

static int write_out(const std::string &path, const u8 *data, u32 size)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return -1; }
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

int h64_rtdump_play(int argc, char **argv)
{
    std::string dump, out;
    std::set<u32> frames;
    int all = 0, last = 1, i;
    FILE *f;
    char magic[8];
    u32 ramSize, hidSize, frame = 0, commands = 0, cmd;
    H64System *sys;
    std::vector<u32> words;
    std::vector<u64> w64;
    u8 buf[4096];
    for (i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            const char *s = argv[++i];
            last = 0;
            if (!strcmp(s, "all")) all = 1;
            else if (!strcmp(s, "last")) last = 1;
            else
                while (*s)
                {
                    frames.insert((u32)strtoul(s, (char **)&s, 10));
                    if (*s == ',') s++; else break;
                }
        }
        else if (dump.empty()) dump = argv[i];
        else if (out.empty()) out = argv[i];
    }
    if (dump.empty() || out.empty()) { fprintf(stderr, "usage: h64test --rt-play DUMP OUTDIR [--frames all|last|N,..]\n"); return 3; }
    f = fopen(dump.c_str(), "rb");
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "RDPDUMP2", 8) || !rd(f, &ramSize) || !rd(f, &hidSize) ||
        ramSize != H64_RDRAM_SIZE || hidSize != HIDDEN_SIZE)
    {
        fprintf(stderr, "not an 8 MB RDPDUMP2 scene: %s\n", dump.c_str());
        return 3;
    }
    sys = (H64System *)calloc(1, sizeof(H64System));
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    h64_rdp_state(sys);   // allocates the hidden RDRAM too
    for (;;)
    {
        u32 a, b, k;
        if (!rd(f, &cmd)) break;
        if (cmd == REC_EOF) break;
        switch (cmd)
        {
        case REC_UPDATE_DRAM:
            if (!rd(f, &a) || !rd(f, &b) || a + b > H64_RDRAM_SIZE || (a | b) & 3) { fprintf(stderr, "bad RDRAM record\n"); return 1; }
            while (b)
            {
                u32 n = b < sizeof(buf) ? b : (u32)sizeof(buf);
                if (fread(buf, 1, n, f) != n) { fprintf(stderr, "short RDRAM record\n"); return 1; }
                for (k = 0; k < n; k += 4)
                {
                    sys->rdram[a + k] = buf[k + 3]; sys->rdram[a + k + 1] = buf[k + 2];
                    sys->rdram[a + k + 2] = buf[k + 1]; sys->rdram[a + k + 3] = buf[k];
                }
                a += n;
                b -= n;
            }
            break;
        case REC_UPDATE_HIDDEN:
            if (!rd(f, &a) || !rd(f, &b) || a + b > HIDDEN_SIZE || fread(sys->rdramHidden + a, 1, b, f) != b)
            { fprintf(stderr, "bad hidden RDRAM record\n"); return 1; }
            break;
        case H64_RT_HLE_TRIANGLE:
            for (k = 0; k < 33; k++) if (!rd(f, &a)) { fprintf(stderr, "short triangle record\n"); return 1; }
            break;
        case REC_RDP_COMMAND:
            if (!rd(f, &a) || !rd(f, &b) || b < 2 || b > 44 || (b & 1)) { fprintf(stderr, "bad command record\n"); return 1; }
            words.resize(b);
            if (fread(&words[0], 4, b, f) != b) { fprintf(stderr, "short command\n"); return 1; }
            w64.resize(b / 2);
            for (k = 0; k < b / 2; k++) w64[k] = (u64)words[k * 2] << 32 | words[k * 2 + 1];
            h64_rdp_command(sys, &w64[0], b / 2);
            commands++;
            break;
        case REC_SET_VI:
            if (!rd(f, &a) || !rd(f, &b)) break;
            if (a < 14) sys->vi.regs[a] = b;
            break;
        case REC_END_FRAME:
            if (all || frames.count(frame))
            {
                char tag[32];
                sprintf(tag, "/soft_f%u", frame);
                write_out(out + tag + ".rdram", sys->rdram, H64_RDRAM_SIZE);
                write_out(out + tag + ".hidden", sys->rdramHidden, HIDDEN_SIZE);
                printf("[rt-play] wrote f%u\n", frame);
            }
            frame++;
            break;
        case REC_UPDATE_DRAM_FLUSH: case REC_UPDATE_HIDDEN_FLUSH: case REC_SIGNAL_COMPLETE: break;
        default: fprintf(stderr, "unknown record %u\n", cmd); return 1;
        }
    }
    fclose(f);
    if (last)
    {
        write_out(out + "/soft_last.rdram", sys->rdram, H64_RDRAM_SIZE);
        write_out(out + "/soft_last.hidden", sys->rdramHidden, HIDDEN_SIZE);
    }
    printf("[rt-play] %u frames, %u commands\n", frame, commands);
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
    return 0;
}
