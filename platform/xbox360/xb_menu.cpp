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
#include "../../core/common/h64_fenv.h"
#include "../../core/common/h64_png.h"
#include "xb_art.h"
#include "xb_drives.h"
#include "xb_flags.h"
#include "xb_lang.h"
#include "xb_ui.h"
#include "xb_update.h"

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

// The selection highlight of a list row: an accent-tinted pill with a bar on
// its left (on its right in a right-to-left menu).
static void selection_bar_side(float x, float y, float w, float h, int right)
{
    UiRoundRect(x, y, w, h, 12, D3DCOLOR_ARGB(78, 234, 76, 58), D3DCOLOR_ARGB(54, 234, 76, 58));
    UiRoundRing(x, y, w, h, 12, 1.0f, D3DCOLOR_ARGB(60, 255, 120, 96));
    UiRoundRect(right ? x + w - 11 : x + 7, y + h * 0.25f, 4, h * 0.5f, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
}

static void selection_bar(float x, float y, float w, float h) { selection_bar_side(x, y, w, h, 0); }

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
        char inner[64];
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

// The width draw_value takes.
static float value_width(const char *value, int selected)
{
    size_t n = strlen(value);
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        return 2 * (UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON) + 10) + UiTextWidth(selected ? UI_BOLD : UI_REGULAR, 21, inner);
    }
    return UiTextWidth(UI_REGULAR, 21, value);
}

static void controls_draw(const UiMenu *m);

void UiMenuDraw(const UiMenu *m)
{
    static UiAnim sel;
    static char lastTitle[128];
    static int lastCount;
    const float w = 660, head = 100, foot = 78;
    // Rows shrink a little when a page has many (the Graphics page: 11), so
    // the panel stays on screen with a margin.
    float rowH = m->count > 0 && (UI_HEIGHT - 40 - head - 18 - foot) / m->count < 48 ? (UI_HEIGHT - 40 - head - 18 - foot) / m->count : 48;
    float h = head + m->count * rowH + 18 + foot, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, sy;
    int i, restart = strcmp(lastTitle, m->title) != 0 || lastCount != m->count, rtl = LangRtl();
    if (m->page == UI_PAGE_CONTROLS) { controls_draw(m); lastTitle[0] = 0; return; }
    strncpy(lastTitle, m->title, sizeof(lastTitle) - 1);
    lastCount = m->count;
    UiDim();
    UiPanel(x, y, w, h, 22);
    // Right to left (Arabic): title and labels on the right, values on the left.
    UiText(UI_BOLD, 30, rtl ? x + w - 40 : x + 40, y + 50, UI_COL_TEXT, m->title, rtl ? UI_RIGHT : UI_LEFT);
    UiRoundRect(rtl ? x + w - 70 : x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    sy = anim_step(&sel, y + head + m->sel * rowH, restart);
    if (m->count) selection_bar_side(x + 18, sy + 3, w - 36, rowH - 6, rtl);
    for (i = 0; i < m->count; i++)
    {
        float cy = y + head + i * rowH + rowH * 0.5f;
        int s = i == m->sel;
        if (rtl)
        {
            UiText(s ? UI_BOLD : UI_REGULAR, 22, x + w - 44, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_RIGHT);
            if (m->value[i][0]) draw_value(x + 40 + value_width(m->value[i], s), cy, m->value[i], s);
            continue;
        }
        UiText(s ? UI_BOLD : UI_REGULAR, 22, x + 44, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_LEFT);
        if (m->value[i][0]) draw_value(x + w - 40, cy, m->value[i], s);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(rtl ? x + w - 40 - UiFooterWidth(m->footer) : x + 40, y + h - foot * 0.5f, m->footer);
}

void UiMenuNavigate(UiMenu *m, WORD down)
{
    if (!m->count) return;
    if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = (m->sel + m->count - 1) % m->count;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) m->sel = (m->sel + 1) % m->count;
}

// ---- Settings ----
// Main page: CPU, RSP, audio, FPS, the Graphics and Controls pages, save, back. The Graphics
// page (resolution, texture filter, smoothing, sharpening, blur, screen effect) is
// shown in the same UiMenu; its Back returns to the main page.
enum { S_CPU = 0, S_HLE, S_AUDIO, S_FPS, S_GRAPHICS, S_CONTROLS, S_LANGUAGE };
enum { G_PRESET = 0, G_RES, G_STRETCH, G_ZOOM, G_POSX, G_POSY, G_BRIGHT, G_VSYNC, G_DEFLICKER, G_FILTER, G_SMOOTH, G_SHARPEN, G_BLUR,
       G_SCREEN, G_BACK };

// Graphics presets (the user's table): texture filter, smoothing, sharpening,
// blur, screen effect. The resolution stays as it is.
struct GfxPreset { int name; int texFilter, smooth, sharpen, blur, screen; };
static const GfxPreset s_presets[4] = {
    { T_PRESET_ORIGINAL, 2, 0, 0, 0, 0 },   // nearest, nothing added
    { T_PRESET_ENHANCED, 0, 1, 1, 0, 0 },   // 3-point, light sharpening
    { T_PRESET_SMOOTH, 0, 1, 1, 0, 5 },     // 3-point, light sharpening, light scanlines
    { T_PRESET_CRT, 0, 0, 0, 0, 2 },        // 3-point, CRT: scanlines and glow
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
static int s_browser;   // opened from the ROM list: the box art rows are shown
static char s_langPath[160];   // where a language change is kept (SettingsSetLangFile)
#define S_ART (S_LANGUAGE + 1)       // "Box art downloads" (ROM list only)
#define S_ART_ALL (S_LANGUAGE + 2)   // "Get box art for all games" (ROM list only)
#define S_SAVE_ALL (s_browser ? S_LANGUAGE + 3 : S_LANGUAGE + 1)

static const char *onoff(int v) { return Tr(v ? T_ON : T_OFF); }

void SettingsSetLangFile(const char *path)
{
    strncpy(s_langPath, path ? path : "", sizeof(s_langPath) - 1);
    s_langPath[sizeof(s_langPath) - 1] = 0;
}

// The menus in another language, kept in the settings file at once.
static void lang_change(Config *c, int lang)
{
    LangSet(lang);
    strcpy(c->lang, LangCode(lang));
    if (s_langPath[0] && !ConfigWriteLang(s_langPath, c->lang)) H64_WARN("[lang] cannot write %s", s_langPath);
    H64_INFO("[lang] menus in %s", c->lang);
}

static void settings_values(UiMenu *m, const Config *c)
{
    if (s_graphicsPage)
    {
        const char *res[4] = { Tr(T_RES_NATIVE), "x2 640x480", "x3 960x720", "HD 1280x720 (16:9)" };
        const char *filt[3] = { Tr(T_FILTER_3POINT), Tr(T_FILTER_BILINEAR), Tr(T_FILTER_NEAREST) };
        const char *lvl[3] = { Tr(T_OFF), Tr(T_LOW), Tr(T_HIGH) };
        const char *blr[3] = { Tr(T_OFF), Tr(T_SOFT), Tr(T_STRONG) };
        const char *scr[6] = { Tr(T_SCREEN_NONE), Tr(T_SCANLINES), Tr(T_CRT), Tr(T_CRT_CURVED), Tr(T_LCD_GRID), Tr(T_LIGHT_SCANLINES) };
        int pr = current_preset(c);
        sprintf(m->value[G_PRESET], "< %s >", pr < 0 ? Tr(T_CUSTOM) : Tr(s_presets[pr].name));
        sprintf(m->value[G_RES], "< %s >", res[(c->resScale < 1 ? 1 : c->resScale > 4 ? 4 : c->resScale) - 1]);
        sprintf(m->value[G_FILTER], "< %s >", filt[c->texFilter < 0 || c->texFilter > 2 ? 1 : c->texFilter]);
        sprintf(m->value[G_SMOOTH], "< %s >", onoff(c->smooth));
        sprintf(m->value[G_SHARPEN], "< %s >", lvl[c->sharpen < 0 || c->sharpen > 2 ? 0 : c->sharpen]);
        sprintf(m->value[G_BLUR], "< %s >", blr[c->blur < 0 || c->blur > 2 ? 0 : c->blur]);
        sprintf(m->value[G_SCREEN], "< %s >", scr[c->screen < 0 || c->screen > 5 ? 0 : c->screen]);
        {
            const char *br[3] = { Tr(T_NORMAL), Tr(T_LIGHTER), Tr(T_MUCH_LIGHTER) };
            sprintf(m->value[G_BRIGHT], "< %s >", br[c->brightness < 0 || c->brightness > 2 ? 0 : c->brightness]);
            sprintf(m->value[G_STRETCH], "< %s >", onoff(c->stretch));
            sprintf(m->value[G_VSYNC], "< %s >", c->vsync == 2 ? Tr(T_VSYNC_STRICT) : onoff(c->vsync));
            sprintf(m->value[G_DEFLICKER], "< %s >", onoff(c->deflicker));
            sprintf(m->value[G_ZOOM], "< %d%% >", c->zoom);
            sprintf(m->value[G_POSX], "< %+d >", c->offsetX);
            sprintf(m->value[G_POSY], "< %+d >", c->offsetY);
        }
        return;
    }
    sprintf(m->value[S_CPU], "< %s >", strcmp(c->cpu, "interp") ? Tr(T_RECOMPILER) : Tr(T_INTERPRETER));
    sprintf(m->value[S_HLE], "< %s >", c->hle ? "HLE" : "LLE");
    sprintf(m->value[S_AUDIO], "< %d ms >", c->audioMs);
    {
        static const int fps[4] = { T_OFF, T_FPS_ONLY, T_FPS_CPU, T_FPS_ALL };
        sprintf(m->value[S_FPS], "< %s >", Tr(fps[c->showFps < 0 || c->showFps > 3 ? 0 : c->showFps]));
    }
    {
        int i, custom = 0;
        for (i = 0; i < 4; i++) custom |= !h64_padmap_is_default(&c->padMap[i]);
        strcpy(m->value[S_CONTROLS], custom ? Tr(T_CUSTOM) : Tr(T_DEFAULT));
    }
    sprintf(m->value[S_LANGUAGE], "< %s >", Tr(T_LANG_NAME));
    if (s_browser) sprintf(m->value[S_ART], "< %s >", onoff(c->artDownload));
}

static void settings_main(UiMenu *m, const Config *c)
{
    s_graphicsPage = 0;
    UiMenuClear(m, Tr(s_perGame ? T_SETTINGS : T_SETTINGS_ALL), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_CPU_NEXT), "");
    UiMenuAdd(m, Tr(T_RSP_NEXT), "");
    UiMenuAdd(m, Tr(T_AUDIO_MARGIN), "");
    UiMenuAdd(m, Tr(T_SHOW_FPS), "");
    UiMenuAdd(m, Tr(T_GRAPHICS), "");
    UiMenuAdd(m, Tr(T_CONTROLS), "");
    UiMenuAdd(m, Tr(T_LANGUAGE), "");
    if (s_browser)
    {
        UiMenuAdd(m, Tr(T_ART_DOWNLOADS), "");
        UiMenuAdd(m, Tr(T_ART_ALL), "");
    }
    UiMenuAdd(m, Tr(T_SAVE_ALL), "");
    if (s_perGame) UiMenuAdd(m, Tr(T_SAVE_GAME), "");
    UiMenuAdd(m, Tr(T_BACK), "");
    settings_values(m, c);
}

static void settings_graphics(UiMenu *m, const Config *c)
{
    s_graphicsPage = 1;
    UiMenuClear(m, Tr(T_GRAPHICS), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PRESET), "");
    UiMenuAdd(m, Tr(T_RESOLUTION), "");
    UiMenuAdd(m, Tr(T_STRETCH), "");
    UiMenuAdd(m, Tr(T_PIC_SIZE), "");
    UiMenuAdd(m, Tr(T_PIC_LR), "");
    UiMenuAdd(m, Tr(T_PIC_UD), "");
    UiMenuAdd(m, Tr(T_BRIGHTNESS), "");
    UiMenuAdd(m, Tr(T_VSYNC), "");
    UiMenuAdd(m, Tr(T_ANTIFLICKER), "");
    UiMenuAdd(m, Tr(T_TEXFILTER), "");
    UiMenuAdd(m, Tr(T_FXAA), "");
    UiMenuAdd(m, Tr(T_SHARPEN), "");
    UiMenuAdd(m, Tr(T_BLUR), "");
    UiMenuAdd(m, Tr(T_SCREEN_EFFECT), "");
    UiMenuAdd(m, Tr(T_BACK), "");
    settings_values(m, c);
}

