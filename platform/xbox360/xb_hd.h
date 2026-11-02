// Harissa64 - hi-res texture packs on the console: the catalogue
// (docs/texture-packs.json), downloading a pack from its author's page,
// checking it, converting it to a Harissa64 HD file (core/rdp/h64_hdbuild.h),
// the packs installed, which one a game uses, and removing them.
//
// Installed packs live in <drive>:\hd_textures\packs\: <id>.h64hd (the
// pictures) and <id>.txt (name, author, version, games, rom: what the page
// shows without the catalogue). One pack serves every game it suits: the
// catalogue's game codes (every region) or the cartridge name its pictures
// carry (hacks keep one or the other), downloaded once.
//
// A game's own file is still read: <drive>:\hd_textures\<game name>.h64hd or
// "<game name> <code>.h64hd" (made on a PC with tools/hd_textures.py, or by
// earlier versions of the page).
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
    char id[48];              // the job's pack id
    char pack[64];            // the pack's name
    char error[96];           // HD_FAILED: what went wrong (English, for the log; the UI shows its own text)
    int errorCode;            // HD_FAILED: HD_ERR_*
    int catalogState;         // 0 not asked, 1 being read, 2 read, 3 failed
};

enum { HD_ERR_NONE = 0, HD_ERR_NETWORK, HD_ERR_SPACE, HD_ERR_CHECKSUM, HD_ERR_ZIP, HD_ERR_WRITE, HD_ERR_EMPTY };

// An installed pack (from its .txt).
struct HdInstalled
{
    std::string id, name, author, version, games, rom;
    u32 mb;                   // the .h64hd's size
};

// What a game uses (HdResolve).
enum { HD_USE_NONE = 0, HD_USE_PACK, HD_USE_FILE };
#define HD_CHOICE_FILE "file"   // hdpack= value for the game's own file

// Once, from the ROM list: where hd_textures\ is ("game", "hdd"...), and the
// catalogue's address (NULL: the one on GitHub). A file
// <drive>:\hd_textures\texture-packs.json is read instead when present.
void HdStart(const char *drive, const char *catalogUrl);
const char *HdDrive(void);
// Reads the catalogue (in the background, once; again with refresh).
void HdCatalogRequest(int refresh);
// The catalogue's packs that suit a game (game code, cartridge name).
void HdCatalogFor(const char *gameCode, const char *romName, std::vector<H64HdPack> *out);

// The packs installed on `drive` (any game).
void HdListInstalled(const char *drive, std::vector<HdInstalled> *out);
int HdInstalledFits(const HdInstalled *p, const char *gameCode, const char *romName);
// The game's own file ("SUPER MARIO 64 NSME" save folder: "SUPER MARIO 64.h64hd", then
// "SUPER MARIO 64 NSME.h64hd"): 1 and its path when there is one; its size in MB in *mb.
int HdOwnFile(const char *drive, const char *folderName, char *path, int size, u32 *mb);
// The pack a game uses, from hdpack= (`choice`: an id, HD_CHOICE_FILE, or ""
// for the first found): the chosen one when installed, else the game's own
// file, else the first installed pack that suits it. Returns HD_USE_*; the
// file in path, the pack's id (or HD_CHOICE_FILE) in id.
int HdResolve(const char *drive, const char *folderName, const char *gameCode, const char *romName, const char *choice,
              char *path, int size, char *id, int idSize);

// Downloads, checks and converts pack p (into packs\). 0: another job is running.
int HdInstall(const H64HdPack *p);
// Removes an installed pack, or the game's own file. 1 when a file was removed.
int HdRemovePack(const char *id);
int HdRemoveOwn(const char *folderName);
// Stops the running job (a download stopped is kept and resumed next time).
void HdStop(void);
void HdPause(int paused);
void HdGetStatus(HdStatus *s);

#endif
