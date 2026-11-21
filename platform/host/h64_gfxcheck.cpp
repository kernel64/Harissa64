// Harissa64 V2 - graphics HLE check (h64_gfxcheck.h).
#include "h64_gfxcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../core/common/h64_endian.h"
#include "../../core/hle/h64_gfx.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/system/h64_system.h"
#include "../../render/api.h"
#include "png_write.h"

struct ChkImage { u32 addr, size, width, height; int depth; };

struct GfxChk
{
    H64System *sys, *shell;
    H64Gfx *gfx;
    H64Renderer mainR, shellR;
    int active;
    int halted, fullSync;      // the LLE task ended / its SYNC_FULL reached the RDP
    std::vector<u64> lle, hle;   // each command: its word count, then its words
    std::string dir;
    int maxPictures, pictures;
    u32 tasks, skipped, bad, identical, pending;
    int warm;
    double worstPct;
    u32 worstTask, worstFrame;
};
static GfxChk s_chk;

static void append(std::vector<u64> &v, const u64 *w, u32 n)
{
    v.push_back(n);
    v.insert(v.end(), w, w + n);
}

static void task_end(H64System *sys);

// Some microcodes (DK64's) write the whole RDP list to RDRAM and the CPU sends
// it after the task: the pictures are compared once the task has ended and
// its SYNC_FULL has run, whichever comes last.
static void main_rdp(void *user, const u64 *w, u32 n)
{
    if (s_chk.active) append(s_chk.lle, w, n);
    h64_rdp_command((H64System *)user, w, n);
    if (s_chk.active && ((u32)(w[0] >> 56) & 0x3F) == 0x29)
    {
        s_chk.fullSync = 1;
        if (s_chk.halted) task_end((H64System *)user);
    }
}

static void main_triangle(void *user, const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c,
                          u32 flags, u32 tile, u32 levels)
{
    u64 w[22];
    u32 n = h64_rdp_build_triangle(a, b, c, flags, tile, levels, w);
    h64_rdp_command((H64System *)user, w, n);
}

static void shell_rdp(void *user, const u64 *w, u32 n)
{
    append(s_chk.hle, w, n);
    h64_rdp_command((H64System *)user, w, n);
}

static void shell_triangle(void *user, const H64RenderVertex *a, const H64RenderVertex *b, const H64RenderVertex *c,
                           u32 flags, u32 tile, u32 levels)
{
    u64 w[22];
    u32 n = h64_rdp_build_triangle(a, b, c, flags, tile, levels, w);
    append(s_chk.hle, w, n);
    h64_rdp_command((H64System *)user, w, n);
}

// The colour and depth images a stream draws into.
static void images_of(const std::vector<u64> &v, std::map<u32, ChkImage> &out)
{
    u32 cAddr = 0, cSize = 2, cWidth = 320, zAddr = 0, yhi = 240;
    int haveColor = 0, haveZ = 0;
    size_t i = 0;
    while (i < v.size())
    {
        u32 n = (u32)v[i];
        const u64 *w = &v[i + 1];
        u32 op = (u32)(w[0] >> 56) & 0x3F;
        if (op == 0x3F) { cAddr = (u32)w[0] & 0xFFFFFF; cSize = (u32)(w[0] >> 51) & 3; cWidth = ((u32)(w[0] >> 32) & 0x3FF) + 1; haveColor = 1; }
        else if (op == 0x3E) { zAddr = (u32)w[0] & 0xFFFFFF; haveZ = 1; }
        else if (op == 0x2D) yhi = (((u32)w[0] & 0xFFF) + 3) >> 2;
        else if (haveColor && ((op >= 0x08 && op <= 0x0F) || op == 0x24 || op == 0x25 || op == 0x36))
        {
            ChkImage &c = out[cAddr];
            if (!c.width) { c.addr = cAddr; c.size = cSize; c.width = cWidth; c.depth = 0; }
            if (yhi > c.height) c.height = yhi;
            if (haveZ && op >= 0x08 && op <= 0x0F && (op & 1))
            {
                ChkImage &z = out[zAddr];
                z.addr = zAddr; z.size = 2; z.width = cWidth; z.depth = 1;
                if (yhi > z.height) z.height = yhi;
            }
        }
        i += 1 + n;
    }
}