// ---- Controls page ----
// Which Xbox 360 input drives each N64 control, per player (LB/RB), and the
// analog stick's source (a stick or the D-pad), inverted axes, dead zone and sensitivity. A on a button waits for
// an input to set it (Y adds one more, X clears); BACK or 5 s cancel.
// Inputs are read from controller 1, whichever player is being edited.
enum { C_STICK = H64_N64_COUNT, C_INVERT, C_DEAD, C_RANGE, C_RUMBLE, C_RESET, C_BACK, C_COUNT };
#define CAPTURE_MS 5000

struct ControlsState
{
    Config *c;
    int player;          // 0..3
    int capturing;       // waiting for an input for item `capItem`
    int capItem, capAdd, capReleased;
    DWORD capStart;
    char msg[160];       // a short note under the panel ("D-pad up set")
    DWORD msgAt;
};
static ControlsState s_ctl;

// The items in the order up/down walks them, and where each one is shown.
static const int s_ctlOrder[C_COUNT] = {
    H64_N64_A, H64_N64_B, H64_N64_Z, H64_N64_START, H64_N64_L, H64_N64_R,
    H64_N64_DUP, H64_N64_DDOWN, H64_N64_DLEFT, H64_N64_DRIGHT, H64_N64_CUP, H64_N64_CDOWN, H64_N64_CLEFT, H64_N64_CRIGHT,
    C_STICK, C_INVERT, C_DEAD, C_RANGE, C_RUMBLE, C_RESET, C_BACK,
};

#define CTL_X 70.0f
#define CTL_Y 55.0f
#define CTL_W 1140.0f
#define CTL_H 610.0f
#define CTL_ROW 40.0f
#define CTL_TOP 150.0f       // first row's top, from the panel's
#define CTL_COLW 352.0f
#define CTL_GAP 20.0f

// Column and row slot (0-based, in rows of CTL_ROW; headings take half a row) of an item.
static void ctl_place(int item, int *col, float *row)
{
    switch (item)
    {
    case H64_N64_A: *col = 0; *row = 0; return;
    case H64_N64_B: *col = 0; *row = 1; return;
    case H64_N64_Z: *col = 0; *row = 2; return;
    case H64_N64_START: *col = 0; *row = 3; return;
    case H64_N64_L: *col = 0; *row = 4; return;
    case H64_N64_R: *col = 0; *row = 5; return;
    case H64_N64_DUP: *col = 1; *row = 0; return;
    case H64_N64_DDOWN: *col = 1; *row = 1; return;
    case H64_N64_DLEFT: *col = 1; *row = 2; return;
    case H64_N64_DRIGHT: *col = 1; *row = 3; return;
    case H64_N64_CUP: *col = 1; *row = 5; return;
    case H64_N64_CDOWN: *col = 1; *row = 6; return;
    case H64_N64_CLEFT: *col = 1; *row = 7; return;
    case H64_N64_CRIGHT: *col = 1; *row = 8; return;
    case C_STICK: *col = 2; *row = 0; return;
    case C_INVERT: *col = 2; *row = 1; return;
    case C_DEAD: *col = 2; *row = 2; return;
    case C_RANGE: *col = 2; *row = 3; return;
    case C_RUMBLE: *col = 0; *row = 6; return;
    case C_RESET: *col = 0; *row = 7; return;
    default: *col = 0; *row = 8; return;
    }
}

static float ctl_col_x(int col) { return CTL_X + 24 + col * (CTL_COLW + CTL_GAP); }
static float ctl_row_y(float row) { return CTL_Y + CTL_TOP + row * CTL_ROW; }

static void controls_open(UiMenu *m, Config *c)
{
    UiMenuClear(m, Tr(T_CONTROLS), "");
    m->page = UI_PAGE_CONTROLS;
    m->count = C_COUNT;
    m->sel = 0;
    s_ctl.c = c;
    s_ctl.capturing = 0;
    s_ctl.msg[0] = 0;
    if (s_ctl.player < 0 || s_ctl.player > 3) s_ctl.player = 0;
}

static void ctl_note(const char *text)
{
    strncpy(s_ctl.msg, text, sizeof(s_ctl.msg) - 1);
    s_ctl.msg[sizeof(s_ctl.msg) - 1] = 0;
    s_ctl.msgAt = GetTickCount();
}

// Controller 1's state as the mapping's input.
static int ctl_read(H64PadInput *in)
{
    XINPUT_STATE st;
    if (XInputGetState(0, &st) != ERROR_SUCCESS) return 0;
    in->buttons = st.Gamepad.wButtons;
    in->lt = st.Gamepad.bLeftTrigger;
    in->rt = st.Gamepad.bRightTrigger;
    in->lx = st.Gamepad.sThumbLX;
    in->ly = st.Gamepad.sThumbLY;
    in->rx = st.Gamepad.sThumbRX;
    in->ry = st.Gamepad.sThumbRY;
    return 1;
}

// While waiting for an input: everything released first (the A that started
// it), then the first input pressed. BACK cancels, as does the time out.
static int controls_capture(void)
{
    H64PadInput in;
    u32 src = 0;
    H64PadMap *map = &s_ctl.c->padMap[s_ctl.player];
    int i;
    char note[160];
    if (ctl_read(&in))
    {
        if (in.buttons & XINPUT_GAMEPAD_BACK)
        {
            s_ctl.capturing = 0;
            ctl_note(Tr(T_CANCELLED));
            return SET_NONE;
        }
        src = h64_padmap_sources(&in);
    }
    if (!s_ctl.capReleased)
    {
        if (!src) { s_ctl.capReleased = 1; s_ctl.capStart = GetTickCount(); }
        return SET_NONE;
    }
    if (GetTickCount() - s_ctl.capStart >= CAPTURE_MS)
    {
        s_ctl.capturing = 0;
        ctl_note(Tr(T_NO_INPUT));
        return SET_NONE;
    }
    if (!src) return SET_NONE;
    for (i = 0; i < H64_SRC_COUNT; i++)
        if (src & (1u << i)) break;
    if (s_ctl.capAdd) map->src[s_ctl.capItem] |= 1u << i;
    else map->src[s_ctl.capItem] = 1u << i;
    s_ctl.capturing = 0;
    _snprintf(note, sizeof(note) - 1, Tr(s_ctl.capAdd ? T_INPUT_ADDED : T_INPUT_SET), TrN64(s_ctl.capItem), TrSource(i));
    note[sizeof(note) - 1] = 0;
    ctl_note(note);
    return SET_CHANGED;
}

static int controls_input(UiMenu *m, Config *c, WORD down)
{
    H64PadMap *map;
    int item, dir, col, i;
    float row;
    s_ctl.c = c;
    map = &c->padMap[s_ctl.player];
    if (s_ctl.capturing) return controls_capture();
    item = s_ctlOrder[m->sel];
    dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if ((down & XINPUT_GAMEPAD_B) || (item == C_BACK && (down & XINPUT_GAMEPAD_A)))
    {
        settings_main(m, c);
        m->sel = S_CONTROLS;
        return SET_NONE;
    }
    if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
    {
        s_ctl.player = (s_ctl.player + ((down & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 3)) % 4;
        return SET_NONE;
    }
    if (item < H64_N64_COUNT)
    {
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y))
        {
            s_ctl.capturing = 1;
            s_ctl.capItem = item;
            s_ctl.capAdd = (down & XINPUT_GAMEPAD_A) ? 0 : 1;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
            return SET_NONE;
        }
        if (down & XINPUT_GAMEPAD_X)
        {
            map->src[item] = 0;
            return SET_CHANGED;
        }
    }
    if (item == C_RESET && (down & XINPUT_GAMEPAD_A))
    {
        h64_padmap_default(map);
        ctl_note(Tr(T_DEFAULT_CONTROLS));
        return SET_CHANGED;
    }
    if (item == C_RUMBLE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->rumble = !c->rumble;   // one setting for every player
        return SET_CHANGED;
    }
    if (item >= C_STICK && item <= C_RANGE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        if (item == C_STICK) map->stick = (map->stick + dir + H64_STICK_COUNT) % H64_STICK_COUNT;
        else if (item == C_INVERT) map->invert = (map->invert + dir + 4) & 3;
        else if (item == C_DEAD)
        {
            map->deadzone += dir * 1000;
            if (map->deadzone < 0) map->deadzone = 0;
            if (map->deadzone > 16000) map->deadzone = 16000;
        }
        else
        {
            map->range += dir * 10;
            if (map->range < 50) map->range = 50;
            if (map->range > 150) map->range = 150;
        }
        return SET_CHANGED;
    }
    if (dir && (item < C_STICK || item >= C_RUMBLE))
    {
        // Left/right on a button row: the next column, on the row nearest the same height.
        int best = m->sel;
        float bestD = 1e9f;
        ctl_place(item, &col, &row);
        col = (col + dir + 3) % 3;
        for (i = 0; i < C_COUNT; i++)
        {
            int c2;
            float r2, d;
            ctl_place(s_ctlOrder[i], &c2, &r2);
            d = r2 > row ? r2 - row : row - r2;
            if (c2 == col && d < bestD) { bestD = d; best = i; }
        }
        m->sel = best;
    }
    return SET_NONE;
}

