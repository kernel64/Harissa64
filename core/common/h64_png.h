// Harissa64 V2 - PNG decoding and image downscaling, for the box art shown by
// the ROM browser (the pictures come from the network or the user's files:
// every size and offset is checked, a bad file is only refused).
//
// Supports every PNG colour type (grey, RGB, palette, grey + alpha, RGBA), bit
// depths 1 to 16, tRNS transparency and Adam7 interlacing. Ancillary chunks
// (gamma, colour profiles, text) are ignored. Inflate is zlib's.
#ifndef H64_PNG_H
#define H64_PNG_H

#include "h64_types.h"

// Decodes `data` into 8-bit RGBA (bytes R, G, B, A per pixel, rows top to
// bottom, no padding). Returns a malloc'd buffer of w*h*4 bytes, or NULL when
// the file is not a valid PNG or has more than `maxPixels` pixels.
u8 *h64_png_decode(const u8 *data, u32 size, int *w, int *h, u32 maxPixels);

// The largest size of `w` x `h` that fits in `maxW` x `maxH` keeping the
// aspect ratio (never larger than the source, at least 1 x 1).
void h64_image_fit(int w, int h, int maxW, int maxH, int *outW, int *outH);

// Box-filter downscale of an RGBA image to `dw` x `dh` (each destination
// pixel averages the source pixels it covers; alpha-weighted colours).
// Returns a malloc'd buffer, or NULL.
u8 *h64_image_scale(const u8 *src, int sw, int sh, int dw, int dh);

#endif