// Largest channel error per pixel (5-bit steps for 16-bit colour, 8-bit for
// 32-bit and 8-bit images; depth: 0 or 1).
static void pixel_errors(const ChkImage &im, const u8 *a, const u8 *b, std::vector<u32> &err)
{
    u32 n = im.width * im.height, k;
    u32 bpp = im.size == 3 ? 4 : im.size == 2 ? 2 : 1;
    err.assign(n, 0);
    if (im.addr + n * bpp > H64_RDRAM_SIZE) n = (H64_RDRAM_SIZE - im.addr) / bpp;
    for (k = 0; k < n; k++)
    {
        u32 o = im.addr + k * bpp, e = 0;
        if (im.depth) e = h64_load_be16(a + o) != h64_load_be16(b + o);
        else if (bpp == 2)
        {
            u32 x = h64_load_be16(a + o), y = h64_load_be16(b + o), s;
            for (s = 1; s <= 11; s += 5)
            {
                u32 d = (u32)abs((int)((x >> s) & 31) - (int)((y >> s) & 31));
                if (d > e) e = d;
            }
        }
        else if (bpp == 4)
        {
            u32 s;
            for (s = 0; s < 3; s++)
            {
                u32 d = (u32)abs((int)a[o + s] - (int)b[o + s]);
                if (d > e) e = d;
            }
        }
        else e = (u32)abs((int)a[o] - (int)b[o]);
        err[k] = e;
    }
}

static void rgb_of(const ChkImage &im, const u8 *ram, u32 k, u8 *rgb)
{
    u32 o = im.addr + k * (im.size == 3 ? 4 : im.size == 2 ? 2 : 1);
    if (im.size == 2)
    {
        u32 v = h64_load_be16(ram + o);
        rgb[0] = (u8)(((v >> 11) & 31) * 255 / 31); rgb[1] = (u8)(((v >> 6) & 31) * 255 / 31); rgb[2] = (u8)(((v >> 1) & 31) * 255 / 31);
    }
    else if (im.size == 3) { rgb[0] = ram[o]; rgb[1] = ram[o + 1]; rgb[2] = ram[o + 2]; }
    else rgb[0] = rgb[1] = rgb[2] = ram[o];
}

static void save_picture(const ChkImage &im, const std::vector<u32> &err, u32 task, u32 frame)
{
    int w = (int)im.width, h = (int)im.height, W = w * 3 + 8, x, y;
    std::vector<u8> rgb((size_t)W * h * 3, 64);
    char path[600];
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            u32 k = (u32)(y * w + x);
            u8 *p0 = &rgb[((size_t)y * W + x) * 3], *p1 = &rgb[((size_t)y * W + w + 4 + x) * 3], *p2 = &rgb[((size_t)y * W + 2 * w + 8 + x) * 3];
            u32 e = k < err.size() ? err[k] : 0;
            rgb_of(im, s_chk.sys->rdram, k, p0);
            rgb_of(im, s_chk.shell->rdram, k, p1);
            p2[0] = p2[1] = p2[2] = 0;
            if (e >= 16) { p2[0] = 255; }
            else if (e >= 4) { p2[0] = 255; p2[1] = 160; }
            else if (e >= 2) { p2[0] = 255; p2[1] = 255; }
            else if (e == 1) { p2[2] = 255; }
        }
    snprintf(path, sizeof(path), "%s/task%05u_f%u_%06X.png", s_chk.dir.c_str(), task, frame, im.addr);
    h64_png_write_rgb(path, &rgb[0], W, h);
}

// The state words each stream sent, by opcode, for the report.
static void state_sets(const std::vector<u64> &v, std::map<u32, std::set<u64> > &out)
{
    size_t i = 0;
    while (i < v.size())
    {
        u32 n = (u32)v[i];
        u64 w = v[i + 1] & ~(3ull << 62);   // the RDP ignores the top two opcode bits (S2DEX leaves them clear)
        u32 op = (u32)(w >> 56) & 0x3F;
        if (op == 0x2F || op == 0x3C || op == 0x35 || op == 0x32 || (op >= 0x37 && op <= 0x3B) || op == 0x2E || op == 0x2C)
            out[op].insert(w);
        i += 1 + n;
    }
}

