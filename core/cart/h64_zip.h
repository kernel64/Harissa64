// Harissa64 V2 - ROMs inside .zip archives.
//
// Reads the archive through a callback (the whole file need not be in
// memory): the central directory locates the first N64 ROM (by its first
// word, any dump order, or else by its extension), and its data is inflated
// with zlib (third_party/zlib) or copied when stored. No Zip64, no
// encryption (neither is used for N64 ROMs).
#ifndef H64_ZIP_H
#define H64_ZIP_H

#include "../common/h64_types.h"

struct H64ZipReader
{
    void *user;
    u32 size;   // archive size in bytes
    // Reads `len` bytes at `offset`; returns the number of bytes read.
    u32 (*read)(void *user, u32 offset, void *buf, u32 len);
};

struct H64ZipEntry
{
    char name[256];
    u32 method;        // 0 stored, 8 deflate
    u32 compSize, size, crc;
    u32 localOffset;   // the local header
};

// The first entry that looks like an N64 ROM. Returns 0, or -1 (not a zip,
// no ROM inside, or an unsupported entry).
int h64_zip_find_rom(const H64ZipReader *r, H64ZipEntry *e);
// Inflates the first `len` bytes of the entry into `out` (len <= e->size).
// The CRC is checked when the whole entry is extracted. Returns 0 or -1.
int h64_zip_extract(const H64ZipReader *r, const H64ZipEntry *e, u8 *out, u32 len);

// Convenience for an archive in memory: user = an H64ZipMem.
struct H64ZipMem { const u8 *data; u32 size; };
u32 h64_zip_mem_read(void *user, u32 offset, void *buf, u32 len);

#endif