// The inputs of a mask as button prompts ("X/Y/LT/RT|DPAD:Up"), or "" when none.
static void ctl_hints(u32 mask, char *out, size_t size)
{
    static const char *const glyph[H64_SRC_COUNT] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "DPAD", "DPAD", "DPAD", "DPAD",
        "LS", "RS", "LS", "LS", "LS", "LS", "RS", "RS", "RS", "RS",
    };
    static const int label[H64_SRC_COUNT] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT,
        T_CLICK, T_CLICK, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT,
    };
    int pass, i;
    out[0] = 0;
    // Plain glyphs first, sharing one prompt ("X/Y/LT"), then the labelled ones.
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < H64_SRC_COUNT; i++)
        {
            char item[96];
            size_t len = strlen(out);
            int labelled = label[i] >= 0;
            if (!(mask & (1u << i)) || labelled != pass) continue;
            if (labelled) _snprintf(item, sizeof(item) - 1, "%s:%s", glyph[i], Tr(label[i]));
            else strcpy(item, glyph[i]);
            item[sizeof(item) - 1] = 0;
            if (len + strlen(item) + 2 > size) return;
            if (len) strcat(out, pass ? "|" : "/");
            strcat(out, item);
        }
}

static void ctl_value_text(int item, const H64PadMap *map, char *out)
{
    if (item == C_RUMBLE) { sprintf(out, "< %s >", onoff(s_ctl.c ? s_ctl.c->rumble : 1)); return; }
    const char *sticks[H64_STICK_COUNT] = { Tr(T_STICK_LEFT), Tr(T_STICK_RIGHT), Tr(T_STICK_DPAD), Tr(T_OFF) };
    const char *inverts[4] = { Tr(T_OFF), "X", "Y", Tr(T_X_AND_Y) };
    if (item == C_STICK) sprintf(out, "< %s >", sticks[map->stick >= 0 && map->stick < H64_STICK_COUNT ? map->stick : 0]);
    else if (item == C_INVERT) sprintf(out, "< %s >", inverts[map->invert & 3]);
    else if (item == C_DEAD) sprintf(out, "< %d%% >", (map->deadzone * 100 + 16383) / 32767);
    else if (item == C_RANGE) sprintf(out, "< %d%% >", map->range);
    else out[0] = 0;
}

// ---- The N64 controller drawn on the Controls page ----
// A plain drawing of the controller in units (100 x 56, scaled by `s`): the
// control being edited is outlined, and blinks while it waits for an input.
static D3DCOLOR pad_rgb(int r, int g, int b) { return D3DCOLOR_ARGB(255, r, g, b); }

// Where `item` is on the drawing (a box in units, its corner radius); 0: not on it.
static int pad_shape(int item, float *x, float *y, float *w, float *h, float *r)
{
    static const float circles[][4] = {   // item, centre x, y, radius
        { H64_N64_A, 78.5f, 30.0f, 3.6f }, { H64_N64_B, 73.0f, 23.0f, 3.0f }, { H64_N64_START, 50.0f, 13.5f, 2.6f },
        { H64_N64_CUP, 86.0f, 12.5f, 2.1f }, { H64_N64_CDOWN, 86.0f, 21.5f, 2.1f },
        { H64_N64_CLEFT, 81.5f, 17.0f, 2.1f }, { H64_N64_CRIGHT, 90.5f, 17.0f, 2.1f },
        { C_STICK, 50.0f, 30.0f, 6.0f }, { C_INVERT, 50.0f, 30.0f, 6.0f }, { C_DEAD, 50.0f, 30.0f, 6.0f },
        { C_RANGE, 50.0f, 30.0f, 6.0f },
    };
    static const float boxes[][6] = {     // item, x, y, w, h, radius
        { H64_N64_DUP, 13.8f, 10.7f, 4.4f, 4.3f, 1.0f }, { H64_N64_DDOWN, 13.8f, 19.4f, 4.4f, 4.3f, 1.0f },
        { H64_N64_DLEFT, 9.5f, 15.0f, 4.3f, 4.4f, 1.0f }, { H64_N64_DRIGHT, 18.2f, 15.0f, 4.3f, 4.4f, 1.0f },
        { H64_N64_L, 8.0f, 1.0f, 22.0f, 5.5f, 2.5f }, { H64_N64_R, 70.0f, 1.0f, 22.0f, 5.5f, 2.5f },   // the part above the body
        { H64_N64_Z, 45.5f, 45.0f, 9.0f, 7.0f, 2.0f },
    };
    int i;
    for (i = 0; i < (int)(sizeof(circles) / sizeof(circles[0])); i++)
        if ((int)circles[i][0] == item)
        {
            *x = circles[i][1] - circles[i][3]; *y = circles[i][2] - circles[i][3];
            *w = *h = circles[i][3] * 2; *r = circles[i][3];
            return 1;
        }
    for (i = 0; i < (int)(sizeof(boxes) / sizeof(boxes[0])); i++)
        if ((int)boxes[i][0] == item)
        {
            *x = boxes[i][1]; *y = boxes[i][2]; *w = boxes[i][3]; *h = boxes[i][4]; *r = boxes[i][5];
            return 1;
        }
    return 0;
}

static void draw_n64_pad(float ox, float oy, float s, int hiItem, int blink)
{
    const D3DCOLOR dark = pad_rgb(54, 56, 64), shoulder = pad_rgb(78, 80, 90);
    float x, y, w, h, r;
    int i;
#define PU(v) ((v) * s)
    // Shoulder buttons behind the body, then the body: three handles and the top bar.
    UiRoundRect(ox + PU(8), oy + PU(1), PU(22), PU(8), PU(2.5f), shoulder, shoulder);
    UiRoundRect(ox + PU(70), oy + PU(1), PU(22), PU(8), PU(2.5f), shoulder, shoulder);
    UiShadow(ox + PU(4), oy + PU(6), PU(92), PU(50), PU(9), 18, UI_COL_SHADOW);
    UiRoundRect(ox + PU(6), oy + PU(12), PU(20), PU(40), PU(10), pad_rgb(152, 154, 164), pad_rgb(112, 114, 124));
    UiRoundRect(ox + PU(74), oy + PU(12), PU(20), PU(40), PU(10), pad_rgb(152, 154, 164), pad_rgb(112, 114, 124));
    UiRoundRect(ox + PU(41), oy + PU(12), PU(18), PU(44), PU(9), pad_rgb(152, 154, 164), pad_rgb(108, 110, 120));
    UiRoundRect(ox + PU(4), oy + PU(6), PU(92), PU(20), PU(9), pad_rgb(178, 180, 190), pad_rgb(146, 148, 158));
    // D-pad, Start, the stick, Z (under the middle handle on the real one).
    UiRoundRect(ox + PU(9.5f), oy + PU(15), PU(13), PU(4.4f), PU(1), dark, dark);
    UiRoundRect(ox + PU(13.8f), oy + PU(10.7f), PU(4.4f), PU(13), PU(1), dark, dark);
    UiCircle(ox + PU(50), oy + PU(13.5f), PU(2.6f), pad_rgb(206, 44, 44));
    UiCircle(ox + PU(50), oy + PU(30), PU(6), pad_rgb(66, 68, 78));
    UiCircle(ox + PU(50), oy + PU(30), PU(4), pad_rgb(196, 198, 206));
    UiRoundRect(ox + PU(45.5f), oy + PU(45), PU(9), PU(7), PU(2), dark, dark);
    UiText(UI_BOLD, PU(4.5f), ox + PU(50), oy + PU(48.5f), UI_COL_TEXT2, "Z", UI_CENTER);
    // A, B and the four C buttons.
    UiCircle(ox + PU(78.5f), oy + PU(30), PU(3.6f), pad_rgb(52, 92, 214));
    UiText(UI_BOLD, PU(4.2f), ox + PU(78.5f), oy + PU(30), UI_COL_TEXT, "A", UI_CENTER);
    UiCircle(ox + PU(73), oy + PU(23), PU(3), pad_rgb(40, 164, 72));
    UiText(UI_BOLD, PU(3.6f), ox + PU(73), oy + PU(23), UI_COL_TEXT, "B", UI_CENTER);
    for (i = H64_N64_CUP; i <= H64_N64_CRIGHT; i++)
        if (pad_shape(i, &x, &y, &w, &h, &r))
            UiCircle(ox + PU(x + r), oy + PU(y + r), PU(r), pad_rgb(232, 192, 40));
    // The control being edited: outlined; blinking while an input is awaited.
    if (pad_shape(hiItem, &x, &y, &w, &h, &r))
    {
        float on = blink ? 0.5f + 0.5f * sinf(UiTime() * 12.566f) : 1.0f, pad = 3.0f;
        int a = (int)(255 * on);
        x = ox + PU(x); y = oy + PU(y); w = PU(w); h = PU(h); r = PU(r);
        if (blink)
        {
            UiShadow(x - 2, y - 2, w + 4, h + 4, r + 2, 16, D3DCOLOR_ARGB((int)(200 * on), 255, 112, 84));
            UiRoundRect(x, y, w, h, r, D3DCOLOR_ARGB((int)(190 * on), 255, 112, 84), D3DCOLOR_ARGB((int)(190 * on), 234, 76, 58));
        }
        UiRoundRing(x - pad, y - pad, w + pad * 2, h + pad * 2, r + pad, 2.5f, D3DCOLOR_ARGB(a, 255, 112, 84));
    }
#undef PU
}

