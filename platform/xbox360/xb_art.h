// Harissa64 - box art and game information for the ROM browser.
//
// Everything lives in <drive>:\art\games\ (game:\ on the console, cache:\ in
// Xenia), one pair of files per game, named by its cache key (the header's
// game code, "NSME"; h64_gameinfo_key):
//   NSME.png   the box art (libretro-thumbnails, as downloaded)
//   NSME.txt   what is known about the game (h64_gameinfo_format)
//   db\        the databases the matching uses (h64_gamedb_files)
// tools/fetch_game_art.py fills the same folder from a PC.
//
// A worker thread (hardware thread 3) reads, decodes and, when allowed,
// downloads; the UI thread never waits for it. The browser says which games
// it wants (the selection first, then its neighbours: those only from the
// files already there) and takes the results when they are ready.
#ifndef XB_ART_H
#define XB_ART_H

#include "../../core/cart/h64_gameinfo.h"
#include "../../core/common/h64_types.h"

// The size covers are decoded to (UI pixels: the browser draws them 1:1 or smaller).
#define ART_COVER_W 400
#define ART_COVER_H 232
// A tiny copy, drawn enlarged as a soft coloured glow behind the cover.
#define ART_GLOW_W 16
#define ART_GLOW_H 10

struct ArtResult
{
    char path[256];         // the ROM file
    char key[16];
    int hasInfo;            // `info` read from the cache (else only `key` is known)
    H64GameInfo info;
    u8 *cover;              // RGBA, coverW x coverH, or NULL
    int coverW, coverH;
    u8 *glow;               // RGBA, ART_GLOW_W x ART_GLOW_H, or NULL
};

struct ArtStatus
{
    int bulk;               // "download all" running
    int done, total;        // its progress
    int found, covers;      // games identified, box art present so far
    int offline;            // the last attempt found no network
    char busyPath[256];     // the ROM whose files are being downloaded now ("" when none)
};

// Starts the worker (first call); `drive` is "game" or "cache".
void ArtStart(const char *drive);
// Whether the selection's missing files may be downloaded.
void ArtSetNetwork(int allowed);
// While a game runs: the worker finishes what it is doing and waits.
void ArtPause(int paused);
// The ROMs the browser shows, the selection first (`count` up to 8). Replaces
// the previous list; results come once per ROM until ArtForgetAll.
void ArtWant(const char *const *paths, int count);
// The next result (1), to free with ArtResultFree.
int ArtTake(ArtResult *r);
void ArtResultFree(ArtResult *r);
// The browser dropped its copy of a ROM's result (or of all): sent again when wanted.
void ArtForget(const char *path);
void ArtForgetAll(void);
// Downloads what is missing for all these ROMs (copied), in the background.
void ArtBulkStart(const char *const *paths, int count);
void ArtGetStatus(ArtStatus *s);
// A message for the user once ("No network connection", the bulk summary).
int ArtTakeNote(char *text, int size);

#endif
