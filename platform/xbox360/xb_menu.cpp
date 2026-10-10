// Harissa64 V2 - front-end menus (see xb_menu.h).
#include "xb_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../core/cart/h64_rom.h"
#include "../../core/cart/h64_save.h"
#include "../../core/cart/h64_zip.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "xb_ui.h"

#define COL_BG       D3DCOLOR_XRGB(14, 16, 26)
#define COL_PANEL    D3DCOLOR_XRGB(24, 28, 44)
#define COL_EDGE     D3DCOLOR_XRGB(220, 40, 30)
#define COL_SELECT   D3DCOLOR_XRGB(60, 70, 120)
#define COL_TEXT     D3DCOLOR_XRGB(230, 230, 230)
#define COL_DIM      D3DCOLOR_XRGB(140, 140, 160)
#define COL_VALUE    D3DCOLOR_XRGB(255, 220, 60)

// ---- Generic list panel ----
void UiMenuClear(UiMenu *m, const char *title, const char *footer)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->title, title, sizeof(m->title) - 1);
    strncpy(m->footer, footer, sizeof(m->footer) - 1);
}

int UiMenuAdd(UiMenu *m, const char *label, const char *value)
{
    if (m->count >= UI_MENU_MAX) return -1;
    strncpy(m->label[m->count], label, sizeof(m->label[0]) - 1);
    strncpy(m->value[m->count], value ? value : "", sizeof(m->value[0]) - 1);
    return m->count++;
}

void UiMenuDraw(IDirect3DDevice9 *dev, const UiMenu *m)
{
    const int w = 760, rowH = 40, x = (UI_WIDTH - w) / 2;
    int h = 110 + m->count * rowH + 64, y = (UI_HEIGHT - h) / 2, i;
    UiRect(dev, x - 4, y - 4, w + 8, h + 8, COL_EDGE);
    UiRect(dev, x, y, w, h, COL_PANEL);
    UiText(dev, x + 30, y + 24, 4, COL_TEXT, m->title);
    for (i = 0; i < m->count; i++)
    {
        int ry = y + 96 + i * rowH;
        if (i == m->sel) UiRect(dev, x + 16, ry - 8, w - 32, rowH - 4, COL_SELECT);
        UiText(dev, x + 40, ry, 3, COL_TEXT, m->label[i]);
        if (m->value[i][0])
            UiText(dev, x + w - 40 - UiTextWidth(3, m->value[i]), ry, 3, COL_VALUE, m->value[i]);
    }
    UiRect(dev, x + 16, y + h - 58, w - 32, 2, COL_EDGE);
    UiFooter(dev, x + 30, y + h - 30, m->footer);
}

void UiMenuNavigate(UiMenu *m, WORD down)
{
    if (!m->count) return;
    if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = (m->sel + m->count - 1) % m->count;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) m->sel = (m->sel + 1) % m->count;
}

// ---- Settings ----
// Main page: CPU, RSP, audio, FPS, the Graphics page, save, back. The Graphics
// page (resolution, aspect, texture filter, smoothing, sharpening, blur, screen effect) is
// shown in the same UiMenu; its Back returns to the main page.
enum { S_CPU = 0, S_HLE, S_AUDIO, S_FPS, S_GRAPHICS };
enum { G_PRESET = 0, G_RES, G_ASPECT, G_FILTER, G_SMOOTH, G_SHARPEN, G_BLUR, G_SCREEN, G_BACK };

// Graphics presets (the user's table): texture filter, smoothing, sharpening,
// blur, screen effect. Resolution and aspect stay as they are.
struct GfxPreset { const char *name; int texFilter, smooth, sharpen, blur, screen; };
static const GfxPreset s_presets[4] = {
    { "Original", 2, 0, 0, 0, 0 },        // nearest, nothing added
    { "N64 Enhanced", 0, 1, 1, 0, 0 },    // 3-point, light sharpening
    { "N64 Smooth", 0, 1, 1, 0, 5 },      // 3-point, light sharpening, light scanlines
    { "N64 CRT", 0, 0, 0, 0, 2 },         // 3-point, CRT: scanlines and glow
};