static void controls_draw(const UiMenu *m)
{
    static UiAnim selX, selY;
    static int lastCol = -1;
    const H64PadMap *map;
    H64PadMap def;
    int i, col, selItem = s_ctlOrder[m->sel < 0 || m->sel >= C_COUNT ? 0 : m->sel];
    float row, x = CTL_X, y = CTL_Y, sx, sy;
    char buf[320];
    if (s_ctl.c) map = &s_ctl.c->padMap[s_ctl.player];
    else { h64_padmap_default(&def); map = &def; }

    UiDim();
    UiPanel(x, y, CTL_W, CTL_H, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_CONTROLS), UI_LEFT);
    UiRoundRect(x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    {
        // Player selector: "LB  Player 1  RB" at the top right.
        float r = x + CTL_W - 40, w;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PLAYER), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        r -= UiFooterWidth("RB");
        UiFooter(r, y + 50, "RB");
        w = UiTextWidth(UI_BOLD, 22, buf);
        UiText(UI_BOLD, 22, r - 16, y + 50, UI_COL_TEXT, buf, UI_RIGHT);
        r -= 16 + w + 16 + UiFooterWidth("LB");
        UiFooter(r, y + 50, "LB");
        if (s_ctl.player)
        {
            _snprintf(buf, sizeof(buf) - 1, Tr(T_USED_WHEN), s_ctl.player + 1);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 16, x + CTL_W - 40, y + 82, UI_COL_TEXT3, buf, UI_RIGHT);
        }
    }

    heading(ctl_col_x(0) + 20, y + CTL_TOP - 22, Tr(T_HEAD_BUTTONS));
    heading(ctl_col_x(1) + 20, y + CTL_TOP - 22, Tr(T_HEAD_DPAD));
    heading(ctl_col_x(1) + 20, ctl_row_y(5) - 16, Tr(T_HEAD_CBUTTONS));
    heading(ctl_col_x(2) + 20, y + CTL_TOP - 22, Tr(T_HEAD_STICK));

    ctl_place(selItem, &col, &row);
    sx = anim_step(&selX, ctl_col_x(col), col != lastCol);
    sy = anim_step(&selY, ctl_row_y(row), col != lastCol);
    lastCol = col;
    selection_bar(sx, sy + 3, CTL_COLW, CTL_ROW - 6);

    for (i = 0; i < C_COUNT; i++)
    {
        int item = s_ctlOrder[i], s = i == m->sel;
        float cx, cy;
        ctl_place(item, &col, &row);
        cx = ctl_col_x(col);
        cy = ctl_row_y(row) + CTL_ROW * 0.5f;
        if (item < H64_N64_COUNT)
        {
            UiText(s ? UI_BOLD : UI_REGULAR, 20, cx + 24, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, TrN64(item), UI_LEFT);
            if (s_ctl.capturing && item == s_ctl.capItem)
                UiText(UI_BOLD, 18, cx + CTL_COLW - 18, cy, UI_COL_ACCENT_HI, Tr(T_PRESS_INPUT), UI_RIGHT);
            else
            {
                ctl_hints(map->src[item], buf, sizeof(buf));
                if (!buf[0]) UiText(UI_REGULAR, 18, cx + CTL_COLW - 18, cy, UI_COL_TEXT3, Tr(T_NO_INPUT_SET), UI_RIGHT);
                else
                {
                    float w = UiFooterWidth(buf), maxW = CTL_COLW - 130;
                    if (w <= maxW) UiFooter(cx + CTL_COLW - 18 - w, cy, buf);
                    else
                    {
                        char n[64];
                        int k, cnt = 0;
                        for (k = 0; k < H64_SRC_COUNT; k++) cnt += (map->src[item] >> k) & 1;
                        _snprintf(n, sizeof(n) - 1, Tr(T_N_INPUTS), cnt);
                        n[sizeof(n) - 1] = 0;
                        UiText(UI_REGULAR, 18, cx + CTL_COLW - 18, cy, UI_COL_TEXT2, n, UI_RIGHT);
                    }
                }
            }
        }
        else
        {
            static const int labels[] = { T_ANALOG_STICK, T_INVERT, T_DEAD_ZONE, T_SENSITIVITY, T_RUMBLE, T_RESET_DEFAULT, T_BACK };
            UiText(s ? UI_BOLD : UI_REGULAR, 20, cx + 24, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, Tr(labels[item - C_STICK]), UI_LEFT);
            ctl_value_text(item, map, buf);
            if (buf[0]) draw_value(cx + CTL_COLW - 18, cy, buf, s);
        }
    }

    // The controller under the stick settings, with the control being edited.
    {
        float s = CTL_COLW / 100.0f, top = ctl_row_y(4) + 8, bottom = y + CTL_H - 86;
        draw_n64_pad(ctl_col_x(2), top + (bottom - top - 56 * s) * 0.5f, s,
                     s_ctl.capturing ? s_ctl.capItem : selItem, s_ctl.capturing);
    }

    UiRect(x + 24, y + CTL_H - 78, CTL_W - 48, 1, UI_COL_LINE);
    if (s_ctl.capturing)
    {
        DWORD age = s_ctl.capReleased ? GetTickCount() - s_ctl.capStart : 0;
        float left = age >= CAPTURE_MS ? 0.0f : (CAPTURE_MS - age) / (float)CAPTURE_MS, bw = 220;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PRESS_FOR), TrN64(s_ctl.capItem));
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_BOLD, 20, x + 40, y + CTL_H - 39, UI_COL_TEXT, buf, UI_LEFT);
        UiFooter(x + CTL_W - 40 - UiFooterWidth(Tr(T_FOOT_CANCEL)), y + CTL_H - 39, Tr(T_FOOT_CANCEL));
        {
            float bx = x + CTL_W - 80 - UiFooterWidth(Tr(T_FOOT_CANCEL)) - bw;
            UiRoundRect(bx, y + CTL_H - 42, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
            if (left > 0) UiRoundRect(bx, y + CTL_H - 42, bw * left, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
        }
    }
    else
    {
        // The last change, or a word of help, on the right.
        DWORD age = GetTickCount() - s_ctl.msgAt;
        if (selItem < H64_N64_COUNT) UiFooter(x + 40, y + CTL_H - 39, Tr(T_FOOT_CTL_BUTTON));
        else UiFooter(x + 40, y + CTL_H - 39, Tr(T_FOOT_CTL_OTHER));
        if (s_ctl.msg[0] && age < 3000) UiText(UI_BOLD, 18, x + CTL_W - 40, y + CTL_H - 39, UI_COL_ACCENT_HI, s_ctl.msg, UI_RIGHT);
        else UiText(UI_REGULAR, 16, x + CTL_W - 40, y + CTL_H - 39, UI_COL_TEXT3, Tr(T_BACK_RESERVED), UI_RIGHT);
    }
}

void SettingsBuild(UiMenu *m, const Config *c, int perGame)
{
    s_perGame = perGame;
    s_browser = 0;
    s_ctl.capturing = 0;
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
        case G_RES: c->resScale = (c->resScale - 1 + dir + 4) % 4 + 1; break;
        case G_FILTER: c->texFilter = (c->texFilter + dir + 3) % 3; break;
        case G_SMOOTH: c->smooth = !c->smooth; break;
        case G_SHARPEN: c->sharpen = (c->sharpen + dir + 3) % 3; if (c->sharpen) c->blur = 0; break;
        case G_BLUR: c->blur = (c->blur + dir + 3) % 3; if (c->blur) c->sharpen = 0; break;
        case G_SCREEN: c->screen = (c->screen + dir + 6) % 6; break;
        case G_BRIGHT: c->brightness = (c->brightness + dir + 3) % 3; break;
        case G_STRETCH: c->stretch = !c->stretch; break;
        case G_VSYNC: c->vsync = ((c->vsync < 0 || c->vsync > 2 ? 0 : c->vsync) + dir + 3) % 3; break;   // off, on (adaptive), strict
        case G_DEFLICKER: c->deflicker = !c->deflicker; break;
        case G_ZOOM: c->zoom += dir * 2; if (c->zoom < 80) c->zoom = 80; if (c->zoom > 140) c->zoom = 140; break;
        case G_POSX: c->offsetX += dir * 4; if (c->offsetX < -160) c->offsetX = -160; if (c->offsetX > 160) c->offsetX = 160; break;
        case G_POSY: c->offsetY += dir * 4; if (c->offsetY < -120) c->offsetY = -120; if (c->offsetY > 120) c->offsetY = 120; break;
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
    if (m->page == UI_PAGE_CONTROLS) return controls_input(m, c, down);
    dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B) return SET_BACK;
    if (m->sel == S_GRAPHICS && (down & XINPUT_GAMEPAD_A))
    {
        settings_graphics(m, c);
        return SET_NONE;
    }
    if (m->sel == S_CONTROLS && (down & XINPUT_GAMEPAD_A))
    {
        controls_open(m, c);
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
        case S_FPS: c->showFps = ((c->showFps < 0 || c->showFps > 3 ? 0 : c->showFps) + dir + 4) % 4; break;   // off, FPS, + CPU/RAM, + resolution
        }
        changed = 1;
    }
    if (m->sel == S_LANGUAGE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // From the ROM list, A opens the list with the flags; left/right (and A in game) step through the languages.
        if (s_browser && !dir) return SET_LANGUAGE;
        lang_change(c, (LangGet() + (dir ? dir : 1) + LANG_COUNT) % LANG_COUNT);
        settings_main(m, c);
        m->sel = S_LANGUAGE;
        return SET_NONE;
    }
    if (s_browser && m->sel == S_ART && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->artDownload = !c->artDownload;
        changed = 1;
    }
    settings_values(m, c);
    if (changed) return SET_CHANGED;
    if (down & XINPUT_GAMEPAD_A)
    {
        int last = m->count - 1;
        if (m->sel == last) return SET_BACK;
        if (s_browser && m->sel == S_ART_ALL) return SET_ART_ALL;
        if (m->sel == S_SAVE_ALL) return SET_SAVE_ALL;
        if (perGame && m->sel == S_SAVE_ALL + 1) return SET_SAVE_GAME;
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
        if (LangRtl())
        {
            UiText(UI_BOLD, 30, x + w - 48, y + 70, UI_COL_TEXT, Tr(T_QUIT_ASK), UI_RIGHT);
            UiFooter(x + w - 48 - UiFooterWidth(Tr(T_FOOT_QUIT)), y + h - 50, Tr(T_FOOT_QUIT));
        }
        else
        {
            UiText(UI_BOLD, 30, x + 48, y + 70, UI_COL_TEXT, Tr(T_QUIT_ASK), UI_LEFT);
            UiFooter(x + 48, y + h - 50, Tr(T_FOOT_QUIT));
        }
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
                         "and to Timothy Lottes and AMD for their display filters. Box art and game "
                         "information: libretro, GameDB-N64, Wikipedia.");
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

