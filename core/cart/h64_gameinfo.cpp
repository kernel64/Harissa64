// Harissa64 V2 - game information for the ROM browser (see h64_gameinfo.h).
#include "h64_gameinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

// ---- Small string helpers ----
static void copy_str(char *dst, int size, const char *src)
{
    int n = 0;
    if (size <= 0) return;
    for (; src && src[n] && n < size - 1; n++) dst[n] = src[n] == '\r' || src[n] == '\n' ? ' ' : src[n];
    dst[n] = 0;
}

static int is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (lower(*s) != lower(*prefix)) return 0;
    return 1;
}

static int contains_ci(const char *s, const char *word)
{
    for (; *s; s++)
        if (starts_with_ci(s, word)) return 1;
    return 0;
}

void h64_gameinfo_clear(H64GameInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->cover = -1;
    info->wiki = -1;
}

void h64_gameinfo_key(const char *gameCode, u32 crc1, char *out)
{
    int i, ok = gameCode != NULL;
    for (i = 0; ok && i < 4; i++)
        ok = gameCode[i] && ((gameCode[i] >= 'A' && gameCode[i] <= 'Z') || (gameCode[i] >= '0' && gameCode[i] <= '9'));
    if (ok && !gameCode[4])
    {
        memcpy(out, gameCode, 4);
        out[4] = 0;
    }
    else
        sprintf(out, "X%08X", (unsigned)crc1);
}

const char *h64_gameinfo_region_name(char c)
{
    switch (c)
    {
    case 'E': return "USA";
    case 'J': return "Japan";
    case 'P': case 'X': case 'Y': case 'Z': return "Europe";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'I': return "Italy";
    case 'S': return "Spain";
    case 'U': return "Australia";
    case 'A': return "Asia";
    case 'B': return "Brazil";
    case 'C': return "China";
    case 'K': return "Korea";
    case 'H': return "Netherlands";
    case 'W': return "Scandinavia";
    case 'N': return "Canada";
    case 'G': return "Gateway 64 (NTSC)";
    case 'L': return "Gateway 64 (PAL)";
    }
    return "";
}

