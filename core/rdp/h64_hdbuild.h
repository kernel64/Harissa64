// Harissa64 V2 - builds Harissa64 HD files (h64_hdpack.h) from community
// hi-res texture packs: PNG pictures named the GLideN64 / Rice way
// (NAME#TEXCRC#FMT#SIZE[#PALCRC]_suffix.png).
//
// Shared by the PC converter (platform/host/h64hdconv.cpp) and the console,
// which converts a pack it downloaded. The pictures go to the file as they
// are compressed (a converted pack can be bigger than the console's memory);
// only the index stays in memory.
//
// PNG decoding: stb_image; DXT compression: stb_dxt (third_party/stb, public domain / MIT).
#ifndef H64_HDBUILD_H
#define H64_HDBUILD_H

#include "h64_hdpack.h"

// One converted picture: its index entry (offset and bytes filled by
// h64_hdbuild_add) and its DXT levels, malloc'd.
struct H64HdItem
{
    H64HdEntry e;
    u8 *data;
    u32 bytes;
};

// Whether `fileName` (no folder) is a picture of the ROM named `romName`
// (the cartridge's internal name, any case), and its key. Returns 1 or 0.
int h64_hdbuild_parse(const char *fileName, const char *romName, H64HdEntry *e);
// Whether a path inside a pack belongs to other ROMs or other emulators
// (folders "Hacks/", "iQue/", "Ports/", "rt64/"): those pictures are left out.
int h64_hdbuild_skip(const char *path);
// Decodes a PNG and makes its DXT1 (opaque) or DXT5 levels: sides halved
// down to maxSide, padded to multiples of 4, mip levels while the sides stay
// multiples of 8, 16-bit words byte-swapped for the console's linear DXT
// formats. Thread-safe. item->e must hold the key (h64_hdbuild_parse).
// Returns 0, or -1 (not a readable PNG, or out of memory).
int h64_hdbuild_make(const u8 *png, u32 len, int maxSide, H64HdItem *item);

struct H64HdBuilder;
// Starts the file `path` with room for `capacity` pictures. NULL: cannot write.
H64HdBuilder *h64_hdbuild_begin(const char *path, u32 capacity);
// Writes a picture (item->data is freed). Not thread-safe: callers serialise.
// Returns 0, or -1 (write error, no room left, or the file would pass 2 GB).
int h64_hdbuild_add(H64HdBuilder *b, H64HdItem *item);
// Sorts the index, drops repeated keys (the entry with the smallest item->e.pad2,
// the caller's order of the pictures, stays: pad2 is not written),
// writes the header and closes. Frees the builder. Returns 0 or -1.
int h64_hdbuild_end(H64HdBuilder *b, u32 *count, u32 *repeated, u32 *bytes);
// Closes and deletes the unfinished file. Frees the builder.
void h64_hdbuild_abort(H64HdBuilder *b);

#endif