// ---- Language list ----
// Seven cards (four, then three), each with its flag and the language's own
// name; the title and the button prompt follow the card under the cursor.
#define LANG_CARD_W 220.0f
#define LANG_CARD_H 196.0f
#define LANG_GAP 24.0f

static void lang_card_pos(int i, float *x, float *y)
{
    int row = i < 4 ? 0 : 1, n = row ? LANG_COUNT - 4 : 4, col = row ? i - 4 : i;
    float total = n * LANG_CARD_W + (n - 1) * LANG_GAP;
    *x = (UI_WIDTH - total) * 0.5f + col * (LANG_CARD_W + LANG_GAP);
    *y = row ? 408.0f : 188.0f;
}

// The card in the other row nearest to card `i` (up/down).
static int lang_other_row(int i)
{
    float x, y, bx, by, best = 1e9f;
    int k, pick = i;
    lang_card_pos(i, &x, &y);
    for (k = 0; k < LANG_COUNT; k++)
    {
        float d;
        if ((k < 4) == (i < 4)) continue;
        lang_card_pos(k, &bx, &by);
        d = fabsf(bx - x);
        if (d < best) { best = d; pick = k; }
    }
    return pick;
}

static int s_langAutoMs;
void LanguageScreenAuto(int ms) { s_langAutoMs = ms; }

int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart)
{
    IDirect3DTexture9 *flags[LANG_COUNT];
    UiInput in;
    UiAnim ax, ay;
    int sel = initial >= 0 && initial < LANG_COUNT ? initial : LANG_EN, i, result = -2;
    DWORD openedAt = GetTickCount();
    memset(&ax, 0, sizeof(ax));
    memset(&ay, 0, sizeof(ay));
    for (i = 0; i < LANG_COUNT; i++)
    {
        int w = 0, h = 0;
        u8 *px = h64_png_decode(s_flagPng[i], s_flagPngSize[i], &w, &h, FLAG_W * FLAG_H);
        flags[i] = px ? UiImageCreate(dev, px, w, h) : NULL;
        free(px);
    }
    UiInputInit(&in);
    H64_INFO("[lang] language list (%s)", firstStart ? "first start" : "settings");
    while (result == -2)
    {
        WORD down = UiInputPoll(&in);
        float cx, cy, now = UiTime();
        if (down & XINPUT_GAMEPAD_DPAD_LEFT) sel = (sel + LANG_COUNT - 1) % LANG_COUNT;
        if (down & XINPUT_GAMEPAD_DPAD_RIGHT) sel = (sel + 1) % LANG_COUNT;
        if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) sel = lang_other_row(sel);
        if ((down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) || (s_langAutoMs > 0 && GetTickCount() - openedAt >= (DWORD)s_langAutoMs)) result = sel;
        else if (!firstStart && (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))) result = -1;

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiText(UI_BOLD, 34, UI_WIDTH * 0.5f, 134, UI_COL_TEXT, TrIn(sel, T_LANG_TITLE), UI_CENTER);
        lang_card_pos(sel, &cx, &cy);
        cx = anim_step(&ax, cx, 0);
        cy = anim_step(&ay, cy, 0);
        UiShadow(cx - 4, cy - 4, LANG_CARD_W + 8, LANG_CARD_H + 8, 24, 26, D3DCOLOR_ARGB(150, 234, 76, 58));
        for (i = 0; i < LANG_COUNT; i++)
        {
            float x, y;
            int s = i == sel;
            lang_card_pos(i, &x, &y);
            UiShadow(x, y + 8, LANG_CARD_W, LANG_CARD_H, 20, 24, UI_COL_SHADOW);
            UiRoundRect(x, y, LANG_CARD_W, LANG_CARD_H, 20, s ? D3DCOLOR_ARGB(255, 52, 40, 46) : UI_COL_PANEL_TOP,
                        s ? D3DCOLOR_ARGB(255, 40, 31, 36) : UI_COL_PANEL);
            UiRoundRing(x, y, LANG_CARD_W, LANG_CARD_H, 20, 1.0f, UI_COL_LINE);
            if (flags[i])
            {
                const float fw = 176, fh = 117, fx = x + (LANG_CARD_W - fw) * 0.5f, fy = y + 20;
                UiShadow(fx, fy + 4, fw, fh, 8, 12, D3DCOLOR_ARGB(140, 0, 0, 0));
                UiImage(flags[i], fx, fy, fw, fh, 8, s ? D3DCOLOR_ARGB(255, 255, 255, 255) : D3DCOLOR_ARGB(255, 200, 200, 200));
                UiRoundRing(fx, fy, fw, fh, 8, 1.0f, D3DCOLOR_ARGB(50, 255, 255, 255));
            }
            UiText(s ? UI_BOLD : UI_REGULAR, 22, x + LANG_CARD_W * 0.5f, y + 166, s ? UI_COL_TEXT : UI_COL_TEXT2, TrIn(i, T_LANG_NAME), UI_CENTER);
        }
        {
            // The selected card's outline, pulsing gently.
            float on = 0.75f + 0.25f * sinf(now * 4.0f);
            UiRoundRing(cx - 3, cy - 3, LANG_CARD_W + 6, LANG_CARD_H + 6, 23, 3.0f, D3DCOLOR_ARGB((int)(255 * on), 255, 112, 84));
        }
        UiScreenFooter(TrIn(sel, firstStart ? T_FOOT_LANG_FIRST : T_FOOT_LANG));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    for (i = 0; i < LANG_COUNT; i++)
        if (flags[i]) UiImageRelease(dev, flags[i]);
    H64_INFO("[lang] language list: %s", result >= 0 ? LangCode(result) : "left");
    return result;
}

// ---- ROM browser ----
struct RomEntry { std::string file, shown; };
static bool rom_less(const RomEntry &a, const RomEntry &b) { return _stricmp(a.shown.c_str(), b.shown.c_str()) < 0; }

