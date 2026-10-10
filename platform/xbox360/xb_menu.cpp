// Harissa64 V2 - front-end menus (see xb_menu.h).
#include "xb_menu.h"

#include <math.h>
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

// Moves an animated value towards its target (exponential, ~70 ms), or
// jumps there when the animation restarts.
struct UiAnim
{
    float value, last;
    int live;
};

static float anim_step(UiAnim *a, float target, int restart)
{
    float now = UiTime(), dt = now - a->last;
    a->last = now;
    if (restart || !a->live || dt > 0.3f || dt < 0.0f)
    {
        a->value = target;
        a->live = 1;
        return target;
    }
    {
        float k = dt * 16.0f;
        if (k > 1.0f) k = 1.0f;
        a->value += (target - a->value) * k;
        if (fabsf(target - a->value) < 0.25f) a->value = target;
    }
    return a->value;
}

// The selection highlight of a list row: an accent-tinted pill with a bar on its left.
static void selection_bar(float x, float y, float w, float h)
{
    UiRoundRect(x, y, w, h, 12, D3DCOLOR_ARGB(78, 234, 76, 58), D3DCOLOR_ARGB(54, 234, 76, 58));
    UiRoundRing(x, y, w, h, 12, 1.0f, D3DCOLOR_ARGB(60, 255, 120, 96));
    UiRoundRect(x + 7, y + h * 0.25f, 4, h * 0.5f, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
}

// A paragraph wrapped to `w` (top at y); returns its height.
static float paragraph(int font, float size, float x, float y, float w, int maxLines, D3DCOLOR color, const char *text)
{
    char buf[1024];
    int lines = UiTextWrap(font, size, w, maxLines, text, buf, sizeof(buf));
    float lineH = size * 1.35f;
    UiText(font, size, x, y + lineH * 0.5f, color, buf, UI_LEFT);
    return lines * lineH;
}

// A small section title in capitals, in the accent colour.
static void heading(float x, float cy, const char *text)
{
    UiText(UI_BOLD, 15, x, cy, UI_COL_ACCENT_HI, text, UI_LEFT);
}

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

// A value shown "< 250 ms >" (DPAD changes it): drawn as the text between two
// chevrons. Returns the left end.
static float draw_value(float right, float cy, const char *value, int selected)
{
    size_t n = strlen(value);
    D3DCOLOR text = selected ? UI_COL_TEXT : UI_COL_TEXT2;
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[32];
        float cw = UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON), x = right;
        D3DCOLOR chev = selected ? UI_COL_ACCENT_HI : UI_COL_TEXT3;
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_RCHEVRON, UI_RIGHT);
        x -= cw + 10;
        x -= UiText(selected ? UI_BOLD : UI_REGULAR, 21, x, cy, text, inner, UI_RIGHT);
        x -= 10;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_LCHEVRON, UI_RIGHT);
        return x - cw;
    }
    return right - UiText(UI_REGULAR, 21, right, cy, text, value, UI_RIGHT);
}

