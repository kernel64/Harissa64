// Harissa64 V2 - menu languages (Xbox 360): English, Spanish, German, French,
// Arabic, Brazilian Portuguese and Russian.
//
// The texts are in platform/xbox360/lang/<code>.txt; tools/gen_lang.py checks
// them and writes xb_lang_table.h (T_* ids, UTF-8 strings, Arabic already
// joined). Tr(T_X) gives a text in the current language. The language is
// chosen once on the first start (LanguageScreen in xb_menu.cpp), kept as
// lang= in config\settings.ini and changed in Settings. Logs, ini keys, game
// names and information, and the About panel stay in English.
#ifndef XB_LANG_H
#define XB_LANG_H

#include "xb_lang_table.h"

enum { LANG_EN = 0, LANG_ES, LANG_DE, LANG_FR, LANG_AR, LANG_PT, LANG_RU };

// A text in the current language.
const char *Tr(int id);
// A text in language `lang` (the language list shows each one in its own).
const char *TrIn(int lang, int id);

int LangGet(void);
void LangSet(int lang);
// 1 when the current language is written right to left (the menus are mirrored).
int LangRtl(void);
// "en", "es", "de", "fr", "ar", "pt", "ru".
const char *LangCode(int lang);
// The language of a code (ini lang=), -1 when unknown.
int LangFromCode(const char *code);
// The console's own language when the menus have it, else English.
int LangFromConsole(void);

// A region's name ("Europe", from the game database or the cartridge's code)
// in the current language; other names come back as they are.
const char *TrRegion(const char *english);
// The names of the N64 controls (H64_N64_*) and of the Xbox 360 inputs
// (H64_SRC_*) in the current language.
const char *TrN64(int n64);
const char *TrSource(int src);

#endif
