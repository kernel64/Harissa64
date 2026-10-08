// Harissa64 V2 - screen text and panels for the front end (Xbox 360).
//
// Everything is drawn with IDirect3DDevice9::Clear rectangles on the bound
// render target (no textures, no shaders): an 8x8 bitmap font, each row of a
// glyph merged into runs of lit pixels. Coordinates are back-buffer pixels
// (1280x720).
#ifndef XB_UI_H
#define XB_UI_H

#include <xtl.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720

// Text at (x, y), each font pixel `scale` x `scale`; '\n' starts a new line.
void UiText(IDirect3DDevice9 *dev, int x, int y, int scale, D3DCOLOR color, const char *text);
// Width in pixels of the text's longest line.
int UiTextWidth(int scale, const char *text);
void UiRect(IDirect3DDevice9 *dev, int x, int y, int w, int h, D3DCOLOR color);
void UiCircle(IDirect3DDevice9 *dev, int cx, int cy, int r, D3DCOLOR color);

// Button prompts as in V1's menus: "A:Play|B:Back|LB/RB:Page|DPAD:Change"
// draws each button's glyph (coloured disc for A/B/X/Y, grey pill for LB, RB,
// BACK, START, a cross for DPAD) followed by its label, from (x, cy) (cy is
// the vertical centre). Returns the width drawn.
int UiFooter(IDirect3DDevice9 *dev, int x, int cy, const char *hints);
// The usual footer of a full screen: a line, then the prompts.
void UiScreenFooter(IDirect3DDevice9 *dev, const char *hints);

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
