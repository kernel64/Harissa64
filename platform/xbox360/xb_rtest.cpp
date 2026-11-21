// Harissa64 V2 - render tests on the console (xb_rtest.h).
#include "xb_rtest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"
#include "../../core/rdp/h64_rdp.h"
#include "../../core/rdp/h64_rdp_state.h"
#include "../../core/rtest/h64_rtscene.h"
#include "../../core/system/h64_profile.h"
#include "../../core/system/h64_system.h"
#include "../../render/api.h"
#include "../../render/xenos/h64_xenos.h"
#include "xb_ui.h"

// Progress on the TV between cases (after HarissaCube's xb_rtest.cpp): a
// black screen for a minute looked like a hung console, and turning it off
// then breaks the run.
static struct { u32 index, total; char name[64]; } s_prog;

static void Progress(IDirect3DDevice9 *dev, const char *status)
{
    IDirect3DSurface9 *bb = NULL;
    D3DVIEWPORT9 vp;
    char line[160];
    float x = UI_SAFE_X, w = UI_WIDTH - 2 * UI_SAFE_X, frac;
    if (!dev || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(NULL);
    vp.X = vp.Y = 0;
    vp.Width = UI_WIDTH;
    vp.Height = UI_HEIGHT;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
    UiBegin(dev);
    UiBackdrop();
    UiText(UI_BOLD, 34, x, 120, UI_COL_TEXT, "Render tests", UI_LEFT);
    _snprintf(line, sizeof(line), "Case %u of %u", s_prog.index, s_prog.total);
    line[sizeof(line) - 1] = 0;
    UiText(UI_REGULAR, 26, x, 220, UI_COL_TEXT, line, UI_LEFT);
    UiText(UI_BOLD, 26, x, 270, UI_COL_ACCENT_HI, s_prog.name, UI_LEFT);
    UiText(UI_REGULAR, 22, x, 320, UI_COL_TEXT2, status, UI_LEFT);
    frac = s_prog.total ? (float)(s_prog.index ? s_prog.index - 1 : 0) / (float)s_prog.total : 0.0f;
    if (s_prog.index > s_prog.total) frac = 1.0f;
    UiRoundRect(x, 380, w, 14, 7, D3DCOLOR_ARGB(255, 40, 42, 52), D3DCOLOR_ARGB(255, 40, 42, 52));
    if (frac > 0) UiRoundRect(x, 380, w * frac, 14, 7, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    UiText(UI_REGULAR, 20, x, 450, UI_COL_TEXT3,
           "A test is running: please do not turn off the console. It returns to the dashboard by itself.", UI_LEFT);
    UiEnd();
    dev->Present(NULL, NULL, NULL, NULL);
    bb->Release();
}

static u8 *ReadAll(const char *path, u32 *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0, len;
    u8 *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    len = GetFileSize(h, NULL);
    buf = (u8 *)malloc(len ? len : 1);
    if (buf && !ReadFile(h, buf, len, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf || got != len) { free(buf); return NULL; }
    *size = len;
    return buf;
}

static void PutBE32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

// The images in RDRAM as records (address, count, bytes), one file.
static int WriteImages(const char *path, const u8 *rdram, const u32 *addr, const u32 *bytes, u32 n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put;
    u32 i;
    if (h == INVALID_HANDLE_VALUE) return -1;
    for (i = 0; i < n; i++)
    {
        u8 head[8];
        u32 a = addr[i], b = bytes[i];
        if (a >= H64_RDRAM_SIZE) continue;
        if (b > H64_RDRAM_SIZE - a) b = H64_RDRAM_SIZE - a;
        PutBE32(head, a);
        PutBE32(head + 4, b);
        WriteFile(h, head, 8, &put, NULL);
        WriteFile(h, rdram + a, b, &put, NULL);
    }
    CloseHandle(h);
    return 0;
}

static int RunCase(IDirect3DDevice9 *dev, const char *dir, const char *name, int scale)
{
    char path[512];
    u8 *data;
    u32 size, frames = 0, written = 0, lastCommands = 0;
    H64RtScene sc;
    H64System *sys;
    H64Renderer *r;
    H64XenosOptions o;
    int rc;
    _snprintf(path, sizeof(path), "%s\\%s\\scene.rdp", dir, name);
    path[sizeof(path) - 1] = 0;
    data = ReadAll(path, &size);
    if (!data) { H64_WARN("[rtest] %s: no scene.rdp", name); return 0; }
    if (h64_rtscene_open(&sc, data, size)) { H64_WARN("[rtest] %s: not a scene", name); free(data); return 0; }
    sys = (H64System *)calloc(1, sizeof(H64System));
    sys->rdram = (u8 *)calloc(1, H64_RDRAM_SIZE);
    if (!sys->rdram) { H64_ERROR("[rtest] out of memory"); free(sys); free(data); return 0; }
    h64_rdp_state(sys);
    {
        // The game's profile (game.txt: its 4-letter code), as in the game:
        // the renderer's per-game options (RE2's memory copies, frame slots
        // by width...) change what Xenos draws. The scene itself was recorded
        // with the profile on (h64test applies it), so only these matter here.
        u32 n = 0;
        u8 *code;
        _snprintf(path, sizeof(path), "%s\\%s\\game.txt", dir, name);
        path[sizeof(path) - 1] = 0;
        code = ReadAll(path, &n);
        if (code && n >= 4)
        {
            memcpy(sys->rom.gameCode, code, 4);
            sys->rom.gameCode[4] = 0;
            h64_apply_game_profile(sys);
        }
        free(code);
    }
    r = h64_xenos_create(sys, dev);   // sets options.rdpStateOnly: the software RDP keeps state only
    if (!r) { H64_ERROR("[rtest] %s: no renderer", name); h64_rdp_free(sys); free(sys->rdram); free(sys); free(data); return 0; }
    memset(&o, 0, sizeof(o));
    if (scale >= 1 && scale <= 3) o.scale = scale;
    else
    {
        // Automatic: one GPU pixel per N64 pixel at least (320 wide per step);
        // a 512-wide image drawn into 320 came back streaked (RE2).
        u32 w = h64_rtscene_max_color_width(data, size);
        o.scale = w > 640 ? 3 : w > 320 ? 2 : 1;
    }
    o.texFilter = 0;   // the N64's 3-point filter, as the RDP
    o.zoom = 100;
    h64_xenos_set_options(r, &o);
    sys->renderer = r;
    // Warm-up: the whole scene once, nothing written. A scene starts in the
    // middle of a game, where Xenos already knows each colour image's size;
    // met for the first time, an unusual one (Banjo-Tooie's 304 x 270, Rogue
    // Squadron's 512 x 448) is drawn at the height guessed from its width for
    // that frame (in the game: one frame when the size changes), and the
    // measured frame came back squashed (render tests: 46-62 % instead of 3-5).
    while (h64_rtscene_play_frame(&sc, sys, r) == 1)
        h64_xenos_present_vi(r, sys->vi.regs);
    h64_rtscene_open(&sc, data, size);
    while ((rc = h64_rtscene_play_frame(&sc, sys, r)) == 1)
    {
        u32 f = sc.frame - 1;
        // The present resolves the image being drawn; a copy back reads only
        // resolved images (before it, the file held the previous frame's).
        h64_xenos_present_vi(r, sys->vi.regs);
        if (sc.commands != lastCommands)
        {
            u32 addr[16], bytes[16], n;
            n = h64_xenos_copy_back_all(r, addr, bytes, 16);
            if (n)
            {
                _snprintf(path, sizeof(path), "%s\\%s\\xenos_f%u.img", dir, name, f);
                path[sizeof(path) - 1] = 0;
                if (!WriteImages(path, sys->rdram, addr, bytes, n)) written++;
            }
            lastCommands = sc.commands;
        }
        frames++;
    }
    if (rc < 0) H64_WARN("[rtest] %s: bad scene data at byte %u", name, sc.pos);
    H64_INFO("[rtest] %s: %u frames, %u commands, %u HLE triangles, %u frames written (scale %d)", name, frames,
             sc.commands, sc.triangles, written, o.scale);
    sys->renderer = NULL;
    h64_xenos_free(r);
    h64_rdp_free(sys);
    free(sys->rdram);
    free(sys);
    free(data);
    return 1;
}

int XbRenderTest(IDirect3DDevice9 *dev, const char *folder, int scale)
{
    char pattern[300];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int cases = 0;
    _snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    pattern[sizeof(pattern) - 1] = 0;
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) { H64_ERROR("[rtest] no folder %s", folder); return 0; }
    memset(&s_prog, 0, sizeof(s_prog));
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') s_prog.total++;
    while (FindNextFileA(f, &fd));
    FindClose(f);
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
        {
            s_prog.index++;
            strncpy(s_prog.name, fd.cFileName, sizeof(s_prog.name) - 1);
            Progress(dev, "Drawing the scene with the Xenos renderer and reading the pictures back");
            cases += RunCase(dev, folder, fd.cFileName, scale);
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    s_prog.index = s_prog.total + 1;
    strcpy(s_prog.name, "All cases drawn");
    Progress(dev, "Writing the log, then back to the dashboard");
    H64_INFO("[rtest] done: %d cases", cases);
    return cases;
}
