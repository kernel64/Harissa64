// Harissa64 V2 - the hi-res texture pack catalogue (docs/texture-packs.json).
//
// The catalogue lists packs by link only: each one stays on its author's
// page. Every member is a string:
//
//   { "format": "1",
//     "sm64-reloaded": { "name": "SM64 Reloaded", "games": "NSM", "rom": "SUPER MARIO 64",
//                        "author": "...", "version": "2.6.0", "page": "https://...",
//                        "url": "https://.../pack.zip", "size": "280611003", "sha256": "e4ae..." } }
//
// "games": game codes without the region letter (comma-separated); "rom": the
// name the pack's PNG pictures start with; "url": a .zip of PNG pictures over
// HTTPS; "size": its bytes (under 4 GB); "sha256": its checksum (may be empty:
// the size is then the only check).
#ifndef H64_HDCATALOG_H
#define H64_HDCATALOG_H

#include "../common/h64_types.h"
#include <string>
#include <vector>

struct H64HdPack
{
    std::string id, name, games, rom, author, version, page, url, sha256;
    u32 size;
};

// Reads the catalogue. Packs missing a name, rom, https url or size are left
// out. Returns the number of packs, or -1 when the text is not a catalogue.
int h64_hdcat_parse(const char *json, u32 len, std::vector<H64HdPack> *out);
// Whether the pack is made for the game code (4 characters, the region letter
// is not compared).
int h64_hdcat_fits(const H64HdPack *p, const char *gameCode);
// The same, or the cartridge's name (header, trimmed) is the one the pack's
// pictures carry, case aside: a hack keeps the name or the code of its game.
int h64_hdcat_fits_rom(const H64HdPack *p, const char *gameCode, const char *romName);

#endif
