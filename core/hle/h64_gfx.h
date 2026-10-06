// Harissa64 V2 - graphics task HLE: runs the display list of a graphics
// OSTask in C++ (the job of the F3D/F3DEX/F3DEX2 microcodes on the RSP) and
// sends the result to the renderer (render/api.h): RDP commands, and
// triangles as screen-space vertices.
//
// Modelled on GLideN64's gSP and microcode handlers (GPL v2, studied and
// followed, see THIRD_PARTY.md); the output side is V2's own: everything is
// expressed as RDP state and triangles, so the software RDP draws HLE
// output too and the two RSP paths can be compared image by image.
//
// A task whose microcode is not recognised, or which switches to one
// (G_LOAD_UCODE), is left to the LLE RSP. RDP output is buffered during the
// task and only sent when the whole display list ran, so a task can still
// fall back to LLE half-way.
#ifndef H64_GFX_H
#define H64_GFX_H

#include "../common/h64_types.h"

struct H64System;
struct H64Gfx;

H64Gfx *h64_gfx_create(H64System *sys);
void h64_gfx_free(H64Gfx *gfx);

// Runs the graphics task in DMEM. Returns 1 when done (`*fullSync` tells
// whether the display list ended with an RDP full sync), 0 to leave the
// task to the LLE RSP.
int h64_gfx_run_task(H64System *sys, H64Gfx *gfx, int *fullSync);

#endif