// The preset the settings match (-1: custom).
static int current_preset(const Config *c)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        const GfxPreset *p = &s_presets[i];
        if (c->texFilter == p->texFilter && c->smooth == p->smooth && c->sharpen == p->sharpen && c->blur == p->blur &&
            c->screen == p->screen)
            return i;
    }
    return -1;
}
static int s_graphicsPage;
static int s_perGame;

static const char *onoff(int v) { return v ? "On" : "Off"; }

static void settings_values(UiMenu *m, const Config *c)
{
    if (s_graphicsPage)
    {
        static const char *res[3] = { "Native 320x240", "x2 640x480", "x3 960x720" };
        static const char *asp[3] = { "4:3", "16:9 Widescreen", "16:9 Stretched" };
        static const char *filt[3] = { "N64 3-point", "Bilinear", "Nearest" };
        static const char *lvl[3] = { "Off", "Low", "High" };
        static const char *blr[3] = { "Off", "Soft", "Strong" };
        static const char *scr[6] = { "None", "Scanlines", "CRT", "CRT curved", "LCD grid", "Light scanlines" };
        int pr = current_preset(c);
        sprintf(m->value[G_PRESET], "< %s >", pr < 0 ? "Custom" : s_presets[pr].name);
        sprintf(m->value[G_RES], "< %s >", res[(c->resScale < 1 ? 1 : c->resScale > 3 ? 3 : c->resScale) - 1]);
        sprintf(m->value[G_ASPECT], "< %s >", asp[c->aspect < 0 || c->aspect > 2 ? 0 : c->aspect]);
        sprintf(m->value[G_FILTER], "< %s >", filt[c->texFilter < 0 || c->texFilter > 2 ? 1 : c->texFilter]);
        sprintf(m->value[G_SMOOTH], "< %s >", onoff(c->smooth));
        sprintf(m->value[G_SHARPEN], "< %s >", lvl[c->sharpen < 0 || c->sharpen > 2 ? 0 : c->sharpen]);
        sprintf(m->value[G_BLUR], "< %s >", blr[c->blur < 0 || c->blur > 2 ? 0 : c->blur]);
        sprintf(m->value[G_SCREEN], "< %s >", scr[c->screen < 0 || c->screen > 5 ? 0 : c->screen]);
        return;
    }
    sprintf(m->value[S_CPU], "< %s >", strcmp(c->cpu, "interp") ? "Recompiler" : "Interpreter");
    sprintf(m->value[S_HLE], "< %s >", c->hle ? "HLE" : "LLE");
    sprintf(m->value[S_AUDIO], "< %d ms >", c->audioMs);
    sprintf(m->value[S_FPS], "< %s >", onoff(c->showFps));
}

static void settings_main(UiMenu *m, const Config *c)
{
    s_graphicsPage = 0;
    UiMenuClear(m, s_perGame ? "Settings" : "Settings: all games", "A:Select|DPAD:Change|B:Back");
    UiMenuAdd(m, "CPU (next start)", "");
    UiMenuAdd(m, "RSP (next start)", "");
    UiMenuAdd(m, "Audio margin", "");
    UiMenuAdd(m, "Show FPS", "");
    UiMenuAdd(m, "Graphics", "");
    UiMenuAdd(m, "Save for all games", "");
    if (s_perGame) UiMenuAdd(m, "Save for this game only", "");
    UiMenuAdd(m, "Back", "");
    settings_values(m, c);
}

