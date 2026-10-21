// Harissa64 V2 - front-end menus (Xbox 360): a generic list panel, the ROM
// browser shown at start-up and the settings screen.
#ifndef XB_MENU_H
#define XB_MENU_H

#include <xtl.h>

#include "xb_config.h"

#define UI_MENU_MAX 16

struct UiMenu
{
    char title[128];
    char label[UI_MENU_MAX][96];   // UTF-8 (xb_lang.h)
    char value[UI_MENU_MAX][64];   // shown right-aligned ("< 250 ms >"), empty for actions
    int count, sel;
    char footer[192];
    int page;                      // UI_PAGE_*: how UiMenuDraw shows it
};
enum { UI_PAGE_LIST = 0, UI_PAGE_CONTROLS };

void UiMenuClear(UiMenu *m, const char *title, const char *footer);
int UiMenuAdd(UiMenu *m, const char *label, const char *value);   // returns the item's index
// A centred panel over whatever is on the target (the frozen game frame in game).
void UiMenuDraw(const UiMenu *m);   // between UiBegin and UiEnd
// Up/down move the selection (wrapping).
void UiMenuNavigate(UiMenu *m, WORD down);

// ---- Settings (cpu, hle, audio margin, FPS display, the Graphics and Controls pages) ----
enum { SET_NONE = 0, SET_CHANGED, SET_BACK, SET_SAVE_ALL, SET_SAVE_GAME, SET_ART_ALL, SET_LANGUAGE };
// Fills the menu from *c; `perGame` adds "Save for this game".
void SettingsBuild(UiMenu *m, const Config *c, int perGame);
// Applies one input to *c and the menu; returns SET_*. The Language row
// changes the language at once (and keeps it in the file given to
// SettingsSetLangFile); from the ROM list, A on it returns SET_LANGUAGE (the
// list with the flags).
int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down);
// The menu settings file where a language change is kept (lang=); "" for none.
void SettingsSetLangFile(const char *path);

// ---- Language ----
// The list of languages with their flags, full screen: returns the language
// chosen with A (LANG_*), or -1 when B leaves it (never on the first start,
// `firstStart`, where a language must be chosen). Starts on `initial`.
int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart);
// Checks (ini langauto=): the list confirms its selection by itself after `ms` ms (0: never).
void LanguageScreenAuto(int ms);

// Reads a ROM file, or the first N64 ROM inside a .zip. Returns a malloc'd
// buffer, or NULL.
u8 *RomFileLoad(const char *path, u32 *size);

// The header of a ROM file (plain or zipped) in *rom (free with h64_rom_free).
// Returns the ROM size, 0 when it is not an N64 ROM. Any thread.
struct H64Rom;
u32 RomReadHeader(const char *path, H64Rom *rom);

// The game's save/profile folder name (h64_save_folder_name) from the ROM
// file's header. Returns 0 when the file is not an N64 ROM.
int RomFolderName(const char *path, char *out, size_t size);

// ---- About ----
// The credits panel (centred, over a veil), and a full screen showing it until B.
void AboutDraw(void);   // between UiBegin and UiEnd
void AboutScreen(IDirect3DDevice9 *dev);

// ---- ROM browser ----
// Lists game:\roms\ (.z64 .n64 .v64 .zip). Returns 1 with the chosen path, 0 when
// the user leaves to the dashboard. Y opens the settings for all games,
// saved to `settingsPath` (also where the last selection is kept).
int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize);

#endif
