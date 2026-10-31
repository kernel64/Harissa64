// Harissa64 - hi-res texture packs on the console: the catalogue
// (docs/texture-packs.json), downloading a pack from its author's page,
// checking it, converting it to a Harissa64 HD file (core/rdp/h64_hdbuild.h)
// in <drive>:\hd_textures\<game name>.h64hd, and removing it.
//
// One job at a time, on a thread of its own; the conversion shares its work
// with three more threads. HdPause(1) when a game starts: the job waits
// (between pictures, or before the next block of a download) until HdPause(0).
#ifndef XB_HD_H
#define XB_HD_H

#include "../../core/cart/h64_hdcatalog.h"

enum HdStep { HD_IDLE = 0, HD_CATALOG, HD_DOWNLOAD, HD_CHECK, HD_CONVERT, HD_DONE, HD_FAILED, HD_STOPPED };

struct HdStatus
{
    int step;                 // HdStep of the current or last job
    u32 done, total;          // bytes (download, check) or pictures (conversion)
    char game[64];            // the game file name the job is for (no extension)
    char pack[64];            // the pack's name
    char error[96];           // HD_FAILED: what went wrong (English, for the log; the UI shows its own text)
    int errorCode;            // HD_FAILED: HD_ERR_*
    int catalogState;         // 0 not asked, 1 being read, 2 read, 3 failed
};

enum { HD_ERR_NONE = 0, HD_ERR_NETWORK, HD_ERR_SPACE, HD_ERR_CHECKSUM, HD_ERR_ZIP, HD_ERR_WRITE, HD_ERR_EMPTY };

// Once, from the ROM list: where hd_textures\ is ("game", "hdd"...), and the
// catalogue's address (NULL: the one on GitHub). A file
// <drive>:\hd_textures\texture-packs.json is read instead when present.
void HdStart(const char *drive, const char *catalogUrl);
// Reads the catalogue (in the background, once; again with refresh).
void HdCatalogRequest(int refresh);
// The packs of the catalogue made for a game code (4 characters).
void HdCatalogFor(const char *gameCode, std::vector<H64HdPack> *out);
// "<drive>:\hd_textures\<name>.h64hd" for a save folder name ("SUPER MARIO 64 NSME" -> "SUPER MARIO 64").
void HdPackFile(const char *folderName, char *out, int size);
// The installed pack's size in MB for that save folder name (0: none).
u32 HdInstalledMB(const char *folderName);
// Downloads, checks and converts pack p for the game (save folder name). 0: another job is running.
int HdInstall(const H64HdPack *p, const char *folderName);
// Removes the installed pack of a game. Returns 1 when a file was removed.
int HdRemove(const char *folderName);
// Stops the running job (a download stopped is kept and resumed next time).
void HdStop(void);
void HdPause(int paused);
void HdGetStatus(HdStatus *s);

#endif