static void settings_graphics(UiMenu *m, const Config *c)
{
    s_graphicsPage = 1;
    UiMenuClear(m, "Graphics", "A:Select|DPAD:Change|B:Back");
    UiMenuAdd(m, "Preset", "");
    UiMenuAdd(m, "Resolution", "");
    UiMenuAdd(m, "Aspect ratio", "");
    UiMenuAdd(m, "Texture filter", "");
    UiMenuAdd(m, "Edge smoothing (FXAA)", "");
    UiMenuAdd(m, "Sharpen", "");
    UiMenuAdd(m, "Blur", "");
    UiMenuAdd(m, "Screen effect", "");
    UiMenuAdd(m, "Back", "");
    settings_values(m, c);
}

void SettingsBuild(UiMenu *m, const Config *c, int perGame)
{
    s_perGame = perGame;
    settings_main(m, c);
}

static int graphics_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if ((down & XINPUT_GAMEPAD_B) || (m->sel == G_BACK && (down & XINPUT_GAMEPAD_A)))
    {
        settings_main(m, c);
        m->sel = S_GRAPHICS;
        return SET_NONE;
    }
    if (m->sel < G_BACK && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        switch (m->sel)
        {
        case G_PRESET:
        {
            int pr = current_preset(c);
            const GfxPreset *p;
            pr = pr < 0 ? (dir > 0 ? 0 : 3) : (pr + dir + 4) % 4;
            p = &s_presets[pr];
            c->texFilter = p->texFilter;
            c->smooth = p->smooth;
            c->sharpen = p->sharpen;
            c->blur = p->blur;
            c->screen = p->screen;
            break;
        }
        case G_RES: c->resScale = (c->resScale - 1 + dir + 3) % 3 + 1; break;
        case G_ASPECT: c->aspect = (c->aspect + dir + 3) % 3; break;
        case G_FILTER: c->texFilter = (c->texFilter + dir + 3) % 3; break;
        case G_SMOOTH: c->smooth = !c->smooth; break;
        case G_SHARPEN: c->sharpen = (c->sharpen + dir + 3) % 3; if (c->sharpen) c->blur = 0; break;
        case G_BLUR: c->blur = (c->blur + dir + 3) % 3; if (c->blur) c->sharpen = 0; break;
        case G_SCREEN: c->screen = (c->screen + dir + 6) % 6; break;
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down)
{
    int dir, changed = 0;
    s_perGame = perGame;
    if (s_graphicsPage) return graphics_input(m, c, down);
    dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B) return SET_BACK;
    if (m->sel == S_GRAPHICS && (down & XINPUT_GAMEPAD_A))
    {
        settings_graphics(m, c);
        return SET_NONE;
    }
    if (m->sel <= S_FPS && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        switch (m->sel)
        {
        case S_CPU: strcpy(c->cpu, strcmp(c->cpu, "interp") ? "interp" : "dynarec"); break;
        case S_HLE: c->hle = !c->hle; break;
        case S_AUDIO:
            c->audioMs += dir * 50;
            if (c->audioMs < 50) c->audioMs = 50;
            if (c->audioMs > 500) c->audioMs = 500;
            break;
        case S_FPS: c->showFps = !c->showFps; break;
        }
        changed = 1;
    }
    settings_values(m, c);
    if (changed) return SET_CHANGED;
    if (down & XINPUT_GAMEPAD_A)
    {
        int last = m->count - 1;
        if (m->sel == last) return SET_BACK;
        if (m->sel == S_GRAPHICS + 1) return SET_SAVE_ALL;
        if (perGame && m->sel == S_GRAPHICS + 2) return SET_SAVE_GAME;
    }
    return SET_NONE;
}