// `dir` ends with a backslash (Config.romDir).
static void scan_roms(const char *dir, std::vector<RomEntry> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((std::string(dir) + "*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const char *dot = strrchr(fd.cFileName, '.');
        RomEntry e;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64") && _stricmp(dot, ".zip"))) continue;
        e.file = std::string(dir) + fd.cFileName;
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
// Returns the ROM size, or 0 when it is not an N64 ROM. Any thread.
static u32 read_header(const char *path, H64Rom *rom)
{
    const u32 headSize = 0x1000;
    u8 *head;
    H64ZipReader r;
    H64ZipEntry e;
    int zip, ok;
    u32 len;
    HANDLE h = rom_open(path, &r, &e, &zip, &len);
    memset(rom, 0, sizeof(*rom));
    if (h == INVALID_HANDLE_VALUE) return 0;
    head = (u8 *)malloc(headSize);
    ok = head && len >= headSize && (zip ? h64_zip_extract(&r, &e, head, headSize) == 0 : handle_read(h, 0, head, headSize) == headSize);
    CloseHandle(h);
    if (!ok || h64_rom_load(rom, head, headSize)) { free(head); return 0; }
    free(head);
    return len;
}

u32 RomReadHeader(const char *path, H64Rom *rom) { return read_header(path, rom); }

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
    char name[24], code[8], size[16], save[128], folder[64], region[32];
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
    _snprintf(info->save, sizeof(info->save), "%s%s",
              type == H64_SAVE_NONE ? Tr(T_SAVE_NONE) : type == H64_SAVE_AUTO ? Tr(T_SAVE_UNKNOWN) : h64_save_type_name(type),
              pak ? " + Controller Pak" : "");
    h64_save_folder_name(&rom, info->folder, (int)sizeof(info->folder));
    strncpy(info->region, h64_gameinfo_region_name(rom.gameCode[3]), sizeof(info->region) - 1);
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

// The settings for all games, from the ROM list. Returns 1 when the user
// asked for the box art of all games.
static int settings_loop(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, int ctlTest)
{
    UiMenu m;
    UiInput in;
    UiInputInit(&in);
    SettingsBuild(&m, c, 0);
    s_browser = 1;
    settings_main(&m, c);
    if (ctlTest >= 300)
    {
        settings_graphics(&m, c);            // controlstest=300+N: the Graphics page, on item N
        m.sel = (ctlTest - 300) % m.count;
    }
    else if (ctlTest >= 200)
        m.sel = (ctlTest - 200) % m.count;   // controlstest=200+N: this page, on item N
    else if (ctlTest >= 0)
    {
        // controlstest=N: the Controls page on item N; 100+N: also waiting
        // for an input, again and again (captures of the blinking control).
        controls_open(&m, c);
        m.sel = (ctlTest % 100) % C_COUNT;
    }
    for (;;)
    {
        int r;
        if (ctlTest >= 100 && ctlTest < 200 && !s_ctl.capturing && s_ctlOrder[m.sel] < H64_N64_COUNT)
        {
            s_ctl.capturing = 1;
            s_ctl.capItem = s_ctlOrder[m.sel];
            s_ctl.capAdd = 0;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
        }
        r = SettingsInput(&m, c, 0, UiInputPoll(&in));
        if (r == SET_LANGUAGE)
        {
            int lang = LanguageScreen(dev, LangGet(), 0);
            if (lang >= 0) lang_change(c, lang);
            settings_main(&m, c);
            m.sel = S_LANGUAGE;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_BACK) { s_browser = 0; return 0; }
        if (r == SET_ART_ALL) { s_browser = 0; return 1; }
        if (r == SET_SAVE_ALL)
        {
            H64_INFO("[menu] settings for all games %s", ConfigWriteMenuKeys(c, settingsPath, 1) ? "saved" : "NOT saved");
            s_browser = 0;
            return 0;
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

// ---- Box art in the browser ----
// The browser's copies of what xb_art.cpp sends (textures and text), by ROM
// file; the least recently shown is dropped when the table is full.
#define ART_VIEWS 24
struct ArtView
{
    int used;
    char path[256];
    int hasInfo;
    H64GameInfo info;
    IDirect3DTexture9 *cover, *glow;
    int coverW, coverH;
    float coverAt;      // UiTime() when the cover arrived (its fade-in)
    DWORD lastUse;
};
static ArtView s_views[ART_VIEWS];
static DWORD s_viewClock;

static void view_free(IDirect3DDevice9 *dev, ArtView *v)
{
    if (v->cover) UiImageRelease(dev, v->cover);
    if (v->glow) UiImageRelease(dev, v->glow);
    memset(v, 0, sizeof(*v));
}

static ArtView *view_find(const char *path)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used && !_stricmp(s_views[i].path, path))
        {
            s_views[i].lastUse = ++s_viewClock;
            return &s_views[i];
        }
    return NULL;
}

// Turns the worker's results into textures.
static void views_take(IDirect3DDevice9 *dev)
{
    ArtResult r;
    while (ArtTake(&r))
    {
        ArtView *v = view_find(r.path);
        float coverAt = UiTime();
        if (v)
        {
            // Newer files for a ROM already shown (a download finished): a cover it had does not fade in again.
            if (v->cover && r.cover) coverAt = v->coverAt;
            view_free(dev, v);
        }
        else
        {
            int i, best = 0;
            for (i = 0; i < ART_VIEWS; i++)
            {
                if (!s_views[i].used) { best = i; break; }
                if (s_views[i].lastUse < s_views[best].lastUse) best = i;
            }
            v = &s_views[best];
            if (v->used)
            {
                ArtForget(v->path);   // sent again if the user comes back to it
                view_free(dev, v);
            }
        }
        v->used = 1;
        strncpy(v->path, r.path, sizeof(v->path) - 1);
        v->hasInfo = r.hasInfo;
        v->info = r.info;
        if (r.cover)
        {
            v->cover = UiImageCreate(dev, r.cover, r.coverW, r.coverH);
            v->coverW = r.coverW;
            v->coverH = r.coverH;
        }
        if (r.glow) v->glow = UiImageCreate(dev, r.glow, ART_GLOW_W, ART_GLOW_H);
        v->coverAt = coverAt;
        v->lastUse = ++s_viewClock;
        ArtResultFree(&r);
    }
}

static void views_clear(IDirect3DDevice9 *dev)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used) view_free(dev, &s_views[i]);
    ArtForgetAll();
}

// ---- Browser layout (UI pixels) ----
#define LIST_W 556.0f
#define LIST_Y 128.0f
#define ROW_H 36.0f
#define CARD_X 644.0f
#define CARD_Y 44.0f
#define CARD_W ((float)UI_WIDTH - UI_SAFE_X - CARD_X)
#define CARD_B 620.0f            // bottom of the list and the card
#define STAGE_H 240.0f

static float smooth01(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3.0f - 2.0f * t);
}

// A colour pair picked from a game's name (the placeholder's gradient).
static void name_colors(const char *name, D3DCOLOR *top, D3DCOLOR *bottom)
{
    static const D3DCOLOR tops[6] = { D3DCOLOR_ARGB(255, 240, 92, 66), D3DCOLOR_ARGB(255, 245, 150, 60), D3DCOLOR_ARGB(255, 150, 100, 235),
                                      D3DCOLOR_ARGB(255, 70, 140, 240), D3DCOLOR_ARGB(255, 40, 180, 170), D3DCOLOR_ARGB(255, 110, 190, 80) };
    static const D3DCOLOR bottoms[6] = { D3DCOLOR_ARGB(255, 150, 28, 30), D3DCOLOR_ARGB(255, 180, 70, 20), D3DCOLOR_ARGB(255, 80, 40, 150),
                                         D3DCOLOR_ARGB(255, 30, 70, 160), D3DCOLOR_ARGB(255, 16, 100, 110), D3DCOLOR_ARGB(255, 40, 110, 50) };
    u32 hsh = 2166136261u;
    const char *p;
    for (p = name; *p; p++) hsh = (hsh ^ (u8)*p) * 16777619u;
    *top = tops[hsh % 6];
    *bottom = bottoms[hsh % 6];
}

// The picture area when there is no box art: the name's gradient and its initials.
static void stage_placeholder(float x, float y, float w, float h, const char *name, float a)
{
    D3DCOLOR top, bottom;
    char ini[4];
    if (a <= 0.01f) return;
    name_colors(name, &top, &bottom);
    initials(name, ini);
    UiRoundRect(x, y, w, h, 16, UiAlpha(top, a * 0.85f), UiAlpha(bottom, a * 0.85f));
    UiRoundRect(x + 2, y + 2, w - 4, h * 0.5f, 14, UiAlpha(D3DCOLOR_ARGB(30, 255, 255, 255), a), D3DCOLOR_ARGB(0, 255, 255, 255));
    UiTextShadowed(UI_BOLD, 92, x + w * 0.5f, y + h * 0.5f, UiAlpha(D3DCOLOR_ARGB(235, 255, 255, 255), a), ini, UI_CENTER);
}

// A small rounded label ("Platform", "1-4 players"); returns its width.
static float chip(float x, float cy, const char *text, float a)
{
    float w = UiTextWidth(UI_BOLD, 15, text) + 26, h = 28;
    UiRoundRect(x, cy - h * 0.5f, w, h, h * 0.5f, UiAlpha(D3DCOLOR_ARGB(255, 50, 53, 68), a), UiAlpha(D3DCOLOR_ARGB(255, 42, 45, 58), a));
    UiRoundRing(x, cy - h * 0.5f, w, h, h * 0.5f, 1.0f, UiAlpha(D3DCOLOR_ARGB(34, 255, 255, 255), a));
    UiText(UI_BOLD, 15, x + w * 0.5f, cy, UiAlpha(UI_COL_TEXT2, a), text, UI_CENTER);
    return w;
}

// One label / value pair of the card's last row.
static void detail(float x, float y, float w, const char *label, const char *value, float a)
{
    char fit[96];
    UiText(UI_BOLD, 13, x, y, UiAlpha(UI_COL_TEXT3, a), label, UI_LEFT);
    UiTextFit(UI_REGULAR, 19, w, value, fit, sizeof(fit));
    UiText(UI_REGULAR, 19, x, y + 25, UiAlpha(UI_COL_TEXT, a), fit, UI_LEFT);
}

// The card on the right: box art, title, facts. `selAt`: when the selection
// changed (the card fades and slides in).
static void card_draw(const char *shown, const RomInfo *ri, const ArtView *v, int busy, float selAt)
{
    const float x = CARD_X, y = CARD_Y, w = CARD_W, h = CARD_B - CARD_Y, ix = x + 22, iw = w - 44;
    const float sx = ix, sy = y + 22, sw = iw, sh = STAGE_H;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    float now = UiTime(), a = 0.3f + 0.7f * smooth01((now - selAt) / 0.2f), slide = (1.0f - smooth01((now - selAt) / 0.2f)) * 10.0f;
    float ca = 0, ty, divY = CARD_B - 76;
    char buf[1024];

    UiPanel(x, y, w, h, 22);

    // ---- The picture: the box art over a glow of its own colours, else the placeholder.
    if (v && v->cover)
        ca = smooth01((now - (v->coverAt > selAt ? v->coverAt : selAt)) / 0.25f);
    UiRoundRect(sx, sy, sw, sh, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    {
        // The initials only when no picture is on its way: the worker sends a game
        // with its picture already decoded, so before its answer, wait a moment.
        float pa = v ? 1.0f : smooth01((now - selAt - 0.3f) / 0.2f);
        stage_placeholder(sx, sy, sw, sh, shown, a * pa * (1.0f - ca));
    }
    if (ca > 0)
    {
        float maxW = sw - 56, maxH = sh - 36, cw = (float)v->coverW, chh = (float)v->coverH, k, cx, cy;
        if (v->glow) UiImage(v->glow, sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        // A veil keeps the glow behind the box (and the edges dark).
        UiRoundRect(sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(120, 12, 13, 18), ca * a), UiAlpha(D3DCOLOR_ARGB(185, 12, 13, 18), ca * a));
        k = maxW / cw < maxH / chh ? maxW / cw : maxH / chh;
        if (k > 1.15f) k = 1.15f;
        cw *= k;
        chh *= k;
        cx = sx + (sw - cw) * 0.5f;
        cy = sy + (sh - chh) * 0.5f + (1.0f - ca) * 6.0f;
        UiShadow(cx, cy + 8, cw, chh, 8, 26, UiAlpha(D3DCOLOR_ARGB(170, 0, 0, 0), ca * a));
        UiImage(v->cover, cx, cy, cw, chh, 7, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        UiRoundRing(cx, cy, cw, chh, 7, 1.0f, UiAlpha(D3DCOLOR_ARGB(40, 255, 255, 255), ca * a));
    }
    UiRoundRing(sx, sy, sw, sh, 16, 1.0f, UiAlpha(UI_COL_LINE, a));
    if (busy && ca <= 0)
    {
        // Downloading: a pill with three pulsing dots at the bottom right of the picture.
        float pw = UiTextWidth(UI_REGULAR, 15, Tr(T_DOWNLOADING)) + 64, ph = 30, px = sx + sw - pw - 12, py = sy + sh - ph - 12;
        int d;
        UiRoundRect(px, py, pw, ph, ph * 0.5f, D3DCOLOR_ARGB(200, 16, 17, 23), D3DCOLOR_ARGB(200, 16, 17, 23));
        UiText(UI_REGULAR, 15, px + 14, py + ph * 0.5f, UI_COL_TEXT2, Tr(T_DOWNLOADING), UI_LEFT);
        for (d = 0; d < 3; d++)
        {
            float on = 0.5f + 0.5f * sinf(now * 6.0f - d * 0.9f);
            UiCircle(px + pw - 38 + d * 11, py + ph * 0.5f, 3.0f, UiAlpha(UI_COL_ACCENT_HI, 0.35f + 0.65f * on));
        }
    }

    // ---- Title, then year, developer, publisher.
    ty = sy + sh + 32 + slide;
    {
        int lines = UiTextWrap(UI_BOLD, 26, iw, 2, gi && gi->title[0] ? gi->title : shown, buf, sizeof(buf));
        UiText(UI_BOLD, 26, ix, ty, UiAlpha(UI_COL_TEXT, a), buf, UI_LEFT);
        ty += (lines - 1) * 26 * 1.35f + 32;
    }
    buf[0] = 0;
    if (gi)
    {
        const char *parts[3];
        int k, np = 0;
        parts[np++] = gi->year[0] ? gi->year : NULL;
        parts[np++] = gi->developer[0] ? gi->developer : NULL;
        parts[np++] = gi->publisher[0] && _stricmp(gi->publisher, gi->developer) ? gi->publisher : NULL;
        for (k = 0; k < np; k++)
        {
            if (!parts[k]) continue;
            if (buf[0]) strcat(buf, "  " UI_CH_MIDDOT "  ");
            strncat(buf, parts[k], sizeof(buf) - strlen(buf) - 8);
        }
    }
    if (!buf[0] && ri->ok) strncpy(buf, ri->name, sizeof(buf) - 1);   // the header's own name
    if (buf[0])
    {
        char fit[256];
        UiTextFit(UI_REGULAR, 18, iw, buf, fit, sizeof(fit));
        UiText(UI_REGULAR, 18, ix, ty, UiAlpha(gi ? UI_COL_TEXT2 : UI_COL_TEXT3, a), fit, UI_LEFT);
        ty += 36;
    }

    // ---- Genre, players, region.
    {
        float cxp = ix;
        const char *region = TrRegion(gi && gi->region[0] ? gi->region : ri->region);
        char players[64];
        int any = 0;
        if (gi && gi->genre[0]) { cxp += chip(cxp, ty, gi->genre, a) + 8; any = 1; }
        if (gi && gi->players[0])
        {
            if (!strcmp(gi->players, "1")) strcpy(players, Tr(T_ONE_PLAYER));
            else _snprintf(players, sizeof(players) - 1, Tr(T_N_PLAYERS), gi->players);
            players[sizeof(players) - 1] = 0;
            cxp += chip(cxp, ty, players, a) + 8;
            any = 1;
        }
        if (region[0] && cxp + UiTextWidth(UI_BOLD, 15, region) + 26 <= ix + iw) { cxp += chip(cxp, ty, region, a) + 8; any = 1; }
        if (any) ty += 30;
    }

    // ---- A few lines about the game (Wikipedia), as many as fit above the last row.
    if (gi && gi->description[0])
    {
        float lineH = 17 * 1.35f, room = divY - 20 - (ty + 4);
        int maxLines = (int)(room / lineH);
        if (maxLines > 0)
        {
            int lines = UiTextWrap(UI_REGULAR, 17, iw, maxLines, gi->description, buf, sizeof(buf));
            UiText(UI_REGULAR, 17, ix, ty + 4 + lineH * 0.5f, UiAlpha(UI_COL_TEXT2, a), buf, UI_LEFT);
            UiText(UI_REGULAR, 13, ix, ty + 4 + lines * lineH + 9, UiAlpha(UI_COL_TEXT3, a), Tr(T_WIKI_TEXT), UI_LEFT);
        }
    }

    // ---- What the emulator knows from the file.
    UiRect(ix, divY, iw, 1, UI_COL_LINE);
    if (!ri->ok)
        UiText(UI_REGULAR, 19, ix, divY + 36, UI_COL_ACCENT_HI, Tr(T_NOT_N64), UI_LEFT);
    else
    {
        detail(ix, divY + 22, 110, Tr(T_GAME_CODE), ri->code, 1.0f);
        detail(ix + 128, divY + 22, 90, Tr(T_SIZE), ri->size, 1.0f);
        detail(ix + 236, divY + 22, iw - 236, Tr(T_SAVE), ri->save, 1.0f);
    }
}

static void browser_draw(const std::vector<RomEntry> &roms, int sel, int top, int rows, const RomInfo *info, const ArtView *view,
                         int busy, float selAt, const ArtStatus *st, const char *romDir)
{
    static UiAnim selAnim;
    const float listX = UI_SAFE_X, listY = LIST_Y, listW = LIST_W, rowH = ROW_H, pad = 12;
    const float listH = rows * rowH + 2 * pad;
    int n = (int)roms.size(), i;
    char buf[160];
    UiBackdrop();
    {
        // The version beside the name, small (the About page has it too).
        float bw = UiBrand(UI_SAFE_X, 72, 34);
        UiText(UI_REGULAR, 18, UI_SAFE_X + bw + 14, 74, UI_COL_TEXT2, "v" H64_VERSION_STRING, UI_LEFT);
    }
    if (n)
    {
        sprintf(buf, "%d / %d", sel + 1, n);
        UiText(UI_REGULAR, 19, listX + listW, 64, UI_COL_TEXT2, buf, UI_RIGHT);
        UiTextFit(UI_REGULAR, 15, listW * 0.7f, romDir, buf, sizeof(buf));
        UiText(UI_REGULAR, 15, listX + listW, 88, UI_COL_TEXT3, buf, UI_RIGHT);
    }
    UiPanel(listX, listY, listW, listH, 20);
    if (!n)
    {
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 30, listX + listW * 0.5f, cy - 50, UI_COL_TEXT, Tr(T_NO_ROM), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy, UI_COL_TEXT2, Tr(T_COPY_ROMS), UI_CENTER);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_INTO_ROMS), romDir);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy + 32, UI_COL_TEXT2, buf, UI_CENTER);
    }
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0), textW = listW - 64;
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
        card_draw(roms[sel].shown.c_str(), info, view, busy, selAt);
    }
    UiScreenFooter(Tr(n ? T_FOOT_BROWSER : T_FOOT_BROWSER_EMPTY));
    if (st->bulk)
    {
        // "Download all" progress at the right end of the footer.
        const float bw = 150, right = UI_WIDTH - UI_SAFE_X, cy = 664;
        float frac = st->total ? (float)st->done / st->total : 0;
        if (st->done) _snprintf(buf, sizeof(buf) - 1, Tr(T_ART_PROGRESS), st->done, st->total);
        else strncpy(buf, Tr(T_ART_CONNECTING), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 17, right - bw - 16, cy, UI_COL_TEXT2, buf, UI_RIGHT);
        UiRoundRect(right - bw, cy - 3, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
        if (frac > 0) UiRoundRect(right - bw, cy - 3, bw * frac, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    }
}

// ---- File browser (START in the ROM list) ----
// The drives (xb_drives.cpp), then folders and ROM files: A opens a folder or
// starts a ROM, B goes up (from the drives: back to the ROM list), Y makes the
// folder shown the ROM list's folder (romdir= in settings.ini).
struct FileEntry { std::string name, full; int dir; };
static bool file_less(const FileEntry &a, const FileEntry &b)
{
    if (a.dir != b.dir) return a.dir > b.dir;   // folders first
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
}

static int rom_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!_stricmp(dot, ".z64") || !_stricmp(dot, ".n64") || !_stricmp(dot, ".v64") || !_stricmp(dot, ".zip"));
}

