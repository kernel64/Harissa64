// Harissa64 V2 - the hi-res texture pack catalogue (h64_hdcatalog.h).
#include "h64_hdcatalog.h"
#include "../common/h64_json.h"

#include <stdlib.h>
#include <string.h>
#include <map>

namespace {

struct Load
{
    std::map<std::string, H64HdPack> packs;
    std::vector<std::string> order;
    std::string format;
};

void member(void *user, int depth, const std::string &k1, const std::string &k2, const std::string &v)
{
    Load *l = (Load *)user;
    H64HdPack *p;
    if (depth == 1)
    {
        if (k1 == "format") l->format = v;
        return;
    }
    if (depth != 2) return;
    if (l->packs.find(k1) == l->packs.end())
    {
        H64HdPack n;
        n.id = k1;
        n.size = 0;
        l->packs[k1] = n;
        l->order.push_back(k1);
    }
    p = &l->packs[k1];
    if (k2 == "name") p->name = v;
    else if (k2 == "games") p->games = v;
    else if (k2 == "rom") p->rom = v;
    else if (k2 == "author") p->author = v;
    else if (k2 == "version") p->version = v;
    else if (k2 == "page") p->page = v;
    else if (k2 == "url") p->url = v;
    else if (k2 == "sha256") p->sha256 = v;
    else if (k2 == "size")
    {
        double d = strtod(v.c_str(), NULL);
        p->size = d > 0 && d < 4294967295.0 ? (u32)d : 0;
    }
}

int hex64(const std::string &s)
{
    size_t i;
    if (s.size() != 64) return 0;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    return 1;
}

}  // namespace

int h64_hdcat_parse(const char *json, u32 len, std::vector<H64HdPack> *out)
{
    Load l;
    size_t i;
    out->clear();
    if (!h64_json_walk(json, len, member, &l) || l.format != "1") return -1;
    for (i = 0; i < l.order.size(); i++)
    {
        H64HdPack &p = l.packs[l.order[i]];
        if (p.name.empty() || p.rom.empty() || p.games.empty() || !p.size || p.url.compare(0, 8, "https://")) continue;
        if (!p.sha256.empty() && !hex64(p.sha256)) continue;
        out->push_back(p);
    }
    return (int)out->size();
}

int h64_hdcat_fits_rom(const H64HdPack *p, const char *gameCode, const char *romName)
{
    const char *a, *b;
    size_t na, nb;
    if (gameCode && strlen(gameCode) >= 3 && h64_hdcat_fits(p, gameCode)) return 1;
    if (!romName || p->rom.empty()) return 0;
    // Both trimmed of spaces, compared without case.
    a = romName;
    b = p->rom.c_str();
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    na = strlen(a);
    nb = strlen(b);
    while (na && a[na - 1] == ' ') na--;
    while (nb && b[nb - 1] == ' ') nb--;
    if (!na || na != nb) return 0;
    while (na--)
    {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return 1;
}

int h64_hdcat_fits(const H64HdPack *p, const char *gameCode)
{
    const char *s = p->games.c_str();
    while (*s)
    {
        while (*s == ',' || *s == ' ') s++;
        if (strlen(s) >= 3 && !memcmp(s, gameCode, 3) && (s[3] == 0 || s[3] == ',' || s[3] == ' ')) return 1;
        while (*s && *s != ',') s++;
    }
    return 0;
}