// ---- Leaving ----
// "Quit to the dashboard?": A quits, B or BACK stays.
static int confirm_quit(IDirect3DDevice9 *dev)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        const int w = 640, h = 220, x = (1280 - w) / 2, y = (720 - h) / 2;
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 9, 16), 1.0f, 0);
        UiRect(dev, x - 4, y - 4, w + 8, h + 8, COL_EDGE);
        UiRect(dev, x, y, w, h, COL_BG);
        UiText(dev, x + 40, y + 40, 3, COL_TEXT, "Quit to the dashboard?");
        UiFooter(dev, x + 40, y + h - 50, "A:Quit|B:Stay");
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- About ----
// The credits panel: from the ROM browser (X) and the in-game menu.
void AboutDraw(IDirect3DDevice9 *dev)
{
    const int x = 140, y = 40, w = 1000, h = 640;
    int tx, ty;
    UiRect(dev, x - 4, y - 4, w + 8, h + 8, COL_EDGE);
    UiRect(dev, x, y, w, h, COL_BG);
    tx = x + 40 + UiLogo(dev, x + 40, y + 30, 4) + 20;
    UiText(dev, tx, y + 42, 5, COL_EDGE, "HARISSA64 V2");
    UiText(dev, tx + UiTextWidth(5, "HARISSA64 V2") + 24, y + 66, 2, COL_DIM, H64_VERSION_STRING);
    ty = y + 120;
    UiText(dev, x + 40, ty, 2, COL_TEXT, "A Nintendo 64 emulator for the Xbox 360");
    UiText(dev, x + 40, ty + 28, 2, COL_VALUE, "Created by Mohamed Aymen (kernel64)");
    UiText(dev, x + 40, ty + 56, 2, COL_DIM, "github.com/kernel64/Harissa64   -   built " __DATE__);
    ty += 104;
    UiText(dev, x + 40, ty, 2, COL_EDGE, "ENGINE");
    UiText(dev, x + 40, ty + 26, 2, COL_TEXT, "MIPS R4300i to PowerPC dynamic recompiler");
    UiText(dev, x + 40, ty + 50, 2, COL_TEXT, "RSP: high-level graphics and audio, low-level interpreter");
    UiText(dev, x + 40, ty + 74, 2, COL_TEXT, "RDP on the Xenos GPU, software RDP for reference");
    ty += 112;
    UiText(dev, x + 40, ty, 2, COL_EDGE, "ACKNOWLEDGEMENTS");
    UiText(dev, x + 40, ty + 26, 2, COL_TEXT, "Thanks to the ares, ParaLLEl-RDP, mupen64plus,");
    UiText(dev, x + 40, ty + 50, 2, COL_TEXT, "GLideN64, libdragon and zlib projects, and to");
    UiText(dev, x + 40, ty + 74, 2, COL_TEXT, "Timothy Lottes and AMD for their display filters.");
    UiText(dev, x + 40, ty + 98, 2, COL_DIM, "Details and licences: THIRD_PARTY.md");
    ty += 136;
    UiText(dev, x + 40, ty, 2, COL_DIM, "Free software under the GNU GPL v2.");
    UiText(dev, x + 40, ty + 24, 2, COL_DIM, "Nintendo 64 is a trademark of Nintendo; this");
    UiText(dev, x + 40, ty + 48, 2, COL_DIM, "project is not affiliated with Nintendo. Play");
    UiText(dev, x + 40, ty + 72, 2, COL_DIM, "only games dumped from cartridges you own.");
    UiFooter(dev, x + 40, y + h - 30, "B:Back");
}

void AboutScreen(IDirect3DDevice9 *dev)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_BACK)) return;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 9, 16), 1.0f, 0);
        AboutDraw(dev);
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- ROM browser ----
struct RomEntry { std::string file, shown; };
static bool rom_less(const RomEntry &a, const RomEntry &b) { return _stricmp(a.shown.c_str(), b.shown.c_str()) < 0; }