// `dir` ends with a backslash; "" lists the drives.
static void files_scan(const std::string &dir, std::vector<FileEntry> *out)
{
    out->clear();
    if (dir.empty())
    {
        char roots[8][16];
        int n = DrivesList(roots, 8), i;
        for (i = 0; i < n; i++)
        {
            FileEntry e;
            e.full = roots[i];
            e.name = std::string(roots[i], strlen(roots[i]) - 1);   // "Usb0:"
            e.dir = 1;
            out->push_back(e);
        }
        return;
    }
    {
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA((dir + "*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do
        {
            FileEntry e;
            int isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (!isDir && !rom_ext(fd.cFileName)) continue;
            e.name = fd.cFileName;
            e.full = dir + fd.cFileName + (isDir ? "\\" : "");
            e.dir = isDir;
            out->push_back(e);
        } while (FindNextFileA(f, &fd) && out->size() < 4000);
        FindClose(f);
    }
    std::sort(out->begin(), out->end(), file_less);
}

static void files_draw(const std::string &dir, const std::vector<FileEntry> &list, int sel, int top, int rows)
{
    static UiAnim selAnim;
    const float listX = UI_SAFE_X, listY = LIST_Y, listW = UI_WIDTH - 2 * UI_SAFE_X, rowH = ROW_H, pad = 12;
    const float listH = rows * rowH + 2 * pad;
    int n = (int)list.size(), i;
    char buf[300];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);
    UiText(UI_BOLD, 24, listX + listW, 64, UI_COL_TEXT, Tr(T_FILES), UI_RIGHT);
    UiTextFit(UI_REGULAR, 17, listW * 0.6f, dir.empty() ? Tr(T_FILES_DRIVES) : dir.c_str(), buf, sizeof(buf));
    UiText(UI_REGULAR, 17, listX + listW, 92, UI_COL_TEXT3, buf, UI_RIGHT);
    UiPanel(listX, listY, listW, listH, 20);
    if (!n)
        UiText(UI_REGULAR, 22, listX + listW * 0.5f, listY + listH * 0.5f, UI_COL_TEXT2, Tr(T_FILES_EMPTY), UI_CENTER);
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0);
        selection_bar(listX + 10, sy + 1, listW - (n > rows ? 36 : 20), rowH - 2);
        for (i = 0; i < rows && top + i < n; i++)
        {
            const FileEntry &e = list[top + i];
            float cy = listY + pad + i * rowH + rowH * 0.5f;
            int s = top + i == sel;
            if (e.dir)
            {
                // A small folder mark before folders and drives.
                UiRoundRect(listX + 30, cy - 7, 18, 13, 3, UI_COL_ACCENT, UI_COL_ACCENT);
                UiRoundRect(listX + 30, cy - 10, 8, 5, 2, UI_COL_ACCENT, UI_COL_ACCENT);
            }
            UiTextFit(s ? UI_BOLD : UI_REGULAR, 21, listW - 120, e.name.c_str(), buf, sizeof(buf));
            UiText(s ? UI_BOLD : UI_REGULAR, 21, listX + 60, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
        }
        if (n > rows)
        {
            float tx = listX + listW - 16, ty = listY + pad + 4, th = listH - 2 * pad - 8;
            float hh = th * rows / n, yy;
            if (hh < 28) hh = 28;
            yy = ty + (th - hh) * top / (float)(n - rows);
            UiRoundRect(tx, ty, 4, th, 2, D3DCOLOR_ARGB(20, 255, 255, 255), D3DCOLOR_ARGB(20, 255, 255, 255));
            UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
        }
    }
    UiScreenFooter(Tr(T_FOOT_FILES));
}

