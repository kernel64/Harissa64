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
#include "../../core/rtest/h64_rtscene.h"
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

// ---- Triangle commands for synthetic scenes ----------------------------------

static u32 le32(const std::vector<u8> &d, size_t p)
{
    return (u32)d[p] | (u32)d[p + 1] << 8 | (u32)d[p + 2] << 16 | (u32)d[p + 3] << 24;
}

static void put_le32(std::vector<u8> &d, u32 v)
{
    d.push_back((u8)v); d.push_back((u8)(v >> 8)); d.push_back((u8)(v >> 16)); d.push_back((u8)(v >> 24));
}

int h64_rtdump_tris(int argc, char **argv)
{
    std::vector<u8> in, out;
    FILE *f;
    long n;
    size_t p = 16;
    u32 added = 0;
    if (argc < 2) { fprintf(stderr, "usage: h64test --rt-tris IN OUT\n"); return 3; }
    f = fopen(argv[0], "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", argv[0]); return 3; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    in.resize(n > 16 ? (size_t)n : 16);
    if (n < 16 || fread(&in[0], 1, (size_t)n, f) != (size_t)n || memcmp(&in[0], "RDPDUMP2", 8)) { fclose(f); fprintf(stderr, "not a scene\n"); return 3; }
    fclose(f);
    out.assign(in.begin(), in.begin() + 16);
    while (p + 4 <= in.size())
    {
        u32 cmd = le32(in, p);
        size_t len = 4;
        if (cmd == 1 || cmd == 8) len = 12 + le32(in, p + 8);
        else if (cmd == 2) len = 12 + 4 * (size_t)le32(in, p + 8);
        else if (cmd == 3) len = 12;
        else if (cmd == H64_RT_HLE_TRIANGLE) len = 4 + 33 * 4;
        if (p + len > in.size()) { fprintf(stderr, "truncated record at %u\n", (u32)p); return 1; }
        out.insert(out.end(), in.begin() + p, in.begin() + p + len);
        // A recorded scene has the triangle command (0x08-0x0F) right after; any other command
        // (the scene's last SYNC_FULL...) means it is missing.
        if (cmd == H64_RT_HLE_TRIANGLE &&
            !(p + len + 8 <= in.size() && le32(in, p + len) == 2 && (le32(in, p + len + 4) & 0x38) == 0x08))
        {
            // No RDP command after it: the one our triangle setup builds (as the HLE path does).
            H64RenderVertex v[3];
            u32 w[30], k, flags = le32(in, p + 4), tile = le32(in, p + 8), levels = le32(in, p + 12), cnt;
            u64 words[22];
            for (k = 0; k < 30; k++) w[k] = le32(in, p + 16 + 4 * k);
            memcpy(v, w, sizeof(w));
            cnt = h64_rdp_build_triangle(&v[0], &v[1], &v[2], flags, tile, levels, words);
            put_le32(out, 2);
            put_le32(out, (u32)(words[0] >> 56) & 0x3F);
            put_le32(out, cnt * 2);
            for (k = 0; k < cnt; k++) { put_le32(out, (u32)(words[k] >> 32)); put_le32(out, (u32)words[k]); }
            added++;
        }
        p += len;
        if (cmd == 6) break;
    }
    f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[1]); return 3; }
    fwrite(&out[0], 1, out.size(), f);
    fclose(f);
    printf("[rt-tris] %u triangle commands built\n", added);
    return 0;
}