// ---- ASCII folding ----
// Latin-1 letters U+00C0..U+00FF without their accents ('?' where no letter fits).
static const char s_latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static void append_cp(std::string *s, u32 cp)
{
    if (cp < 0x80) { *s += (char)cp; return; }
    if (cp >= 0xC0 && cp <= 0xFF) { *s += s_latin1[cp - 0xC0]; if (cp == 0xC6) *s += 'E'; else if (cp == 0xE6) *s += 'e'; else if (cp == 0xDF) *s += 's'; return; }
    switch (cp)
    {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: *s += '\''; return;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: *s += '"'; return;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: *s += '-'; return;
    case 0x2026: *s += "..."; return;
    case 0x00A0: case 0x2009: case 0x200A: case 0x202F: *s += ' '; return;
    case 0x00D7: *s += 'x'; return;
    case 0x0152: *s += "OE"; return;
    case 0x0153: *s += "oe"; return;
    case 0x00AE: case 0x2122: case 0x00A9: case 0x00B0: case 0x200B: return;   // (R), TM, (C), degree: dropped
    }
    if (cp >= 0x0100 && cp <= 0x017F)
    {
        // Latin Extended-A: pairs of upper/lower case letters with accents.
        static const char ext[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        if (cp - 0x100 < sizeof(ext) - 1) *s += ext[cp - 0x100];
        return;
    }
    // Anything else (Japanese, symbols) is left out.
}

static void fold_into(std::string *out, const char *text, size_t len)
{
    size_t i = 0;
    const u8 *p = (const u8 *)text;
    while (i < len)
    {
        u32 c = p[i], cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { i++; continue; }
        if (i + n > len) break;
        {
            int k, bad = 0;
            for (k = 1; k < n; k++)
            {
                if ((p[i + k] & 0xC0) != 0x80) { bad = 1; break; }
                cp = (cp << 6) | (p[i + k] & 0x3F);
            }
            if (bad) { i++; continue; }
        }
        if (cp >= 0x20 || cp == '\n') append_cp(out, cp == '\n' ? ' ' : cp);
        i += n;
    }
}

void h64_ascii_fold(char *text)
{
    std::string s;
    fold_into(&s, text, strlen(text));
    memcpy(text, s.c_str(), s.size() + 1);   // never longer: every code point folds to at most its own UTF-8 length
}

// ---- Cache files ----
struct InfoField { const char *key; size_t offset, size; };
#define INFO_FIELD(k, m) { k, offsetof(H64GameInfo, m), sizeof(((H64GameInfo *)0)->m) }
static const InfoField s_fields[] = {
    INFO_FIELD("name", name), INFO_FIELD("title", title), INFO_FIELD("year", year), INFO_FIELD("date", date),
    INFO_FIELD("developer", developer), INFO_FIELD("publisher", publisher), INFO_FIELD("genre", genre),
    INFO_FIELD("players", players), INFO_FIELD("region", region), INFO_FIELD("rating", rating),
    INFO_FIELD("description", description),
};

int h64_gameinfo_format(const H64GameInfo *info, char *out, int outSize)
{
    std::string s = "# Harissa64 game information (sources: libretro-database CC BY-SA 4.0, GameDB-N64, Wikipedia CC BY-SA)\r\n";
    unsigned i;
    int n;
    if (outSize <= 0) return 0;
    s += info->found ? "found=1\r\n" : "found=0\r\n";
    s += info->cover > 0 ? "cover=yes\r\n" : info->cover == 0 ? "cover=none\r\n" : "cover=unknown\r\n";
    if (info->wiki >= 0) s += info->wiki > 0 ? "wiki=yes\r\n" : "wiki=none\r\n";
    for (i = 0; i < H64_ARRAY_COUNT(s_fields); i++)
    {
        const char *v = (const char *)info + s_fields[i].offset;
        if (!v[0]) continue;
        s += s_fields[i].key;
        s += '=';
        s += v;
        s += "\r\n";
    }
    n = (int)s.size() < outSize - 1 ? (int)s.size() : outSize - 1;
    memcpy(out, s.c_str(), n);
    out[n] = 0;
    return n;
}

int h64_gameinfo_parse(H64GameInfo *info, const char *text, u32 len)
{
    const char *p = text, *end = text + len;
    int sawFound = 0;
    h64_gameinfo_clear(info);
    while (p < end)
    {
        const char *eol = p, *eq;
        while (eol < end && *eol != '\n') eol++;
        for (eq = p; eq < eol && *eq != '='; eq++) ;
        if (eq < eol && *p != '#')
        {
            std::string key(p, eq - p), value(eq + 1, eol - eq - 1);
            unsigned i;
            while (!value.empty() && (value[value.size() - 1] == '\r' || value[value.size() - 1] == ' ')) value.erase(value.size() - 1);
            if (key == "found") { info->found = atoi(value.c_str()) != 0; sawFound = 1; }
            else if (key == "cover") info->cover = value == "yes" ? 1 : value == "none" ? 0 : -1;
            else if (key == "wiki") info->wiki = value == "yes" ? 1 : value == "none" ? 0 : -1;
            else
                for (i = 0; i < H64_ARRAY_COUNT(s_fields); i++)
                    if (key == s_fields[i].key) copy_str((char *)info + s_fields[i].offset, (int)s_fields[i].size, value.c_str());
        }
        p = eol + 1;
    }
    return sawFound;
}

// ---- Databases ----
#define LIBRETRO_DB "/libretro/libretro-database/master/metadat/"
const H64GameDbFile h64_gamedb_files[H64_GAMEDB_FILE_COUNT] = {
    { "nointro.dat", "raw.githubusercontent.com", LIBRETRO_DB "no-intro/Nintendo%20-%20Nintendo%2064.dat", 0 },
    { "developer.dat", "raw.githubusercontent.com", LIBRETRO_DB "developer/Nintendo%20-%20Nintendo%2064.dat", 0 },
    { "publisher.dat", "raw.githubusercontent.com", LIBRETRO_DB "publisher/Nintendo%20-%20Nintendo%2064.dat", 0 },
    { "releaseyear.dat", "raw.githubusercontent.com", LIBRETRO_DB "releaseyear/Nintendo%20-%20Nintendo%2064.dat", 0 },
    { "genre.dat", "raw.githubusercontent.com", LIBRETRO_DB "genre/Nintendo%20-%20Nintendo%2064.dat", 0 },
    { "maxusers.dat", "raw.githubusercontent.com", LIBRETRO_DB "maxusers/Nintendo%20-%20Nintendo%2064.dat", 0 },
    // GameDB-N64's latest release asset (github.com redirects to its storage).
    { "gamedb-n64.json", "github.com", "/niemasd/GameDB-N64/releases/latest/download/N64.data.json", 1 },
};

struct DbEntry
{
    std::string name, region, serial;
    std::vector<u32> crcs;
};
struct DbMeta
{
    std::string comment, developer, publisher, year, genre, users;
};
struct DbGdb
{
    std::string title, developer, publisher, date, rating;
};
struct H64GameDb
{
    std::vector<DbEntry> games;
    std::map<std::string, size_t> byName;          // No-Intro name -> games index
    std::multimap<std::string, size_t> bySerial;   // game code -> games index
    std::map<u32, DbMeta> meta;                    // ROM CRC -> metadat fields
    std::map<std::string, DbGdb> gdb;              // game code -> GameDB-N64
};

H64GameDb *h64_gamedb_new(void) { return new H64GameDb; }
void h64_gamedb_free(H64GameDb *db) { delete db; }
int h64_gamedb_count(const H64GameDb *db) { return db ? (int)db->games.size() : 0; }

// clrmamepro tokens: words, "strings", ( and ).
enum { TK_END = 0, TK_WORD, TK_OPEN, TK_CLOSE };
static int dat_token(const char **pp, const char *end, std::string *tok)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end) { *pp = p; return TK_END; }
    if (*p == '(') { *pp = p + 1; return TK_OPEN; }
    if (*p == ')') { *pp = p + 1; return TK_CLOSE; }
    if (*p == '"')
    {
        const char *s = ++p;
        while (p < end && *p != '"') p++;
        tok->assign(s, p - s);
        *pp = p < end ? p + 1 : p;
        return TK_WORD;
    }
    {
        const char *s = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '(' && *p != ')') p++;
        tok->assign(s, p - s);
        *pp = p;
        return TK_WORD;
    }
}

