// Harissa64 V2 - game information for the ROM browser: matching a ROM with
// public game databases, and the small text files that cache the result.
//
// Sources (downloaded at run time, or by tools/fetch_game_art.py; never
// shipped with the emulator):
// - libretro-database (CC BY-SA 4.0), clrmamepro "dat" files: the No-Intro
//   list (name, region, serial = the header's game code, CRC-32 of the .z64
//   file) and the "metadat" lists keyed by that CRC (developer, publisher,
//   release year, genre, players). The No-Intro name is also the file name of
//   the box art in libretro-thumbnails.
// - GameDB-N64 (github.com/niemasd/GameDB-N64), N64.data.json keyed by serial
//   ("NUS-NSME-USA"): clean title, release date, rating.
// - Wikipedia's page summaries for a short description.
//
// Matching: the 4-character game code of the ROM header (e.g. "NSME") picks
// the No-Intro entries with that serial; the header's version byte picks the
// revision ("(Rev 1)"), and betas, demos, kiosk or Virtual Console dumps come
// last. ROMs without a usable code (homebrew, some prototypes) fall back to
// their file name, compared without punctuation or bracketed tags.
#ifndef H64_GAMEINFO_H
#define H64_GAMEINFO_H

#include "../common/h64_types.h"

struct H64GameInfo
{
    int found;              // 1: the game is in a database (else only the cover may be known)
    int cover;              // 1: box art in the cache, 0: none could be found, -1: not tried yet
    int wiki;               // 1: description from Wikipedia, 0: none found, -1: not tried yet
    char name[128];         // No-Intro name, "Super Mario 64 (USA)" (the box art's name)
    char title[96];         // shown title, "The Legend of Zelda: Ocarina of Time"
    char year[8];           // "1996"
    char date[16];          // "1996-09-26" when known
    char developer[64];
    char publisher[64];
    char genre[48];
    char players[8];        // "1", "1-4"
    char region[32];        // "USA", "Europe", "Japan, USA"
    char rating[24];        // "E / 3+" (GameDB-N64)
    char description[640];  // a few sentences (Wikipedia), ASCII
};

void h64_gameinfo_clear(H64GameInfo *info);

// The cache key of a ROM: its game code when it is 4 letters or digits
// ("NSME"), else "X" and the header CRC1 in hex. `out` holds 16 bytes.
void h64_gameinfo_key(const char *gameCode, u32 crc1, char *out);

// The region of a game code's last letter ("E" -> "USA"), "" when unknown.
const char *h64_gameinfo_region_name(char codeLetter);

// ---- Cache files: "key=value" lines ----
// Writes the fields of `info` (found=0 alone when not found). Returns the length.
int h64_gameinfo_format(const H64GameInfo *info, char *out, int outSize);
// Reads a file written by h64_gameinfo_format (unknown keys ignored). Returns 1
// when the text had a "found=" line.
int h64_gameinfo_parse(H64GameInfo *info, const char *text, u32 len);

// ---- Databases ----
// The files of the cache's "db" folder and where they are downloaded from
// (HTTPS). `json` 1: GameDB-N64's JSON, else a libretro dat.
struct H64GameDbFile { const char *file, *host, *path; int json; };
#define H64_GAMEDB_FILE_COUNT 7
extern const H64GameDbFile h64_gamedb_files[H64_GAMEDB_FILE_COUNT];
// Box art: https://H64_THUMB_HOST H64_THUMB_PATH <thumb name, URL-encoded>.png
#define H64_THUMB_HOST "raw.githubusercontent.com"
#define H64_THUMB_PATH "/libretro-thumbnails/Nintendo_-_Nintendo_64/master/Named_Boxarts/"
// Descriptions: https://H64_WIKI_HOST H64_WIKI_PATH <title, spaces as _, URL-encoded>
#define H64_WIKI_HOST "en.wikipedia.org"
#define H64_WIKI_PATH "/api/rest_v1/page/summary/"

struct H64GameDb;
H64GameDb *h64_gamedb_new(void);
void h64_gamedb_free(H64GameDb *db);
// Adds a libretro clrmamepro dat: the No-Intro list (games with a "name") or
// a metadat list (games with a "comment" and fields per ROM CRC). Returns the
// number of games read, -1 on a syntax error.
int h64_gamedb_add_dat(H64GameDb *db, const char *text, u32 len);
// Adds GameDB-N64's N64.data.json. Returns the number of games read.
int h64_gamedb_add_gamedb_json(H64GameDb *db, const char *text, u32 len);
int h64_gamedb_count(const H64GameDb *db);   // No-Intro games known

// Fills `info` for a ROM (`fileStem`: the file name without folder or
// extension, may be NULL). Returns 1 when found. Box art names to try, best
// first, go to `covers` (up to `maxCovers` of 128 bytes each; *coverCount).
int h64_gamedb_lookup(const H64GameDb *db, const char *gameCode, int version, const char *fileStem, H64GameInfo *info,
                      char (*covers)[128], int maxCovers, int *coverCount);

// ---- Helpers for the downloads ----
// libretro-thumbnails' file name for a No-Intro name: &*/:`<>?\| become _.
void h64_gameinfo_thumb_name(const char *name, char *out, int outSize);
// Percent-encodes everything but letters, digits and -._~ ("%20" for spaces).
void h64_url_encode(const char *text, char *out, int outSize);
// The Wikipedia pages to try for a game, best first, as URL paths after
// H64_WIKI_PATH ("Super_Mario_64", "..._(video_game)", "..._(1997_video_game)").
// Returns how many (up to `max`).
int h64_gameinfo_wiki_pages(const H64GameInfo *info, char (*out)[256], int max);
// A Wikipedia page summary (REST API JSON): when it describes a video game
// (its short description says so, not a series, not a disambiguation page), its extract cut
// to whole sentences within `outSize` and folded to ASCII. Returns 1 if used.
int h64_gameinfo_wiki_extract(const char *json, u32 len, char *out, int outSize);
// UTF-8 (with JSON \u escapes already decoded) to the ASCII the UI font has:
// accents dropped, typographic quotes and dashes made plain. In place.
void h64_ascii_fold(char *text);

#endif
