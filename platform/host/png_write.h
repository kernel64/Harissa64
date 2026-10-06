#ifndef H64_PNG_WRITE_H
#define H64_PNG_WRITE_H

#include "../../core/common/h64_types.h"

// Writes an 8-bit RGB image (w * h * 3 bytes). Returns 0 or -1.
int h64_png_write_rgb(const char *path, const u8 *rgb, int w, int h);

#endif
