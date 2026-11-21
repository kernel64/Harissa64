// Harissa64 V2 - graphics HLE check (h64test --hle-gfx-check DIR), level B of
// the render tests (docs-local/render_tests_study.md).
//
// The game runs its graphics tasks on the LLE RSP (the real microcode) and the
// software RDP draws them. At each graphics task start, a shadow system (RDRAM,
// DMEM, RDP state and hidden RDRAM copied) runs the same task through the
// graphics HLE and draws it with its own software RDP. When the RSP halts, the
// images both drew are compared pixel by pixel, and the state commands each
// sent (other modes, combiner, tiles...) are compared as sets: a difference
// there names what the HLE got wrong (Top Gear Rally's other-mode word).
// Tasks whose pictures differ are listed; the first ones get a PNG
// (LLE | HLE | heat map) in DIR.
#ifndef H64_GFXCHECK_H
#define H64_GFXCHECK_H

struct H64System;

int h64_gfxcheck_begin(H64System *sys, const char *dir, int maxPictures);
void h64_gfxcheck_end(H64System *sys);   // prints the summary

#endif