// Skips the rest of a block whose "(" was read.
static int dat_skip(const char **pp, const char *end)
{
    std::string t;
    int depth = 1;
    while (depth > 0)
    {
        int k = dat_token(pp, end, &t);
        if (k == TK_END) return -1;
        if (k == TK_OPEN) depth++;
        if (k == TK_CLOSE) depth--;
    }
    return 0;
}

static u32 hex32(const std::string &s)
{
    return (u32)strtoul(s.c_str(), NULL, 16);
}

static void merge(std::string *dst, const std::string &src)
{
    if (dst->empty()) *dst = src;
}

int h64_gamedb_add_dat(H64GameDb *db, const char *text, u32 len)
{
    const char *p = text, *end = text + len;
    std::string tok, key, val;
    int count = 0;
    for (;;)
    {
        int k = dat_token(&p, end, &tok);
        if (k == TK_END) break;
        if (k != TK_WORD) return -1;
        if (dat_token(&p, end, &val) != TK_OPEN) return -1;
        if (tok != "game")
        {
            if (dat_skip(&p, end)) return -1;
            continue;
        }
        {
            // One game: its fields and its ROMs' CRCs and serials.
            std::map<std::string, std::string> f;
            std::vector<u32> crcs;
            std::string romSerial;
            for (;;)
            {
                k = dat_token(&p, end, &key);
                if (k == TK_CLOSE) break;
                if (k != TK_WORD) return -1;
                k = dat_token(&p, end, &val);
                if (k == TK_OPEN)
                {
                    if (key != "rom") { if (dat_skip(&p, end)) return -1; continue; }
                    for (;;)
                    {
                        std::string rk, rv;
                        k = dat_token(&p, end, &rk);
                        if (k == TK_CLOSE) break;
                        if (k != TK_WORD) return -1;
                        k = dat_token(&p, end, &rv);
                        if (k == TK_OPEN) { if (dat_skip(&p, end)) return -1; continue; }
                        if (k != TK_WORD) return -1;
                        if (rk == "crc") crcs.push_back(hex32(rv));
                        else if (rk == "serial") romSerial = rv;
                    }
                    continue;
                }
                if (k != TK_WORD) return -1;
                f[key] = val;
            }
            count++;
            if (f.count("name"))
            {
                // No-Intro list: one entry per name (the .z64 and .v64 dumps share it).
                const std::string &name = f["name"];
                std::map<std::string, size_t>::iterator it = db->byName.find(name);
                size_t idx;
                unsigned i;
                if (it == db->byName.end())
                {
                    DbEntry e;
                    e.name = name;
                    e.region = f.count("region") ? f["region"] : std::string();
                    e.serial = f.count("serial") ? f["serial"] : romSerial;
                    idx = db->games.size();
                    db->games.push_back(e);
                    db->byName[name] = idx;
                    if (!db->games[idx].serial.empty()) db->bySerial.insert(std::make_pair(db->games[idx].serial, idx));
                }
                else
                    idx = it->second;
                for (i = 0; i < crcs.size(); i++) db->games[idx].crcs.push_back(crcs[i]);
            }
            else
            {
                // A metadat list: its fields for each CRC.
                unsigned i;
                for (i = 0; i < crcs.size(); i++)
                {
                    DbMeta &m = db->meta[crcs[i]];
                    if (f.count("comment")) merge(&m.comment, f["comment"]);
                    if (f.count("developer")) merge(&m.developer, f["developer"]);
                    if (f.count("publisher")) merge(&m.publisher, f["publisher"]);
                    if (f.count("releaseyear")) merge(&m.year, f["releaseyear"]);
                    if (f.count("genre")) merge(&m.genre, f["genre"]);
                    if (f.count("users")) merge(&m.users, f["users"]);
                }
            }
        }
    }
    return count;
}