void UiMenuDraw(const UiMenu *m)
{
    static UiAnim sel;
    static char lastTitle[64];
    static int lastCount;
    const float w = 660, rowH = 48, head = 100, foot = 78;
    float h = head + m->count * rowH + 18 + foot, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, sy;
    int i, restart = strcmp(lastTitle, m->title) != 0 || lastCount != m->count;
    strncpy(lastTitle, m->title, sizeof(lastTitle) - 1);
    lastCount = m->count;
    UiDim();
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, m->title, UI_LEFT);
    UiRoundRect(x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    sy = anim_step(&sel, y + head + m->sel * rowH, restart);
    if (m->count) selection_bar(x + 18, sy + 3, w - 36, rowH - 6);
    for (i = 0; i < m->count; i++)
    {
        float cy = y + head + i * rowH + rowH * 0.5f;
        int s = i == m->sel;
        UiText(s ? UI_BOLD : UI_REGULAR, 22, x + 44, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_LEFT);
        if (m->value[i][0]) draw_value(x + w - 40, cy, m->value[i], s);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
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
        const float w = 640, h = 220, x = (UI_WIDTH - w) / 2, y = (UI_HEIGHT - h) / 2;
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiPanel(x, y, w, h, 22);
        UiText(UI_BOLD, 30, x + 48, y + 70, UI_COL_TEXT, "Quit to the dashboard?", UI_LEFT);
        UiFooter(x + 48, y + h - 50, "A:Quit|B:Stay");
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- About ----
// The credits panel: from the ROM browser (X) and the in-game menu.
void AboutDraw(void)
{
    const float x = 150, y = 46, w = 980, h = 628, cx = x + 52, col2 = x + 560;
    float ty;
    UiDim();
    UiPanel(x, y, w, h, 24);
    UiBrand(cx, y + 72, 44);
    UiText(UI_REGULAR, 18, x + w - 52, y + 62, UI_COL_TEXT2, "Version " H64_VERSION_STRING, UI_RIGHT);
    UiText(UI_REGULAR, 16, x + w - 52, y + 88, UI_COL_TEXT3, "Built " __DATE__, UI_RIGHT);
    UiText(UI_REGULAR, 24, cx, y + 146, UI_COL_TEXT, "A Nintendo 64 emulator for the Xbox 360", UI_LEFT);
    {
        float a = UiText(UI_REGULAR, 20, cx, y + 184, UI_COL_TEXT2, "Created by ", UI_LEFT);
        UiText(UI_BOLD, 20, cx + a, y + 184, UI_COL_TEXT, "Mohamed Aymen (kernel64)", UI_LEFT);
    }
    UiText(UI_REGULAR, 18, cx, y + 214, UI_COL_TEXT3, "github.com/kernel64/Harissa64", UI_LEFT);
    UiRect(x + 32, y + 246, w - 64, 1, UI_COL_LINE);

    ty = y + 280;
    heading(cx, ty, "ENGINE");
    paragraph(UI_REGULAR, 19, cx, ty + 16, 460, 6, UI_COL_TEXT2,
              "MIPS R4300i to PowerPC dynamic recompiler.\nRSP: high-level graphics and audio, low-level interpreter.\n"
              "RDP on the Xenos GPU, software RDP for reference.");
    heading(col2, ty, "ACKNOWLEDGEMENTS");
    ty += 16 + paragraph(UI_REGULAR, 19, col2, ty + 16, 368, 6, UI_COL_TEXT2,
                         "Thanks to the ares, ParaLLEl-RDP, mupen64plus, GLideN64, libdragon and zlib projects, "
                         "and to Timothy Lottes and AMD for their display filters.");
    UiText(UI_REGULAR, 17, col2, ty + 18, UI_COL_TEXT3, "Details and licences: THIRD_PARTY.md", UI_LEFT);

    ty = y + 474;
    heading(cx, ty, "LICENCE");
    paragraph(UI_REGULAR, 17, cx, ty + 14, w - 104, 4, UI_COL_TEXT3,
              "Free software under the GNU GPL v2. Interface font: Inter (SIL Open Font License).\n"
              "Nintendo 64 is a trademark of Nintendo; this project is not affiliated with Nintendo. "
              "Play only games dumped from cartridges you own.");
    UiRect(x + 32, y + h - 72, w - 64, 1, UI_COL_LINE);
    UiFooter(cx, y + h - 36, "B:Back");
}

void AboutScreen(IDirect3DDevice9 *dev)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_BACK)) return;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        AboutDraw();
        UiEnd();
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

// The header of one ROM, for the details card.
struct RomInfo
{
    int ok;
    char name[24], code[8], size[16], save[48], folder[64];
};