static void scan_roms(std::vector<RomEntry> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("game:\\roms\\*", &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const char *dot = strrchr(fd.cFileName, '.');
        RomEntry e;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64") && _stricmp(dot, ".zip"))) continue;
        e.file = std::string("game:\\roms\\") + fd.cFileName;
        e.shown = std::string(fd.cFileName, dot - fd.cFileName);
        if (e.shown.size() > 52) e.shown = e.shown.substr(0, 49) + "...";
        out->push_back(e);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    std::sort(out->begin(), out->end(), rom_less);
}

// ---- ROM files (plain or zipped) ----
static u32 handle_read(void *user, u32 offset, void *buf, u32 len)
{
    DWORD got = 0;
    if (SetFilePointer((HANDLE)user, (LONG)offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return 0;
    if (!ReadFile((HANDLE)user, buf, len, &got, NULL)) return 0;
    return got;
}

// Opens `path`; for a zip, finds the ROM inside (*isZip set). Returns the
// handle (INVALID_HANDLE_VALUE on failure) and the ROM size in *size.
static HANDLE rom_open(const char *path, H64ZipReader *r, H64ZipEntry *e, int *isZip, u32 *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    u8 magic[4];
    *isZip = 0;
    if (h == INVALID_HANDLE_VALUE) return h;
    r->user = h;
    r->size = GetFileSize(h, NULL);
    r->read = handle_read;
    *size = r->size;
    if (handle_read(h, 0, magic, 4) == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4)
    {
        if (h64_zip_find_rom(r, e)) { H64_WARN("[rom] no N64 ROM in %s", path); CloseHandle(h); return INVALID_HANDLE_VALUE; }
        *isZip = 1;
        *size = e->size;
    }
    return h;
}

u8 *RomFileLoad(const char *path, u32 *size)
{
    H64ZipReader r;
    H64ZipEntry e;
    int zip, ok;
    u32 len;
    HANDLE h = rom_open(path, &r, &e, &zip, &len);
    u8 *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    buf = (u8 *)malloc(len ? len : 1);
    if (!buf) { CloseHandle(h); return NULL; }
    ok = zip ? h64_zip_extract(&r, &e, buf, len) == 0 : handle_read(h, 0, buf, len) == len;
    CloseHandle(h);
    if (!ok) { H64_WARN("[rom] cannot read %s%s", path, zip ? " (bad zip entry)" : ""); free(buf); return NULL; }
    if (zip) H64_INFO("[rom] %s: %s (%u bytes)", path, e.name, len);
    *size = len;
    return buf;
}

// Reads the first 4 KB of a ROM (any dump order, plain or zipped) into *rom.
// Returns the ROM size, or 0 when it is not an N64 ROM.
static u32 read_header(const char *path, H64Rom *rom)
{
    static u8 head[0x1000];
    H64ZipReader r;
    H64ZipEntry e;
    int zip, ok;
    u32 len;
    HANDLE h = rom_open(path, &r, &e, &zip, &len);
    memset(rom, 0, sizeof(*rom));
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = len >= sizeof(head) && (zip ? h64_zip_extract(&r, &e, head, sizeof(head)) == 0 : handle_read(h, 0, head, sizeof(head)) == sizeof(head));
    CloseHandle(h);
    if (!ok || h64_rom_load(rom, head, sizeof(head))) return 0;
    return len;
}

int RomFolderName(const char *path, char *out, size_t size)
{
    H64Rom rom;
    if (!read_header(path, &rom)) return 0;
    h64_save_folder_name(&rom, out, (int)size);
    h64_rom_free(&rom);
    return 1;
}

// The header of one ROM, for the details line.
static void rom_details(const char *path, char *out, size_t size)
{
    H64Rom rom;
    u32 len = read_header(path, &rom);
    int pak, type;
    if (!len) { _snprintf(out, size, "Not an N64 ROM"); out[size - 1] = 0; return; }
    type = h64_save_type_for_rom(&rom, &pak);
    _snprintf(out, size, "%s  %s  %u MB\nSave: %s%s", rom.name, rom.gameCode, len >> 20, h64_save_type_name(type),
              pak ? ", Controller Pak" : "");
    out[size - 1] = 0;
    h64_rom_free(&rom);
}

static void settings_loop(IDirect3DDevice9 *dev, Config *c, const char *settingsPath)
{
    UiMenu m;
    UiInput in;
    UiInputInit(&in);
    SettingsBuild(&m, c, 0);
    for (;;)
    {
        int r = SettingsInput(&m, c, 0, UiInputPoll(&in));
        if (r == SET_BACK) return;
        if (r == SET_SAVE_ALL)
        {
            H64_INFO("[menu] settings for all games %s", ConfigWriteMenuKeys(c, settingsPath, 1) ? "saved" : "NOT saved");
            return;
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG, 1.0f, 0);
        UiMenuDraw(dev, &m);
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    std::vector<RomEntry> roms;
    UiInput in;
    int sel = 0, top = 0, detailsFor = -1, i;
    char details[160];
    const int rows = 13, rowH = 30, listY = 140;
    scan_roms(&roms);
    for (i = 0; i < (int)roms.size(); i++)
        if (!_stricmp(roms[i].file.c_str(), c->lastRom)) sel = i;
    if (c->autoStart > 0 && !roms.empty())
    {
        // Remote checks: the last selection, then the next ROMs in the list.
        sel = (sel + c->autoIndex) % (int)roms.size();
        strncpy(path, roms[sel].file.c_str(), pathSize - 1);
        path[pathSize - 1] = 0;
        H64_INFO("[menu] autostart %d: %s", c->autoIndex + 1, path);
        return 1;
    }
    UiInputInit(&in);
    details[0] = 0;
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int n = (int)roms.size();
        if ((down & XINPUT_GAMEPAD_BACK) && confirm_quit(dev)) return 0;
        if (n)
        {
            if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
            if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel > rows ? sel - rows : 0;
            if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + rows < n ? sel + rows : n - 1;
            if (down & XINPUT_GAMEPAD_A)
            {
                strncpy(path, roms[sel].file.c_str(), pathSize - 1);
                path[pathSize - 1] = 0;
                strncpy(c->lastRom, path, sizeof(c->lastRom) - 1);
                ConfigWriteMenuKeys(c, settingsPath, 1);
                return 1;
            }
            if (sel < top) top = sel;
            if (sel >= top + rows) top = sel - rows + 1;
            if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), details, sizeof(details)); detailsFor = sel; }
        }
        if (down & XINPUT_GAMEPAD_Y)
        {
            settings_loop(dev, c, settingsPath);
            UiInputInit(&in);
        }
        if (down & XINPUT_GAMEPAD_X)
        {
            AboutScreen(dev);
            UiInputInit(&in);
        }

        dev->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG, 1.0f, 0);
        {
            int tx = 96 + UiLogo(dev, 96, 38, 4) + 20;   // 64 px pepper, centred on the 40 px title
            UiText(dev, tx, 50, 5, COL_EDGE, "HARISSA64 V2");
            UiText(dev, tx + UiTextWidth(5, "HARISSA64 V2") + 24, 74, 2, COL_DIM, H64_VERSION_STRING);
        }
        if (!n)
            UiText(dev, 96, listY, 3, COL_TEXT, "No ROM found.\n\nCopy .z64, .n64, .v64 or .zip files\ninto game:\\roms\\");
        for (i = 0; i < rows && top + i < n; i++)
        {
            int y = listY + i * rowH;
            if (top + i == sel) UiRect(dev, 80, y - 6, 1120, rowH - 2, COL_SELECT);
            UiText(dev, 96, y, 2, top + i == sel ? COL_TEXT : COL_DIM, roms[top + i].shown.c_str());
        }
        if (n)
        {
            char count[32];
            sprintf(count, "%d / %d", sel + 1, n);
            UiText(dev, 1184 - UiTextWidth(2, count), 110, 2, COL_DIM, count);
            UiText(dev, 96, listY + rows * rowH + 16, 2, COL_VALUE, details);
        }
        UiScreenFooter(dev, n ? "A:Play|LB/RB:Page|Y:Settings|X:About|BACK:Dashboard" : "Y:Settings|X:About|BACK:Dashboard");
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}
