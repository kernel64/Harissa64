// Harissa64 V2 - screen text and panels (see xb_ui.h).
#include "xb_ui.h"

#include <string.h>

#include "../../core/common/h64_types.h"
#include "font8x8_basic.h"

#define MAX_RECTS 1024
static D3DRECT s_rects[MAX_RECTS];

void UiRect(IDirect3DDevice9 *dev, int x, int y, int w, int h, D3DCOLOR color)
{
    D3DRECT r;
    if (w <= 0 || h <= 0) return;
    r.x1 = x < 0 ? 0 : x;
    r.y1 = y < 0 ? 0 : y;
    r.x2 = x + w > UI_WIDTH ? UI_WIDTH : x + w;
    r.y2 = y + h > UI_HEIGHT ? UI_HEIGHT : y + h;
    if (r.x2 > r.x1 && r.y2 > r.y1) dev->Clear(1, &r, D3DCLEAR_TARGET, color, 1.0f, 0);
}

void UiText(IDirect3DDevice9 *dev, int x, int y, int scale, D3DCOLOR color, const char *text)
{
    int cx = x, n = 0;
    for (; *text; text++)
    {
        unsigned char c = (unsigned char)*text;
        int row;
        if (c == '\n') { y += 10 * scale; cx = x; continue; }
        if (c >= 128) c = '?';
        for (row = 0; row < 8; row++)
        {
            unsigned bits = font8x8_basic[c][row];   // bit 0 = leftmost pixel
            int col = 0;
            while (col < 8)
            {
                int start;
                if (!(bits & (1u << col))) { col++; continue; }
                start = col;
                while (col < 8 && (bits & (1u << col))) col++;
                if (n == MAX_RECTS)
                {
                    dev->Clear(n, s_rects, D3DCLEAR_TARGET, color, 1.0f, 0);
                    n = 0;
                }
                s_rects[n].x1 = cx + start * scale;
                s_rects[n].y1 = y + row * scale;
                s_rects[n].x2 = cx + col * scale;
                s_rects[n].y2 = y + (row + 1) * scale;
                if (s_rects[n].x1 >= 0 && s_rects[n].y1 >= 0 && s_rects[n].x2 <= UI_WIDTH && s_rects[n].y2 <= UI_HEIGHT) n++;
            }
        }
        cx += 8 * scale;
    }
    if (n) dev->Clear(n, s_rects, D3DCLEAR_TARGET, color, 1.0f, 0);
}

void UiCircle(IDirect3DDevice9 *dev, int cx, int cy, int r, D3DCOLOR color)
{
    D3DRECT rows[64];
    int n = 0, dy;
    if (r <= 0 || r > 32) return;
    for (dy = -r; dy < r; dy++)
    {
        // Half-width of the row through the middle of this pixel row.
        float fy = dy + 0.5f;
        int hw = 0;
        while ((hw + 0.5f) * (hw + 0.5f) + fy * fy <= (float)(r * r)) hw++;
        if (!hw) continue;
        rows[n].x1 = cx - hw;
        rows[n].x2 = cx + hw;
        rows[n].y1 = cy + dy;
        rows[n].y2 = cy + dy + 1;
        if (rows[n].x1 >= 0 && rows[n].y1 >= 0 && rows[n].x2 <= UI_WIDTH && rows[n].y2 <= UI_HEIGHT) n++;
    }
    if (n) dev->Clear(n, rows, D3DCLEAR_TARGET, color, 1.0f, 0);
}

// ---- Button prompts (style of V1's xenon/gui.cpp, at 720p) ----
#define BTN_R 18
#define PILL_H 32
#define COL_BTN_LABEL  D3DCOLOR_XRGB(255, 255, 255)
#define COL_BTN_SHADOW D3DCOLOR_XRGB(20, 20, 24)
#define COL_BTN_GREY   D3DCOLOR_XRGB(78, 82, 96)
#define COL_BTN_RIM    D3DCOLOR_XRGB(44, 46, 56)
#define COL_HINT       D3DCOLOR_XRGB(140, 140, 160)
#define COL_LINE       D3DCOLOR_XRGB(220, 40, 30)

