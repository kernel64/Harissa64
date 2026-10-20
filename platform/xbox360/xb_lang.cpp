// Harissa64 V2 - menu languages (see xb_lang.h).
#define XB_LANG_DEFINE_TABLES
#include "xb_lang.h"

#include <string.h>
#include <xtl.h>

#include "../../core/pif/h64_padmap.h"

static int s_lang = LANG_EN;

const char *TrIn(int lang, int id)
{
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_EN;
    if (id < 0 || id >= T_COUNT) return "";
    return s_langText[lang][id];
}

const char *Tr(int id) { return TrIn(s_lang, id); }
int LangGet(void) { return s_lang; }
void LangSet(int lang) { s_lang = lang >= 0 && lang < LANG_COUNT ? lang : LANG_EN; }
int LangRtl(void) { return s_lang == LANG_AR; }
const char *LangCode(int lang) { return s_langCode[lang >= 0 && lang < LANG_COUNT ? lang : LANG_EN]; }

int LangFromCode(const char *code)
{
    int i;
    for (i = 0; i < LANG_COUNT; i++)
        if (!_stricmp(code, s_langCode[i])) return i;
    return -1;
}

int LangFromConsole(void)
{
    switch (XGetLanguage())
    {
    case XC_LANGUAGE_SPANISH: return LANG_ES;
    case XC_LANGUAGE_GERMAN: return LANG_DE;
    case XC_LANGUAGE_FRENCH: return LANG_FR;
    case XC_LANGUAGE_PORTUGUESE: return LANG_PT;
    case XC_LANGUAGE_RUSSIAN: return LANG_RU;
    }
    return LANG_EN;
}

const char *TrRegion(const char *english)
{
    static const char *const names[] = { "USA", "Japan", "Europe", "Germany", "France", "Italy", "Spain", "Australia", "Asia",
                                         "Brazil", "China", "Korea", "Netherlands", "Scandinavia", "Canada" };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcmp(english, names[i])) return Tr(T_REGION_USA + i);
    return english;
}

const char *TrN64(int n64)
{
    switch (n64)
    {
    case H64_N64_DUP: return Tr(T_N64_DUP);
    case H64_N64_DDOWN: return Tr(T_N64_DDOWN);
    case H64_N64_DLEFT: return Tr(T_N64_DLEFT);
    case H64_N64_DRIGHT: return Tr(T_N64_DRIGHT);
    case H64_N64_CUP: return Tr(T_N64_CUP);
    case H64_N64_CDOWN: return Tr(T_N64_CDOWN);
    case H64_N64_CLEFT: return Tr(T_N64_CLEFT);
    case H64_N64_CRIGHT: return Tr(T_N64_CRIGHT);
    }
    return h64_padmap_n64_label(n64);   // A, B, Z, START, L, R
}

const char *TrSource(int src)
{
    switch (src)
    {
    case H64_SRC_DUP: return Tr(T_SRC_DUP);
    case H64_SRC_DDOWN: return Tr(T_SRC_DDOWN);
    case H64_SRC_DLEFT: return Tr(T_SRC_DLEFT);
    case H64_SRC_DRIGHT: return Tr(T_SRC_DRIGHT);
    case H64_SRC_LTHUMB: return Tr(T_SRC_LS);
    case H64_SRC_RTHUMB: return Tr(T_SRC_RS);
    case H64_SRC_LUP: return Tr(T_SRC_LUP);
    case H64_SRC_LDOWN: return Tr(T_SRC_LDOWN);
    case H64_SRC_LLEFT: return Tr(T_SRC_LLEFT);
    case H64_SRC_LRIGHT: return Tr(T_SRC_LRIGHT);
    case H64_SRC_RUP: return Tr(T_SRC_RUP);
    case H64_SRC_RDOWN: return Tr(T_SRC_RDOWN);
    case H64_SRC_RLEFT: return Tr(T_SRC_RLEFT);
    case H64_SRC_RRIGHT: return Tr(T_SRC_RRIGHT);
    }
    return h64_padmap_source_label(src);   // A, B, X, Y, LB, RB, LT, RT, START
}