// ---- A small JSON reader: string members of objects, at depth 1 or 2 ----
typedef void (*JsonFn)(void *user, int depth, const std::string &k1, const std::string &k2, const std::string &value);

static void json_ws(const char **pp, const char *end)
{
    while (*pp < end && (**pp == ' ' || **pp == '\t' || **pp == '\r' || **pp == '\n')) (*pp)++;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void utf8_put(std::string *s, u32 cp)
{
    if (cp < 0x80) *s += (char)cp;
    else if (cp < 0x800) { *s += (char)(0xC0 | (cp >> 6)); *s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *s += (char)(0xE0 | (cp >> 12)); *s += (char)(0x80 | ((cp >> 6) & 0x3F)); *s += (char)(0x80 | (cp & 0x3F)); }
    else { *s += (char)(0xF0 | (cp >> 18)); *s += (char)(0x80 | ((cp >> 12) & 0x3F)); *s += (char)(0x80 | ((cp >> 6) & 0x3F)); *s += (char)(0x80 | (cp & 0x3F)); }
}

static int json_hex4(const char *p, const char *end, u32 *v)
{
    int i;
    *v = 0;
    if (end - p < 4) return 0;
    for (i = 0; i < 4; i++)
    {
        int h = hexval(p[i]);
        if (h < 0) return 0;
        *v = (*v << 4) | (u32)h;
    }
    return 1;
}

// A string whose opening quote is at *pp; decoded to UTF-8.
static int json_string(const char **pp, const char *end, std::string *out)
{
    const char *p = *pp + 1;
    out->clear();
    while (p < end && *p != '"')
    {
        if (*p != '\\') { *out += *p++; continue; }
        if (++p >= end) return 0;
        switch (*p)
        {
        case 'n': case 'r': case 't': *out += ' '; p++; break;
        case 'b': case 'f': p++; break;
        case 'u':
        {
            u32 cp, lo;
            if (!json_hex4(p + 1, end, &cp)) return 0;
            p += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' && json_hex4(p + 2, end, &lo) && lo >= 0xDC00 && lo < 0xE000)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            utf8_put(out, cp);
            break;
        }
        default: *out += *p++; break;
        }
    }
    if (p >= end) return 0;
    *pp = p + 1;
    return 1;
}

static int json_value(const char **pp, const char *end, int depth, const std::string &k1, const std::string &k2, JsonFn fn, void *user);

static int json_object(const char **pp, const char *end, int depth, const std::string &k1, JsonFn fn, void *user)
{
    std::string key;
    (*pp)++;
    json_ws(pp, end);
    if (*pp < end && **pp == '}') { (*pp)++; return 1; }
    for (;;)
    {
        json_ws(pp, end);
        if (*pp >= end || **pp != '"' || !json_string(pp, end, &key)) return 0;
        json_ws(pp, end);
        if (*pp >= end || **pp != ':') return 0;
        (*pp)++;
        json_ws(pp, end);
        if (!json_value(pp, end, depth + 1, depth == 0 ? key : k1, depth == 0 ? std::string() : key, fn, user)) return 0;
        json_ws(pp, end);
        if (*pp >= end) return 0;
        if (**pp == ',') { (*pp)++; continue; }
        if (**pp == '}') { (*pp)++; return 1; }
        return 0;
    }
}

static int json_value(const char **pp, const char *end, int depth, const std::string &k1, const std::string &k2, JsonFn fn, void *user)
{
    json_ws(pp, end);
    if (*pp >= end || depth > 32) return 0;
    if (**pp == '{') return json_object(pp, end, depth, k1, fn, user);
    if (**pp == '"')
    {
        std::string v;
        if (!json_string(pp, end, &v)) return 0;
        if (depth >= 1 && depth <= 2) fn(user, depth, k1, k2, v);
        return 1;
    }
    if (**pp == '[')
    {
        (*pp)++;
        json_ws(pp, end);
        if (*pp < end && **pp == ']') { (*pp)++; return 1; }
        for (;;)
        {
            if (!json_value(pp, end, depth < 1 ? 3 : depth + 2, k1, k2, fn, user)) return 0;   // array items are not reported
            json_ws(pp, end);
            if (*pp >= end) return 0;
            if (**pp == ',') { (*pp)++; continue; }
            if (**pp == ']') { (*pp)++; return 1; }
            return 0;
        }
    }
    // Numbers, true, false, null.
    while (*pp < end && **pp != ',' && **pp != '}' && **pp != ']' && **pp != ' ' && **pp != '\r' && **pp != '\n' && **pp != '\t') (*pp)++;
    return 1;
}

static int json_walk(const char *text, u32 len, JsonFn fn, void *user)
{
    const char *p = text;
    std::string none;
    return json_value(&p, text + len, 0, none, none, fn, user);
}

struct GdbLoad { H64GameDb *db; int count; };

static void gdb_member(void *user, int depth, const std::string &k1, const std::string &k2, const std::string &value)
{
    GdbLoad *g = (GdbLoad *)user;
    std::string code;
    if (depth != 2) return;
    // "NUS-NSME-USA" -> "NSME"
    if (k1.size() >= 8 && k1[3] == '-') code = k1.substr(4, 4);
    else return;
    {
        std::map<std::string, DbGdb>::iterator it = g->db->gdb.find(code);
        DbGdb *d;
        if (it == g->db->gdb.end()) { d = &g->db->gdb[code]; g->count++; }
        else d = &it->second;
        if (k2 == "title") merge(&d->title, value);
        else if (k2 == "developer") merge(&d->developer, value);
        else if (k2 == "publisher") merge(&d->publisher, value);
        else if (k2 == "release_date") merge(&d->date, value);
        else if (k2 == "rating") merge(&d->rating, value);
    }
}

int h64_gamedb_add_gamedb_json(H64GameDb *db, const char *text, u32 len)
{
    GdbLoad g;
    g.db = db;
    g.count = 0;
    if (!json_walk(text, len, gdb_member, &g)) return g.count ? g.count : -1;
    return g.count;
}

// ---- Matching ----
// "Super Mario 64 (USA) (Rev 1)": how good a pick for a ROM of `version`
// whose region is `region` (higher is better).
static int name_score(const std::string &name, int version, const char *region)
{
    static const char *bad[] = { "Beta", "Proto", "Demo", "Kiosk", "LodgeNet", "Debug", "Pirate", "Unl", "Hack", "Sample",
                                 "Promo", "Virtual Console", "Switch Online", "Aftermarket", "Wii", "iQue", "Competition",
                                 "Alt", "Program", "GameCube", "Collection" };
    const char *p = name.c_str();
    int score = 100, rev = 0;
    unsigned i;
    while ((p = strchr(p, '(')) != NULL)
    {
        const char *q = strchr(p, ')');
        std::string tag(p + 1, q ? q - p - 1 : strlen(p + 1));
        if (starts_with_ci(tag.c_str(), "Rev "))
        {
            const char *r = tag.c_str() + 4;
            rev = *r >= 'A' && *r <= 'Z' ? *r - 'A' + 1 : atoi(r);
        }
        for (i = 0; i < H64_ARRAY_COUNT(bad); i++)
            if (contains_ci(tag.c_str(), bad[i])) { score -= 50; break; }
        if (!q) break;
        p = q + 1;
    }
    score += rev == version ? 20 : -5 * (rev > version ? rev - version : version - rev);
    if (region && region[0] && contains_ci(name.c_str(), region)) score += 10;
    return score * 1000 - (int)name.size();
}

// The part of a name before its tags, lower case letters and digits only.
static std::string base_key(const char *name)
{
    std::string k;
    for (; *name && *name != '(' && *name != '['; name++)
        if (is_alnum(*name)) k += lower(*name);
    return k;
}

// "Legend of Zelda, The: Ocarina of Time (USA)" -> "The Legend of Zelda: Ocarina of Time".
static void display_title(const std::string &src, int dashToColon, char *out, int outSize)
{
    std::string t = src.substr(0, src.find(" ("));
    static const char *arts[] = { "The", "A", "An" };
    size_t sep;
    unsigned i;
    if (dashToColon && (sep = t.find(" - ")) != std::string::npos) t.replace(sep, 3, ": ");
    sep = t.find(": ");
    {
        std::string head = sep == std::string::npos ? t : t.substr(0, sep), tail = sep == std::string::npos ? std::string() : t.substr(sep);
        for (i = 0; i < H64_ARRAY_COUNT(arts); i++)
        {
            std::string suf = std::string(", ") + arts[i];
            if (head.size() > suf.size() && head.compare(head.size() - suf.size(), suf.size(), suf) == 0)
            {
                head = std::string(arts[i]) + " " + head.substr(0, head.size() - suf.size());
                break;
            }
        }
        t = head + tail;
    }
    {
        std::string folded;
        fold_into(&folded, t.c_str(), t.size());
        copy_str(out, outSize, folded.c_str());
    }
}

// copy_str of the ASCII-folded text (the UI font has no accents).
static void copy_fold(char *dst, int size, const char *src)
{
    std::string folded;
    fold_into(&folded, src, strlen(src));
    copy_str(dst, size, folded.c_str());
}

struct Cand { size_t idx; int score; };

int h64_gamedb_lookup(const H64GameDb *db, const char *gameCode, int version, const char *fileStem, H64GameInfo *info,
                      char (*covers)[128], int maxCovers, int *coverCount)
{
    std::vector<Cand> cands;
    const char *region = gameCode && strlen(gameCode) == 4 ? h64_gameinfo_region_name(gameCode[3]) : "";
    std::string code = gameCode ? gameCode : "";
    size_t i, j;
    int found = 0;
    if (coverCount) *coverCount = 0;
    if (!db) return 0;
    if (code.size() == 4)
    {
        std::multimap<std::string, size_t>::const_iterator it = db->bySerial.lower_bound(code);
        for (; it != db->bySerial.end() && it->first == code; ++it)
        {
            Cand c;
            c.idx = it->second;
            c.score = name_score(db->games[c.idx].name, version, region);
            cands.push_back(c);
        }
    }
    if (cands.empty() && fileStem && fileStem[0])
    {
        // No usable code: the file name, without its tags.
        std::string k = base_key(fileStem);
        if (k.size() >= 3)
            for (i = 0; i < db->games.size(); i++)
                if (base_key(db->games[i].name.c_str()) == k)
                {
                    Cand c;
                    c.idx = i;
                    c.score = name_score(db->games[i].name, version, region);
                    cands.push_back(c);
                }
    }
    // Best first (a plain insertion sort: a handful of names).
    for (i = 1; i < cands.size(); i++)
        for (j = i; j > 0 && cands[j].score > cands[j - 1].score; j--)
        {
            Cand t = cands[j];
            cands[j] = cands[j - 1];
            cands[j - 1] = t;
        }
    if (!cands.empty())
    {
        const DbEntry &e = db->games[cands[0].idx];
        const DbMeta *m = NULL;
        found = 1;
        info->found = 1;
        copy_str(info->name, sizeof(info->name), e.name.c_str());
        copy_fold(info->region, sizeof(info->region), e.region.c_str());
        // The metadata of any dump of this game (the .z64 CRC first), else of the other candidates.
        for (j = 0; j < cands.size() && !m; j++)
        {
            const DbEntry &ce = db->games[cands[j].idx];
            for (i = 0; i < ce.crcs.size() && !m; i++)
            {
                std::map<u32, DbMeta>::const_iterator it = db->meta.find(ce.crcs[i]);
                if (it != db->meta.end()) m = &it->second;
            }
        }
        if (m)
        {
            copy_fold(info->developer, sizeof(info->developer), m->developer.c_str());
            copy_fold(info->publisher, sizeof(info->publisher), m->publisher.c_str());
            copy_fold(info->year, sizeof(info->year), m->year.c_str());
            copy_fold(info->genre, sizeof(info->genre), m->genre.c_str());
            if (!m->users.empty())
            {
                int u = atoi(m->users.c_str());
                if (u > 1) sprintf(info->players, "1-%d", u);
                else if (u == 1) strcpy(info->players, "1");
            }
        }
        display_title(m && !m->comment.empty() ? m->comment : e.name, !(m && !m->comment.empty()), info->title, sizeof(info->title));
        if (covers && coverCount)
            for (i = 0; i < cands.size() && *coverCount < maxCovers; i++)
                copy_str(covers[(*coverCount)++], 128, db->games[cands[i].idx].name.c_str());
    }
    if (code.size() == 4)
    {
        // GameDB-N64: the title as printed, the release date, the rating.
        std::map<std::string, DbGdb>::const_iterator it = db->gdb.find(code);
        if (it != db->gdb.end())
        {
            const DbGdb &g = it->second;
            std::string t;
            found = 1;
            info->found = 1;
            if (!g.title.empty()) display_title(g.title, 0, info->title, sizeof(info->title));
            if (!info->developer[0]) copy_fold(info->developer, sizeof(info->developer), g.developer.c_str());
            if (!info->publisher[0]) copy_fold(info->publisher, sizeof(info->publisher), g.publisher.c_str());
            if (g.date.size() >= 4)
            {
                // The release date of this region: its year wins over libretro's.
                copy_fold(info->date, sizeof(info->date), g.date.c_str());
                copy_fold(info->year, sizeof(info->year), g.date.substr(0, 4).c_str());
            }
            copy_fold(info->rating, sizeof(info->rating), g.rating.c_str());
            if (!info->region[0]) copy_fold(info->region, sizeof(info->region), region);
        }
    }
    return found;
}

// ---- Download helpers ----
void h64_gameinfo_thumb_name(const char *name, char *out, int outSize)
{
    int n = 0;
    if (outSize <= 0) return;
    for (; *name && n < outSize - 1; name++) out[n++] = strchr("&*/:`<>?\\|\"", *name) ? '_' : *name;
    out[n] = 0;
}

void h64_url_encode(const char *text, char *out, int outSize)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    if (outSize <= 0) return;
    for (; *text; text++)
    {
        u8 c = (u8)*text;
        if (is_alnum((char)c) || c == '-' || c == '.' || c == '_' || c == '~')
        {
            if (n + 1 >= outSize) break;
            out[n++] = (char)c;
        }
        else
        {
            if (n + 3 >= outSize) break;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}

int h64_gameinfo_wiki_pages(const H64GameInfo *info, char (*out)[256], int max)
{
    std::string t = info->title, pages[3];
    int n = 0, i;
    size_t k;
    if (t.empty()) return 0;
    for (k = 0; k < t.size(); k++)
        if (t[k] == ' ') t[k] = '_';
    pages[n++] = t;
    pages[n++] = t + "_(video_game)";
    if (info->year[0]) pages[n++] = t + "_(" + info->year + "_video_game)";
    for (i = 0; i < n && i < max; i++) h64_url_encode(pages[i].c_str(), out[i], 256);
    return i;
}

struct WikiLoad { std::string type, description, extract; };

static void wiki_member(void *user, int depth, const std::string &k1, const std::string &, const std::string &value)
{
    WikiLoad *w = (WikiLoad *)user;
    if (depth != 1) return;
    if (k1 == "type") w->type = value;
    else if (k1 == "description") w->description = value;
    else if (k1 == "extract") w->extract = value;
}

int h64_gameinfo_wiki_extract(const char *json, u32 len, char *out, int outSize)
{
    WikiLoad w;
    std::string text;
    size_t cut = 0, pos = 0;
    int sentences = 0;
    if (outSize <= 1) return 0;
    out[0] = 0;
    json_walk(json, len, wiki_member, &w);
    // A game's page, not a series' ("Mario Party": the series; "Mario Party (video game)": the game).
    if (w.type == "disambiguation" || w.extract.empty() || !contains_ci(w.description.c_str(), "game") ||
        contains_ci(w.description.c_str(), "series") || contains_ci(w.description.c_str(), "franchise"))
        return 0;
    fold_into(&text, w.extract.c_str(), w.extract.size());
    // Whole sentences (at most 4) that fit.
    while (sentences < 4)
    {
        size_t dot = text.find(". ", pos);
        size_t endPos = dot == std::string::npos ? (text.size() && text[text.size() - 1] == '.' ? text.size() : std::string::npos) : dot + 1;
        if (endPos == std::string::npos || endPos >= (size_t)outSize) break;
        cut = endPos;
        sentences++;
        if (dot == std::string::npos) break;
        pos = dot + 2;
    }
    if (!cut) return 0;
    copy_str(out, outSize, text.substr(0, cut).c_str());
    return 1;
}
