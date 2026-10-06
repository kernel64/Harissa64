// Harissa64 V2 - VI output.
//
// M1: a direct capture of the framebuffer the VI points at (origin, width,
// 16- or 32-bit pixels), without the VI's filtering, scaling or gamma, for
// screenshots and test ROM scoring. The real VI stage comes with M2.
#ifndef H64_VI_H
#define H64_VI_H

#include "../common/h64_types.h"

struct H64System;

// Writes RGB8 pixels (3 bytes each) into `rgb` (at least maxW * maxH * 3).
// Returns 0 and the image size, or -1 when the VI is blank.
int h64_vi_capture(H64System *sys, u8 *rgb, int maxW, int maxH, int *w, int *h);

#endif