static void report_states(void)
{
    std::map<u32, std::set<u64> > a, b;
    std::map<u32, std::set<u64> >::iterator it;
    state_sets(s_chk.lle, a);
    state_sets(s_chk.hle, b);
    for (it = a.begin(); it != a.end(); ++it)
    {
        std::set<u64> &l = it->second, &h = b[it->first];
        std::set<u64>::iterator j;
        int shown = 0;
        for (j = l.begin(); j != l.end() && shown < 4; ++j)
            if (!h.count(*j)) { printf("    op %02X only LLE: %016llX\n", it->first, (unsigned long long)*j); shown++; }
        shown = 0;
        for (j = h.begin(); j != h.end() && shown < 4; ++j)
            if (!l.count(*j)) { printf("    op %02X only HLE: %016llX\n", it->first, (unsigned long long)*j); shown++; }
    }
}

static void task_start(H64System *sys)
{
    H64System *sh = s_chk.shell;
    H64RdpState *ms, *ss;
    int fullSync = 0, ok;
    if (h64_load_be32(sys->spMem + 0xFC0) != 1) return;   // not a graphics task
    if (h64_load_be32(sys->spMem + 0xFC4) & 1) return;     // OS_TASK_YIELDED: a task resumed, already started
    if (s_chk.active) { s_chk.pending++; return; }
    if (!s_chk.warm)
    {
        // The first task after the start (a state loaded in the other mode)
        // may find an RDP list half built: not compared.
        s_chk.warm = 1;
        return;
    }          // the previous one waits for its list: this one is not checked
    s_chk.lle.clear();
    s_chk.hle.clear();
    memcpy(sh->rdram, sys->rdram, H64_RDRAM_SIZE);
    memcpy(sh->spMem, sys->spMem, sizeof(sh->spMem));
    sh->rsp.status = sys->rsp.status;
    memcpy(sh->cpuSprite, sys->cpuSprite, sizeof(sh->cpuSprite));
    sh->cpuSpriteNext = sys->cpuSpriteNext;
    sh->options = sys->options;
    sh->options.hleGfx = 1;
    sh->options.rdpStateOnly = 0;
    sh->options.noRdpDraw = 0;
    ms = h64_rdp_state(sys);
    ss = h64_rdp_state(sh);
    *ss = *ms;
    ss->loadRam = 0; ss->cpuRanges = 0; ss->cpuRangeCount = 0; ss->cimgFresh = 0; ss->cimgFreshCount = 0;
    ss->drawRam = 0; ss->drawRam2 = 0; ss->hires = 0; ss->probeOn = 0; ss->softDraw = 0;
    memcpy(sh->rdramHidden, sys->rdramHidden, H64_RDRAM_SIZE / 2);
    ok = h64_gfx_run_task(sh, s_chk.gfx, &fullSync);
    if (!ok || h64_gfx_stream_waiting(s_chk.gfx))
    {
        s_chk.skipped++;
        return;
    }
    s_chk.active = 1;
    s_chk.halted = s_chk.fullSync = 0;
}

