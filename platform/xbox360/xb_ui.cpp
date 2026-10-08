// Harissa64 V2 - screen text and panels (see xb_ui.h).
#include "xb_ui.h"

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