static void rom_details(const char *path, RomInfo *info)
{
    H64Rom rom;
    u32 len = read_header(path, &rom);
    int pak, type;
    memset(info, 0, sizeof(*info));
    if (!len) return;
    info->ok = 1;
    type = h64_save_type_for_rom(&rom, &pak);
    strncpy(info->name, rom.name, sizeof(info->name) - 1);
    strncpy(info->code, rom.gameCode, sizeof(info->code) - 1);
    if (len >= (1u << 20)) _snprintf(info->size, sizeof(info->size), "%u MB", len >> 20);
    else _snprintf(info->size, sizeof(info->size), "%u KB", len >> 10);
    _snprintf(info->save, sizeof(info->save), "%s%s", h64_save_type_name(type), pak ? " + Controller Pak" : "");
    h64_save_folder_name(&rom, info->folder, (int)sizeof(info->folder));
    info->size[sizeof(info->size) - 1] = 0;
    info->save[sizeof(info->save) - 1] = 0;
    h64_rom_free(&rom);
}

// Up to two initials of a game's name ("Super Mario 64 (USA)" -> "SM") for its tile.
static void initials(const char *name, char *out)
{
    int n = 0, start = 1;
    for (; *name && n < 2; name++)
    {
        char c = *name;
        if (c == '(' || c == '[') break;
        if (start && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        start = c == ' ' || c == '-' || c == '_';
    }
    if (!n) out[n++] = '?';
    out[n] = 0;
}

// The tile above the details: a gradient picked from the name, with its initials.
static void game_tile(float x, float y, float w, float h, const char *name)
{
    static const D3DCOLOR tops[6] = { D3DCOLOR_ARGB(255, 240, 92, 66), D3DCOLOR_ARGB(255, 245, 150, 60), D3DCOLOR_ARGB(255, 150, 100, 235),
                                      D3DCOLOR_ARGB(255, 70, 140, 240), D3DCOLOR_ARGB(255, 40, 180, 170), D3DCOLOR_ARGB(255, 110, 190, 80) };
    static const D3DCOLOR bottoms[6] = { D3DCOLOR_ARGB(255, 150, 28, 30), D3DCOLOR_ARGB(255, 180, 70, 20), D3DCOLOR_ARGB(255, 80, 40, 150),
                                         D3DCOLOR_ARGB(255, 30, 70, 160), D3DCOLOR_ARGB(255, 16, 100, 110), D3DCOLOR_ARGB(255, 40, 110, 50) };
    char ini[4];
    u32 hsh = 2166136261u;
    const char *p;
    int k;
    for (p = name; *p; p++) hsh = (hsh ^ (u8)*p) * 16777619u;
    k = (int)(hsh % 6);
    initials(name, ini);
    UiShadow(x, y + 6, w, h, 16, 20, D3DCOLOR_ARGB(110, 0, 0, 0));
    UiRoundRect(x, y, w, h, 16, tops[k], bottoms[k]);
    UiRoundRing(x, y, w, h, 16, 1.0f, D3DCOLOR_ARGB(50, 255, 255, 255));
    // A soft sheen along the top half.
    UiRoundRect(x + 2, y + 2, w - 4, h * 0.5f, 14, D3DCOLOR_ARGB(34, 255, 255, 255), D3DCOLOR_ARGB(0, 255, 255, 255));
    UiTextShadowed(UI_BOLD, h * 0.46f, x + 28, y + h * 0.5f, D3DCOLOR_ARGB(240, 255, 255, 255), ini, UI_LEFT);
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
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// One label / value pair of the details card.
static void detail(float x, float y, float w, const char *label, const char *value)
{
    char fit[96];
    UiText(UI_BOLD, 14, x, y, UI_COL_TEXT3, label, UI_LEFT);
    UiTextFit(UI_REGULAR, 20, w, value, fit, sizeof(fit));
    UiText(UI_REGULAR, 20, x, y + 26, UI_COL_TEXT, fit, UI_LEFT);
}

static void browser_draw(const std::vector<RomEntry> &roms, int sel, int top, int rows, const RomInfo *info)
{
    static UiAnim selAnim;
    const float listX = UI_SAFE_X, listY = 128, listW = 700, rowH = 36, pad = 12;
    const float listH = rows * rowH + 2 * pad, cardX = 788, cardW = UI_WIDTH - UI_SAFE_X - cardX;
    int n = (int)roms.size(), i;
    char buf[160];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);
    if (n)
    {
        sprintf(buf, "%d / %d", sel + 1, n);
        UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
        UiText(UI_REGULAR, 15, UI_WIDTH - UI_SAFE_X, 88, UI_COL_TEXT3, "game:\\roms\\", UI_RIGHT);
    }
    UiPanel(listX, listY, listW, listH, 20);
    if (!n)
    {
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 30, listX + listW * 0.5f, cy - 50, UI_COL_TEXT, "No ROM found", UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy, UI_COL_TEXT2, "Copy .z64, .n64, .v64 or .zip files", UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy + 32, UI_COL_TEXT2, "into game:\\roms\\", UI_CENTER);
    }
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0), textW = listW - 72;
        selection_bar(listX + 10, sy + 1, listW - (n > rows ? 36 : 20), rowH - 2);
        for (i = 0; i < rows && top + i < n; i++)
        {
            float cy = listY + pad + i * rowH + rowH * 0.5f;
            int s = top + i == sel;
            UiTextFit(s ? UI_BOLD : UI_REGULAR, 21, textW, roms[top + i].shown.c_str(), buf, sizeof(buf));
            UiText(s ? UI_BOLD : UI_REGULAR, 21, listX + 30, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
        }
        if (n > rows)
        {
            // Scroll bar: the visible part of the list.
            float tx = listX + listW - 16, ty = listY + pad + 4, th = listH - 2 * pad - 8;
            float hh = th * rows / n, yy;
            if (hh < 28) hh = 28;
            yy = ty + (th - hh) * top / (float)(n - rows);
            UiRoundRect(tx, ty, 4, th, 2, D3DCOLOR_ARGB(20, 255, 255, 255), D3DCOLOR_ARGB(20, 255, 255, 255));
            UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
        }

        // Details card.
        UiPanel(cardX, listY, cardW, listH, 20);
        game_tile(cardX + 20, listY + 20, cardW - 40, 118, roms[sel].shown.c_str());
        {
            float ty = listY + 162;
            int lines = UiTextWrap(UI_BOLD, 25, cardW - 40, 2, roms[sel].shown.c_str(), buf, sizeof(buf));
            UiText(UI_BOLD, 25, cardX + 20, ty + 17, UI_COL_TEXT, buf, UI_LEFT);
            ty += lines * 25 * 1.35f + 6;
            if (!info->ok)
                UiText(UI_REGULAR, 19, cardX + 20, ty + 12, UI_COL_ACCENT_HI, "Not an N64 ROM", UI_LEFT);
            else
            {
                float half = (cardW - 40) * 0.5f, gy = listY + listH - 186;
                UiText(UI_REGULAR, 18, cardX + 20, ty + 12, UI_COL_TEXT2, info->name, UI_LEFT);
                UiRect(cardX + 20, gy - 24, cardW - 40, 1, UI_COL_LINE);
                detail(cardX + 20, gy, half - 12, "GAME CODE", info->code);
                detail(cardX + 20 + half, gy, half - 12, "SIZE", info->size);
                detail(cardX + 20, gy + 62, cardW - 40, "SAVE", info->save);
                detail(cardX + 20, gy + 124, cardW - 40, "SAVE FOLDER", info->folder);
            }
        }
    }
    UiScreenFooter(n ? "A:Play|LB/RB:Page|Y:Settings|X:About|BACK:Dashboard" : "Y:Settings|X:About|BACK:Dashboard");
}

int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    std::vector<RomEntry> roms;
    UiInput in;
    int sel = 0, top = 0, detailsFor = -1, i;
    RomInfo details;
    const int rows = 13;
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
    memset(&details, 0, sizeof(details));
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
            if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), &details); detailsFor = sel; }
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

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        browser_draw(roms, sel, top, rows, &details);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}
