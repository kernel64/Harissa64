// Harissa64 V2 - Xenos renderer (Xbox 360, Direct3D 9 of the XDK).
//
// Implements render/api.h with the GPU. The software RDP runs in "state
// only" mode (sys->options.rdpStateOnly) and keeps the RDP state and TMEM;
// this renderer reads them to draw:
//  - HLE triangles (screen-space vertices) and texture/fill rectangles,
//  - with pixel shaders generated from the colour combiner (compiled at run
//    time with D3DXCompileShader, cached per combiner mode),
//  - textures decoded texel by texel from TMEM (h64_rdp_fetch_texel), so
//    every format, palette, mask, mirror and clamp follows the RDP,
//  - blender modes mapped to GPU blending, depth test and alpha compare.
// N64 frames are drawn into an EDRAM render target of their own, resolved
// into one texture per RDRAM colour image, and the one the VI shows is
// drawn to the back buffer and presented at each VI interrupt. Frames the
// CPU wrote itself (no RDP drawing at that address) are shown from RDRAM.
//
// Xbox lessons applied (V1 CLAUDE.md): never leave a texture unit empty,
// never release a texture the GPU may still read, DrawPrimitiveUP only,
// linear textures for CPU uploads (0xAARRGGBB words on the big-endian CPU).
#ifndef H64_XENOS_H
#define H64_XENOS_H

#include "../api.h"

struct H64System;
struct IDirect3DDevice9;

// Creates the renderer on an existing device (640x480 back buffer created
// without an automatic depth buffer), and puts the software RDP in state
// only mode. Returns NULL on failure.
H64Renderer *h64_xenos_create(H64System *sys, IDirect3DDevice9 *dev);
void h64_xenos_free(H64Renderer *r);

// At each VI interrupt: shows the frame the VI is scanning out and presents.
void h64_xenos_present(H64Renderer *r);

// Saves the frame shown at the last present as a 24-bit BMP (waits for the
// GPU). Returns 0 on success.
int h64_xenos_save_frame(H64Renderer *r, const char *path);

// Debug output of combined primitives: 0 normal, 1 solid red, 2 shade
// colour only, 3 texture 0 only.
void h64_xenos_set_debug(H64Renderer *r, int mode);

struct H64XenosStats
{
    u32 triangles, rects, fills, draws;
    u32 textureUploads, shaderCompiles, presents, copyBacks;
};
void h64_xenos_stats(H64Renderer *r, H64XenosStats *out, int reset);

#endif