// ---- Replay with the software RDP (core/rtest/h64_rtscene.h) -----------------

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
    std::vector<u8> data;
    int all = 0, last = 1, i, rc, probe = 0, cvgStats = 0;
    H64RdpCvgStats cs;
    u32 probeX = 0, probeY = 0, probeFrame = 0;
    FILE *f;
    long n;
    H64RtScene sc;
    H64System *sys;
    for (i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "--cvg-stats")) { cvgStats = 1; continue; }
        if (!strcmp(argv[i], "--probe") && i + 1 < argc)
        {
            // Every software-RDP write to pixel X,Y during scene frame FRAME (mode, colours, before/after).
            if (sscanf(argv[++i], "%u,%u,%u", &probeX, &probeY, &probeFrame) != 3) { fprintf(stderr, "--probe takes X,Y,FRAME\n"); return 3; }
            probe = 1;
            continue;
        }
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
    if (dump.empty() || out.empty()) { fprintf(stderr, "usage: h64test --rt-play SCENE OUTDIR [--frames all|last|N,..] [--probe X,Y,FRAME]\n"); return 3; }
    f = fopen(dump.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", dump.c_str()); return 3; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(n > 0 ? (size_t)n : 1);
    if (n <= 0 || fread(&data[0], 1, (size_t)n, f) != (size_t)n) { fclose(f); fprintf(stderr, "cannot read %s\n", dump.c_str()); return 3; }
    fclose(f);
    if (h64_rtscene_open(&sc, &data[0], (u32)n)) { fprintf(stderr, "not an 8 MB RDPDUMP2 scene: %s\n", dump.c_str()); return 3; }
    sys = (H64System *)calloc(1, sizeof(H64System));
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    h64_rdp_state(sys);   // allocates the hidden RDRAM too
    memset(&cs, 0, sizeof(cs));
    if (cvgStats) h64_rdp_state(sys)->cvgStats = &cs;
    for (;;)
    {
        if (probe)
        {
            H64RdpState *ps = h64_rdp_state(sys);
            ps->probeOn = sc.frame == probeFrame;
            ps->probeX = probeX;
            ps->probeY = probeY;
        }
        if ((rc = h64_rtscene_play_frame(&sc, sys, 0)) != 1) break;
        u32 fr = sc.frame - 1;
        if (all || frames.count(fr))
        {
            char tag[32];
            sprintf(tag, "/soft_f%u", fr);
            write_out(out + tag + ".rdram", sys->rdram, H64_RDRAM_SIZE);
            write_out(out + tag + ".hidden", sys->rdramHidden, HIDDEN_SIZE);
            printf("[rt-play] wrote f%u\n", fr);
        }
    }
    if (rc < 0) { fprintf(stderr, "bad scene data at byte %u\n", sc.pos); return 1; }
    if (last)
    {
        write_out(out + "/soft_last.rdram", sys->rdram, H64_RDRAM_SIZE);
        write_out(out + "/soft_last.hidden", sys->rdramHidden, HIDDEN_SIZE);
    }
    printf("[rt-play] %u frames, %u commands\n", sc.frame, sc.commands);
    if (cvgStats)
    {
        // What a GPU renderer without coverage gets wrong, in pixels.
        double n = cs.pixels ? (double)cs.pixels : 1.0;
        printf("[cvg] pixels %llu | partial coverage %.2f %% | colour-on-coverage kept memory %llu (%.2f %%, %llu of them fully covered)"
               " | edge blends %llu (%.2f %%) | unblended overflows %llu\n",
               (unsigned long long)cs.pixels, 100.0 * (double)(cs.pixels - cs.pixCov[8]) / n,
               (unsigned long long)cs.cocSuppressed, 100.0 * (double)cs.cocSuppressed / n, (unsigned long long)cs.cocSuppressedFull,
               (unsigned long long)cs.edgeBlend, 100.0 * (double)cs.edgeBlend / n, (unsigned long long)cs.unblendedOverflow);
        printf("[cvg] pixel coverage 0..8:");
        for (i = 0; i < 9; i++) printf(" %llu", (unsigned long long)cs.pixCov[i]);
        printf(" | memory coverage of the kept ones 0..7:");
        for (i = 0; i < 8; i++) printf(" %llu", (unsigned long long)cs.cocMemCov[i]);
        printf("\n");
    }
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
    return 0;
}
