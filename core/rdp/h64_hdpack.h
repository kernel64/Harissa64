// "Harissa64 HD" texture pack file (.h64hd): a community hi-res texture pack
// converted for the console by h64hdconv (platform/host/h64hdconv.cpp).
// Everything is big-endian.
//
//   header   32 bytes: "H64HDPK1", version, entry count, index offset, data offset, 0, 0
//   index    count x 32 bytes, sorted by (texCrc, palCrc, fmt, size, flags)
//   data     per entry: DXT1 or DXT5 blocks, mip level 0 first, each level's
//            block rows packed; every 16-bit word byte-swapped (the Xbox 360
//            GPU reads DXT data as big-endian 16-bit words)
//
// An entry's key is the texture's Rice checksum (h64_rdp_hires.h). A
// "wildcard" entry (flags & H64HD_WILDCARD, texCrc 0) stands for any texture
// with that palette checksum, format and size (the packs' "#$#" names).
#ifndef H64_HDPACK_H
#define H64_HDPACK_H

#include "../common/h64_types.h"

#define H64HD_MAGIC "H64HDPK1"
#define H64HD_VERSION 1
#define H64HD_HEADER_BYTES 32
#define H64HD_ENTRY_BYTES 32

enum { H64HD_HAS_PAL = 1, H64HD_WILDCARD = 2 };

struct H64HdEntry
{
    u32 texCrc, palCrc;
    u8 fmt, size, flags, dxt;   // dxt: 1 (DXT1) or 5 (DXT5)
    u16 width, height;          // level 0, multiples of 4
    u8 mips, pad0;
    u16 pad1;
    u32 offset, bytes;          // from the start of the file
    u32 pad2;
};

#endif
