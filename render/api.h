// Harissa64 V2 - renderer interface used by the core.
//
// The core produces two kinds of drawing work:
//  - raw RDP commands (1 to 22 big-endian 64-bit words, exactly as the RDP
//    receives them on its bus), from the LLE RSP or passed through by the
//    graphics HLE (state, texture loads, rectangles, syncs);
//  - triangles from the graphics HLE, as three screen-space vertices plus
//    the RDP state set by the preceding commands. They stand for the RDP
//    triangle command the real microcode would have built.
//
// Two renderers implement it: the software RDP (reference, render/soft),
// which turns HLE triangles into RDP triangle commands, and the Xenos
// renderer on the Xbox 360 (render/xenos), which draws them with the GPU.
// With no renderer installed the core uses the software RDP.
#ifndef H64_RENDER_API_H
#define H64_RENDER_API_H

#include "../core/common/h64_types.h"

struct H64RenderVertex
{
    float x, y;         // screen position in pixels (sub-pixel precision)
    float z;            // depth in RDP units: 0 .. 0x7FFF (integer part of the RDP's 15.16 Z)
    float invw;         // 1 / clip-space w (> 0), for perspective-correct texturing
    float r, g, b, a;   // shade colour, 0 .. 255
    float s, t;         // texture coordinates in s10.5 units (32 per texel), before perspective
};

enum
{
    H64_TRI_ZBUFFER = 1,   // the triangle carries depth (RDP triangle command bit 0)
    H64_TRI_TEXTURE = 2,   // bit 1
    H64_TRI_SHADE = 4      // bit 2
};

struct H64Renderer
{
    void *user;
    // One complete RDP command.
    void (*rdp)(void *user, const u64 *words, u32 count);
    // One triangle: `flags` is a set of H64_TRI_*, `tile` the RDP tile,
    // `levels` the mip-map level count (1: no mip-mapping). Vertex order
    // does not matter.
    void (*triangle)(void *user, const H64RenderVertex *v0, const H64RenderVertex *v1, const H64RenderVertex *v2,
                     u32 flags, u32 tile, u32 levels);
    // Optional: the machine state was replaced (save state loaded): drop
    // everything derived from RDRAM (cached frames, textures).
    void (*reset)(void *user);
};

#endif
