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
enum { S_CPU = 0, S_HLE, S_AUDIO, S_SMOOTH, S_FPS };

static void settings_values(UiMenu *m, const Config *c)
{
    sprintf(m->value[S_CPU], "< %s >", strcmp(c->cpu, "interp") ? "Recompiler" : "Interpreter");
    sprintf(m->value[S_HLE], "< %s >", c->hle ? "HLE" : "LLE");
    sprintf(m->value[S_AUDIO], "< %d ms >", c->audioMs);
    sprintf(m->value[S_SMOOTH], "< %s >", c->smooth ? "On" : "Off");
    sprintf(m->value[S_FPS], "< %s >", c->showFps ? "On" : "Off");
}

void SettingsBuild(UiMenu *m, const Config *c, int perGame)
{
    UiMenuClear(m, perGame ? "Settings" : "Settings: all games", "A:Select|DPAD:Change|B:Back");
    UiMenuAdd(m, "CPU (next start)", "");
    UiMenuAdd(m, "RSP (next start)", "");
    UiMenuAdd(m, "Audio margin", "");
    UiMenuAdd(m, "Edge smoothing", "");
    UiMenuAdd(m, "Show FPS", "");
    UiMenuAdd(m, "Save for all games", "");
    if (perGame) UiMenuAdd(m, "Save for this game only", "");
    UiMenuAdd(m, "Back", "");
    settings_values(m, c);
}

int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    int changed = 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B) return SET_BACK;
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
        case S_SMOOTH: c->smooth = !c->smooth; break;
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
        if (m->sel == S_FPS + 1) return SET_SAVE_ALL;
        if (perGame && m->sel == S_FPS + 2) return SET_SAVE_GAME;
    }
    return SET_NONE;
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
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64"))) continue;
        e.file = std::string("game:\\roms\\") + fd.cFileName;
        e.shown = std::string(fd.cFileName, dot - fd.cFileName);
        if (e.shown.size() > 52) e.shown = e.shown.substr(0, 49) + "...";
        out->push_back(e);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    std::sort(out->begin(), out->end(), rom_less);
}

// Reads the first 4 KB of a ROM file (any dump order) into *rom. Returns the
// file size, or 0 when it is not an N64 ROM.
static u32 read_header(const char *path, H64Rom *rom)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    static u8 head[0x1000];
    DWORD got = 0, len;
    memset(rom, 0, sizeof(*rom));
    if (h == INVALID_HANDLE_VALUE) return 0;
    len = GetFileSize(h, NULL);
    ReadFile(h, head, sizeof(head), &got, NULL);
    CloseHandle(h);
    if (got < sizeof(head) || h64_rom_load(rom, head, got)) return 0;
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
    UiInputInit(&in);
    details[0] = 0;
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int n = (int)roms.size();
        if (down & XINPUT_GAMEPAD_BACK) return 0;
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

        dev->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG, 1.0f, 0);
        UiText(dev, 96, 50, 5, COL_EDGE, "HARISSA64 V2");
        UiText(dev, 96 + UiTextWidth(5, "HARISSA64 V2") + 24, 74, 2, COL_DIM, H64_VERSION_STRING);
        if (!n)
            UiText(dev, 96, listY, 3, COL_TEXT, "No ROM found.\n\nCopy .z64, .n64 or .v64 files\ninto game:\\roms\\");
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
        UiScreenFooter(dev, n ? "A:Play|LB/RB:Page|Y:Settings|BACK:Dashboard" : "Y:Settings|BACK:Dashboard");
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}
