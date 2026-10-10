// Harissa64 V2 - screen drawing for the front end (Xbox 360): text, panels,
// button prompts, the logo, toasts.
//
// Everything is drawn as alpha-blended quads in one small batch per UiBegin /
// UiEnd pair, with one pixel shader: rounded rectangles, circles and rings are
// signed distance functions evaluated per pixel (anti-aliased edges, soft
// shadows), text and the pepper logo come from a signed distance field atlas
// (xb_ui_atlas.h, built offline by tools/gen_ui_atlas.py from Inter), so they
// stay smooth at any size. Coordinates are back-buffer pixels (1280x720),
// fractional values allowed.
//
// The renderer calls the overlay between its last draw and the present, so the
// UI may change any state it likes: h64_xenos_present_vi binds the renderer's
// vertex shader, declaration and textures again afterwards.
#ifndef XB_UI_H
#define XB_UI_H

#include <xtl.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720
// Title-safe area (5 % margins at 720p).
#define UI_SAFE_X 64
#define UI_SAFE_Y 36

// ---- Palette: dark neutrals and one accent (harissa red) ----
#define UI_COL_BG_TOP     D3DCOLOR_ARGB(255, 18, 19, 26)
#define UI_COL_BG_BOTTOM  D3DCOLOR_ARGB(255, 10, 11, 15)
#define UI_COL_PANEL      D3DCOLOR_ARGB(244, 27, 29, 38)
#define UI_COL_PANEL_TOP  D3DCOLOR_ARGB(244, 33, 35, 46)
#define UI_COL_CARD       D3DCOLOR_ARGB(255, 34, 36, 47)
#define UI_COL_LINE       D3DCOLOR_ARGB(26, 255, 255, 255)
#define UI_COL_TEXT       D3DCOLOR_ARGB(255, 243, 244, 247)
#define UI_COL_TEXT2      D3DCOLOR_ARGB(255, 164, 168, 182)
#define UI_COL_TEXT3      D3DCOLOR_ARGB(255, 112, 116, 132)
#define UI_COL_ACCENT     D3DCOLOR_ARGB(255, 234, 76, 58)
#define UI_COL_ACCENT_HI  D3DCOLOR_ARGB(255, 255, 112, 84)
#define UI_COL_SELECT     D3DCOLOR_ARGB(64, 234, 76, 58)
#define UI_COL_SHADOW     D3DCOLOR_ARGB(150, 0, 0, 0)
#define UI_COL_DIM        D3DCOLOR_ARGB(150, 4, 5, 8)

enum { UI_REGULAR = 0, UI_BOLD = 1 };
enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };

// Starts a batch on `dev` (the bound render target, normally the back buffer).
// The first call creates the atlas texture and the shaders.
void UiBegin(IDirect3DDevice9 *dev);
// Draws what is batched; leaves blending off.
void UiEnd(void);
// Seconds since the first UI call (animations).
float UiTime(void);

// ---- Shapes ----
// Rounded rectangle with a vertical gradient (top colour, bottom colour).
void UiRoundRect(float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom);
void UiRect(float x, float y, float w, float h, D3DCOLOR color);
// The outline of a rounded rectangle, `thick` pixels inside its edge.
void UiRoundRing(float x, float y, float w, float h, float r, float thick, D3DCOLOR color);
// A soft shadow of a rounded rectangle, `blur` pixels wide.
void UiShadow(float x, float y, float w, float h, float r, float blur, D3DCOLOR color);
void UiCircle(float cx, float cy, float r, D3DCOLOR color);
// Full-screen backdrop of the menus (gradient and a warm glow).
void UiBackdrop(void);
// The translucent veil over the frozen game frame.
void UiDim(void);
// A panel: shadow, translucent body, a fine light edge.
void UiPanel(float x, float y, float w, float h, float r);

// ---- Text ----
// Draws `text` with the vertical middle of its capitals at `cy` (later lines
// of a "\n"-separated text 1.35 sizes lower), aligned on x by `align`.
// Codes 0x80.. are extra glyphs: UI_CH_* below. Returns the widest line's width.
float UiText(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
// The same with a soft drop shadow under it.
float UiTextShadowed(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
float UiTextWidth(int font, float size, const char *text);
// Copies `text` into `out`, cut with an ellipsis to fit `maxW` pixels.
void UiTextFit(int font, float size, float maxW, const char *text, char *out, int outSize);
// Word-wraps `text` into `out` ("\n" between lines) to fit `maxW`; at most
// `maxLines` lines, the last one cut with an ellipsis. Returns the line count.
int UiTextWrap(int font, float size, float maxW, int maxLines, const char *text, char *out, int outSize);
#define UI_CH_LCHEVRON "\x80"
#define UI_CH_RCHEVRON "\x81"
#define UI_CH_ELLIPSIS "\x82"
#define UI_CH_BULLET   "\x83"
#define UI_CH_MIDDOT   "\x84"

// The Harissa pepper, `size` pixels square, top-left at (x, y). Returns `size`.
float UiLogo(float x, float y, float size);
// The logo, "Harissa64" and the V2 badge; (x, cy) left end and vertical
// middle, `size` the title's size. Returns the width.
float UiBrand(float x, float cy, float size);

// ---- Button prompts ----
// "A:Play|B:Back|LB/RB:Page|DPAD:Change": each button's glyph (coloured disc
// for A/B/X/Y, a pill for LB, RB, BACK, START, a cross for DPAD) and its label,
// from (x, cy) (cy is the vertical centre). Returns the width.
float UiFooter(float x, float cy, const char *hints);
float UiFooterWidth(const char *hints);
// The footer of a full screen: a fine line and the prompts in the safe area.
void UiScreenFooter(const char *hints);

// ---- Notifications ----
// A toast at the bottom centre; `age` and `life` in ms (it slides in and fades out).
void UiToast(const char *text, float age, float life);
// The frames-per-second badge in the top-right corner.
void UiFpsBadge(const char *text);

// Controller 1 for menus: buttons pressed since the last call, with
// auto-repeat for the D-pad and the left stick (as D-pad directions).
struct UiInput
{
    WORD held, prev;
    DWORD repeatAt;
};
void UiInputInit(UiInput *in);
WORD UiInputPoll(UiInput *in);   // XINPUT_GAMEPAD_* bits newly pressed (or repeating)

#endif