static void task_end(H64System *sys)
{
    std::map<u32, ChkImage> imgs;
    std::map<u32, ChkImage>::iterator it;
    std::vector<u32> err;
    u32 task = s_chk.tasks++, frame = sys->vi.frames;
    int taskBad = 0, any = 0;
    s_chk.active = 0;
    images_of(s_chk.lle, imgs);
    images_of(s_chk.hle, imgs);
    for (it = imgs.begin(); it != imgs.end(); ++it)
    {
        const ChkImage &im = it->second;
        u32 k, hist[6] = { 0, 0, 0, 0, 0, 0 }, n;
        double pct, pct2;
        if (!im.height || !im.width || im.depth) continue;   // depth: HLE and microcode differ in precision everywhere
        pixel_errors(im, sys->rdram, s_chk.shell->rdram, err);
        n = (u32)err.size();
        for (k = 0; k < n; k++)
        {
            u32 e = err[k];
            hist[e == 0 ? 0 : e == 1 ? 1 : e < 4 ? 2 : e < 8 ? 3 : e < 16 ? 4 : 5]++;
        }
        if (hist[0] == n) continue;
        any = 1;
        pct2 = 100.0 * (n - hist[0] - hist[1]) / n;           // 2 steps or more: shading rounding, dither
        pct = 100.0 * (hist[3] + hist[4] + hist[5]) / n;      // 4 steps or more: the verdict
        if (pct > s_chk.worstPct) { s_chk.worstPct = pct; s_chk.worstTask = task; s_chk.worstFrame = frame; }
        if (pct < 0.5) continue;
        if (!taskBad)
        {
            printf("[gfxcheck] task %u (VI %u): LLE %u words, HLE %u words\n", task, frame,
                   (u32)s_chk.lle.size(), (u32)s_chk.hle.size());
            report_states();
        }
        taskBad = 1;
        printf("  color %06X %ux%u %u-bit: %.2f%% pixels 4+ steps apart (%.2f%% 2+), hist 0:%u 1:%u 2-3:%u 4-7:%u 8-15:%u 16+:%u\n",
               im.addr, im.width, im.height, 4u << im.size, pct, pct2, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
        if (s_chk.pictures < s_chk.maxPictures && !s_chk.dir.empty())
        {
            save_picture(im, err, task, frame);
            s_chk.pictures++;
        }
    }
    if (taskBad) s_chk.bad++;
    else if (!any) s_chk.identical++;
}

// The LLE commands so far draw into a colour image the HLE drew into.
static int draws_into_hle_image(void)
{
    std::map<u32, ChkImage> l, h;
    std::map<u32, ChkImage>::iterator it;
    images_of(s_chk.lle, l);
    images_of(s_chk.hle, h);
    for (it = h.begin(); it != h.end(); ++it)
        if (!it->second.depth && l.count(it->first) && !l[it->first].depth) return 1;
    return 0;
}

static void hook(H64System *sys, int start)
{
    if (start) task_start(sys);
    else if (s_chk.active && !s_chk.halted && !(sys->rsp.status & 0x100))   // SIG1: yielded, it resumes later
    {
        s_chk.halted = 1;
        if (!draws_into_hle_image())
        {
            // What reached the RDP so far is the previous task's list (sent by
            // the CPU while this task ran): this task's list comes later.
            s_chk.lle.clear();
            s_chk.fullSync = 0;
        }
        if (s_chk.fullSync) task_end(sys);
    }
}

int h64_gfxcheck_begin(H64System *sys, const char *dir, int maxPictures)
{
    H64System *sh = (H64System *)calloc(1, sizeof(H64System));
    if (!sh) return -1;
    sh->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    h64_rdp_state(sh);
    s_chk.sys = sys;
    s_chk.shell = sh;
    s_chk.gfx = h64_gfx_create(sh);
    s_chk.dir = dir ? dir : "";
    s_chk.maxPictures = maxPictures;
    s_chk.shellR.user = sh;
    s_chk.shellR.rdp = shell_rdp;
    s_chk.shellR.triangle = shell_triangle;
    s_chk.shellR.reset = 0;
    sh->renderer = &s_chk.shellR;
    s_chk.mainR.user = sys;
    s_chk.mainR.rdp = main_rdp;
    s_chk.mainR.triangle = main_triangle;
    s_chk.mainR.reset = 0;
    sys->renderer = &s_chk.mainR;
    sys->rspTaskHook = hook;
    return 0;
}

void h64_gfxcheck_end(H64System *sys)
{
    (void)sys;
    if (!s_chk.shell) return;
    printf("[gfxcheck] %u graphics tasks compared: %u identical, %u with differences under 0.5%% of an image (4+ steps), %u over; "
           "%u left to the LLE (microcode the HLE does not know)\n",
           s_chk.tasks, s_chk.identical, s_chk.tasks - s_chk.identical - s_chk.bad, s_chk.bad, s_chk.skipped);
    if (s_chk.pending) printf("[gfxcheck] %u tasks not checked (started while the previous one waited for its RDP list)\n", s_chk.pending);
    if (s_chk.worstPct > 0) printf("[gfxcheck] largest difference: %.2f%% of an image, task %u (VI %u)\n", s_chk.worstPct, s_chk.worstTask, s_chk.worstFrame);
}