// Returns 1 with `path` set when a ROM was chosen, 2 when the ROM folder
// changed (c->romDir, written to settings.ini), 0 when left with B.
static int file_browser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    static std::string s_dir;   // where the browser was left (this session)
    std::vector<FileEntry> list;
    UiInput in;
    int sel = 0, top = 0;
    const int rows = 13;
    std::string came;   // the folder just left with B: selected again in its parent
    files_scan(s_dir, &list);
    if (list.empty() && !s_dir.empty()) { s_dir.clear(); files_scan(s_dir, &list); }   // a drive taken out
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int n = (int)list.size();
        if (n)
        {
            if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
            if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel > rows ? sel - rows : 0;
            if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + rows < n ? sel + rows : n - 1;
            if (down & XINPUT_GAMEPAD_A)
            {
                const FileEntry &e = list[sel];
                if (e.dir)
                {
                    s_dir = e.full;
                    files_scan(s_dir, &list);
                    sel = top = 0;
                    continue;
                }
                strncpy(path, e.full.c_str(), pathSize - 1);
                path[pathSize - 1] = 0;
                strncpy(c->lastRom, path, sizeof(c->lastRom) - 1);
                c->lastRom[sizeof(c->lastRom) - 1] = 0;
                ConfigWriteMenuKeys(c, settingsPath, 1);
                H64_INFO("[files] %s", path);
                return 1;
            }
        }
        if ((down & XINPUT_GAMEPAD_Y) && !s_dir.empty())
        {
            strncpy(c->romDir, s_dir.c_str(), sizeof(c->romDir) - 1);
            c->romDir[sizeof(c->romDir) - 1] = 0;
            ConfigWriteMenuKeys(c, settingsPath, 1);
            H64_INFO("[files] ROM folder %s", c->romDir);
            return 2;
        }
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
        {
            size_t cut;
            int k;
            if (s_dir.empty()) return 0;
            // Up one folder ("Usb0:\N64\" -> "Usb0:\"), or to the drives from a root.
            came = s_dir;
            cut = s_dir.find_last_of('\\', s_dir.size() - 2);
            if (s_dir.size() >= 2 && s_dir[s_dir.size() - 2] == ':') s_dir.clear();
            else if (cut == std::string::npos) s_dir.clear();
            else s_dir.resize(cut + 1);
            files_scan(s_dir, &list);
            sel = top = 0;
            for (k = 0; k < (int)list.size(); k++)
                if (!_stricmp(list[k].full.c_str(), came.c_str())) sel = k;
            continue;
        }
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        files_draw(s_dir, list, sel, top, rows);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

#define MSG_MS 6000   // how long a message (new version, box art) stays

// Box art may be downloaded here (the settings allow it; not in Xenia, whose
// network is not the console's, unless artdownload=2).
static int art_net_possible(const Config *c) { return (!c->xenia && h64_fenv_reliable()) || c->artDownload == 2; }

int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    std::vector<RomEntry> roms;
    std::vector<const char *> paths;
    UiInput in;
    int sel = 0, top = 0, detailsFor = -1, i, lastSel = -1;
    RomInfo details;
    const int rows = 13;
    char newVersion[32], msg[256], drive[16];
    DWORD msgAt = 0, demoAt = GetTickCount();
    float selAt = 0;
    msg[0] = 0;
    scan_roms(c->romDir, &roms);
    for (i = 0; i < (int)roms.size(); i++)
    {
        paths.push_back(roms[i].file.c_str());
        if (!_stricmp(roms[i].file.c_str(), c->lastRom)) sel = i;
    }
    if (c->browserSel[0])
    {
        // browsersel=FILE (console checks): start on that ROM, whatever the
        // last game played was.
        for (i = 0; i < (int)roms.size(); i++)
        {
            const char *f = roms[i].file.c_str(), *base = strrchr(f, '\\');
            if (!_stricmp(base ? base + 1 : f, c->browserSel)) sel = i;
        }
    }
    if (c->autoStart > 0 && !roms.empty())
    {
        // Remote checks: the last selection, then the next ROMs in the list.
        sel = (sel + c->autoIndex) % (int)roms.size();
        strncpy(path, roms[sel].file.c_str(), pathSize - 1);
        path[pathSize - 1] = 0;
        H64_INFO("[menu] autostart %d: %s", c->autoIndex + 1, path);
        return 1;
    }
    // Box art: in the folder of the settings' drive ("game:\config\settings.ini" -> game).
    SettingsSetLangFile(settingsPath);
    for (i = 0; settingsPath[i] && settingsPath[i] != ':' && i < (int)sizeof(drive) - 1; i++) drive[i] = settingsPath[i];
    drive[i] = 0;
    if (!settingsPath[i] || !i) strcpy(drive, "game");
    ArtStart(drive);
    ArtSetNetwork(c->artDownload && art_net_possible(c));
    ArtPause(0);

    UiInputInit(&in);
    memset(&details, 0, sizeof(details));
    if (c->controlsTest >= 0) settings_loop(dev, c, settingsPath, c->controlsTest);
    if (c->artAll && !roms.empty() && art_net_possible(c)) ArtBulkStart(&paths[0], (int)paths.size());   // artall=1 (checks)
    // A newer release on GitHub: a message for a few seconds (the check runs
    // in the background and may answer later, or never without a network).
    // updatecheck=2 (testing): as if this were version 0.0.0, so any release is newer.
    if (c->updateCheck && !c->xenia && h64_fenv_reliable()) UpdateCheckStart(c->updateCheck == 2 ? "0.0.0" : H64_VERSION_STRING);
    if (c->filesOpen && file_browser(dev, c, settingsPath, path, pathSize) == 1) return 1;   // filesopen=1 (checks)
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        ArtStatus st;
        int n = (int)roms.size();
        if (UpdateCheckTake(newVersion, sizeof(newVersion)))
        {
            _snprintf(msg, sizeof(msg) - 1, Tr(T_UPDATE_AVAILABLE), newVersion);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
            H64_INFO("[update] %s", msg);
        }
        if (ArtTakeNote(msg, sizeof(msg))) msgAt = GetTickCount();
        if ((down & XINPUT_GAMEPAD_BACK) && confirm_quit(dev))
        {
            views_clear(dev);
            ArtPause(1);
            return 0;
        }
        if (n)
        {
            if (c->browserDemo > 0 && GetTickCount() - demoAt >= (DWORD)c->browserDemo)
            {
                // browserdemo=N: walk the list by itself (captures).
                sel = (sel + 1) % n;
                demoAt = GetTickCount();
            }
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
                // The game needs the memory and the GPU: the pictures go, the worker waits.
                views_clear(dev);
                ArtPause(1);
                return 1;
            }
            if (sel < top) top = sel;
            if (sel >= top + rows) top = sel - rows + 1;
            if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), &details); detailsFor = sel; }
            if (sel != lastSel)
            {
                // The selection first, then its neighbours (read ahead from the files already there).
                const char *want[5];
                int k, cnt = 0;
                static const int order[5] = { 0, 1, -1, 2, -2 };
                for (k = 0; k < 5 && k < n; k++) want[cnt++] = paths[(sel + order[k] + n * 2) % n];
                ArtWant(want, cnt);
                selAt = UiTime();
                lastSel = sel;
            }
        }
        if (down & XINPUT_GAMEPAD_Y)
        {
            int all = settings_loop(dev, c, settingsPath, -1);
            detailsFor = -1;   // the card's texts again (the language may have changed)
            ArtSetNetwork(c->artDownload && art_net_possible(c));
            if (all && n)
            {
                if (!art_net_possible(c)) strncpy(msg, Tr(T_NO_DOWNLOADS), sizeof(msg) - 1);
                else
                {
                    ArtBulkStart(&paths[0], n);
                    strncpy(msg, Tr(T_GETTING_ART), sizeof(msg) - 1);
                }
                msgAt = GetTickCount();
            }
            UiInputInit(&in);
        }
        if (down & XINPUT_GAMEPAD_X)
        {
            AboutScreen(dev);
            UiInputInit(&in);
        }
        if (down & XINPUT_GAMEPAD_START)
        {
            int r = file_browser(dev, c, settingsPath, path, pathSize);
            if (r == 1)
            {
                views_clear(dev);
                ArtPause(1);
                return 1;
            }
            if (r == 2)
            {
                // Another ROM folder: the list again from it.
                roms.clear();
                paths.clear();
                scan_roms(c->romDir, &roms);
                for (i = 0; i < (int)roms.size(); i++) paths.push_back(roms[i].file.c_str());
                sel = top = 0;
                detailsFor = lastSel = -1;
                _snprintf(msg, sizeof(msg) - 1, Tr(T_ROM_FOLDER_SET), c->romDir);
                msg[sizeof(msg) - 1] = 0;
                msgAt = GetTickCount();
            }
            UiInputInit(&in);
        }
        views_take(dev);
        ArtGetStatus(&st);

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        browser_draw(roms, sel, top, rows, &details, n ? view_find(roms[sel].file.c_str()) : NULL,
                     n && !_stricmp(st.busyPath, roms[sel].file.c_str()), selAt, &st, c->romDir);
        if (msg[0])
        {
            DWORD age = GetTickCount() - msgAt;
            if (age < MSG_MS) UiToast(msg, (float)age, (float)MSG_MS);
            else msg[0] = 0;
        }
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}