static int button_glyph(IDirect3DDevice9 *dev, int x, int cy, const char *name)
{
    D3DCOLOR fill = 0, rim = 0;
    if (!strcmp(name, "A"))      { fill = D3DCOLOR_XRGB(96, 184, 46);  rim = D3DCOLOR_XRGB(52, 112, 22); }
    else if (!strcmp(name, "B")) { fill = D3DCOLOR_XRGB(222, 42, 38);  rim = D3DCOLOR_XRGB(132, 20, 18); }
    else if (!strcmp(name, "X")) { fill = D3DCOLOR_XRGB(36, 118, 222); rim = D3DCOLOR_XRGB(18, 64, 132); }
    else if (!strcmp(name, "Y")) { fill = D3DCOLOR_XRGB(246, 192, 22); rim = D3DCOLOR_XRGB(150, 110, 8); }
    if (fill)
    {
        // Face button: coloured disc with a darker rim and the letter.
        int cx = x + BTN_R;
        UiCircle(dev, cx, cy, BTN_R, rim);
        UiCircle(dev, cx, cy - 1, BTN_R - 3, fill);
        UiText(dev, cx - 6, cy - 6, 2, COL_BTN_SHADOW, name);
        UiText(dev, cx - 8, cy - 8, 2, COL_BTN_LABEL, name);
        return BTN_R * 2;
    }
    if (!strcmp(name, "DPAD"))
    {
        int cx = x + BTN_R;
        UiRect(dev, cx - 16, cy - 6, 32, 12, COL_BTN_RIM);
        UiRect(dev, cx - 6, cy - 16, 12, 32, COL_BTN_RIM);
        UiRect(dev, cx - 14, cy - 4, 28, 8, D3DCOLOR_XRGB(230, 230, 230));
        UiRect(dev, cx - 4, cy - 14, 8, 28, D3DCOLOR_XRGB(230, 230, 230));
        return BTN_R * 2;
    }
    {
        // Bumpers, BACK, START: grey pill with the name.
        int r = PILL_H / 2, w = UiTextWidth(2, name) + PILL_H, top = cy - r;
        UiCircle(dev, x + r, cy, r, COL_BTN_RIM);
        UiCircle(dev, x + w - r, cy, r, COL_BTN_RIM);
        UiRect(dev, x + r, top, w - PILL_H, PILL_H, COL_BTN_RIM);
        UiCircle(dev, x + r, cy, r - 3, COL_BTN_GREY);
        UiCircle(dev, x + w - r, cy, r - 3, COL_BTN_GREY);
        UiRect(dev, x + r, top + 3, w - PILL_H, PILL_H - 6, COL_BTN_GREY);
        UiText(dev, x + r, cy - 8, 2, COL_BTN_LABEL, name);
        return w;
    }
}

int UiFooter(IDirect3DDevice9 *dev, int x, int cy, const char *hints)
{
    char buf[256], *item, *nextItem;
    int x0 = x;
    strncpy(buf, hints, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (item = buf; item && *item; item = nextItem)
    {
        char *label, *btn, *nextBtn;
        nextItem = strchr(item, '|');
        if (nextItem) *nextItem++ = 0;
        label = strchr(item, ':');
        if (label) *label++ = 0;
        for (btn = item; btn && *btn; btn = nextBtn)   // "LB/RB": glyphs sharing one label
        {
            nextBtn = strchr(btn, '/');
            if (nextBtn) *nextBtn++ = 0;
            x += button_glyph(dev, x, cy, btn) + 6;
        }
        if (label)
        {
            x += 6;
            UiText(dev, x, cy - 8, 2, COL_HINT, label);
            x += UiTextWidth(2, label);
        }
        x += 36;
    }
    return x - x0;
}

void UiScreenFooter(IDirect3DDevice9 *dev, const char *hints)
{
    UiRect(dev, 80, 640, 1120, 2, COL_LINE);
    UiFooter(dev, 96, 672, hints);
}

int UiTextWidth(int scale, const char *text)
{
    int w = 0, best = 0;
    for (; *text; text++)
    {
        if (*text == '\n') { w = 0; continue; }
        w += 8 * scale;
        if (w > best) best = w;
    }
    return best;
}

// ---- Menu input ----
#define REPEAT_FIRST_MS 350
#define REPEAT_NEXT_MS 90
static const WORD REPEATS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;

void UiInputInit(UiInput *in)
{
    XINPUT_STATE st;
    in->held = in->prev = 0;
    in->repeatAt = 0;
    // Buttons already down (the press that opened the menu) do not count.
    if (XInputGetState(0, &st) == ERROR_SUCCESS) in->prev = st.Gamepad.wButtons;
}

WORD UiInputPoll(UiInput *in)
{
    XINPUT_STATE st;
    WORD b = 0, down;
    DWORD now = GetTickCount();
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
    {
        b = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLY > 20000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -20000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX < -20000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (st.Gamepad.sThumbLX > 20000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }
    down = b & ~in->prev;
    if (down & REPEATS) in->repeatAt = now + REPEAT_FIRST_MS;
    else if ((b & REPEATS) && (s32)(now - in->repeatAt) >= 0)
    {
        down |= b & REPEATS;
        in->repeatAt = now + REPEAT_NEXT_MS;
    }
    in->prev = b;
    in->held = b;
    return down;
}
