// Harissa64 V2 - cheats (see h64_cheat.h). The code semantics follow
// mupen64plus-core's cheat.c (GPL v2 or later; see THIRD_PARTY.md).
#include "h64_cheat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../common/h64_crc32.h"
#include "../system/h64_system.h"

namespace
{
struct Code
{
    u32 addr;      // the code's first word
    u16 value;
    u8 pick;       // the value comes from the cheat's chosen option
    u8 saved;      // `old` holds what a plain write replaced
    u16 old;
};
struct Option { u16 value; std::string name; };
struct Cheat
{
    std::string name, note;
    u32 id;
    std::vector<Code> codes;
    std::vector<Option> options;
    int state;     // 0 off, 1.. on
    int undo;      // just turned off: put the old values back
    int fresh;     // just turned on: the F0/F1 codes run once
};
}

struct H64CheatList
{
    std::vector<Cheat> cheats;
    int active, undo;
};

H64CheatList *h64_cheat_new(void)
{
    H64CheatList *l = new H64CheatList;
    l->active = l->undo = 0;
    return l;
}

void h64_cheat_free(H64CheatList *list) { delete list; }

// ---- Reading ----
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// `n` hexadecimal digits at p; returns 0 when they are not all digits.
static int hex_word(const char *p, int n, u32 *out)
{
    u32 v = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        int d = hex_digit(p[i]);
        if (d < 0) return 0;
        v = (v << 4) | (u32)d;
    }
    *out = v;
    return 1;
}

// A code line "AAAAAAAA VVVV[ choices]" (already trimmed). Returns 1 when it is one.
static int parse_code(const std::string &line, Cheat *c)
{
    Code code;
    u32 addr, value = 0;
    size_t p;
    if (line.size() < 13 || line[8] != ' ' || !hex_word(line.c_str(), 8, &addr)) return 0;
    memset(&code, 0, sizeof(code));
    code.addr = addr;
    if (!strncmp(line.c_str() + 9, "????", 4)) code.pick = 1;
    else if (!hex_word(line.c_str() + 9, 4, &value)) return 0;
    if (line.size() > 13 && line[13] != ' ') return 0;
    code.value = (u16)value;
    c->codes.push_back(code);
    // The choices: VVVV:"name",VVVV:"name"...
    p = 13;
    while (p < line.size())
    {
        Option o;
        u32 v;
        size_t q, e;
        while (p < line.size() && (line[p] == ' ' || line[p] == ',')) p++;
        q = line.find(':', p);
        if (q == std::string::npos || q - p < 1 || q - p > 4 || !hex_word(line.c_str() + p, (int)(q - p), &v)) break;
        if (q + 1 >= line.size() || line[q + 1] != '"') break;
        e = line.find('"', q + 2);
        if (e == std::string::npos) break;
        o.value = (u16)v;
        o.name = line.substr(q + 2, e - q - 2);
        if (c->options.size() < 255) c->options.push_back(o);
        p = e + 1;
    }
    return 1;
}

static std::string trim(const char *p, size_t n)
{
    while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) n--;
    return std::string(p, n);
}

static void finish(H64CheatList *list, Cheat *c, int *added)
{
    if (!c->codes.empty() && !c->name.empty())
    {
        c->id = h64_crc32(0, (const u8 *)c->name.c_str(), c->name.size());
        c->state = c->undo = c->fresh = 0;
        list->cheats.push_back(*c);
        (*added)++;
    }
    c->name.clear();
    c->note.clear();
    c->codes.clear();
    c->options.clear();
}

// Reads cheats from [p, end) up to the next "crc " line. `plain`: a user's
// file, where a line that is no code, note or comment names a cheat.
static int read_cheats(H64CheatList *list, const char *p, const char *end, int plain)
{
    Cheat cur;
    int added = 0;
    cur.id = 0;
    cur.state = cur.undo = cur.fresh = 0;
    while (p < end)
    {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        std::string line = trim(p, n);
        p = nl ? nl + 1 : end;
        if (line.empty() || !line.compare(0, 2, "//") || line[0] == '#') continue;
        if (!line.compare(0, 4, "crc ")) break;
        if (!line.compare(0, 3, "gn ")) continue;
        if (!line.compare(0, 3, "cn "))
        {
            finish(list, &cur, &added);
            cur.name = trim(line.c_str() + 3, line.size() - 3);
            continue;
        }
        if (!line.compare(0, 3, "cd "))
        {
            cur.note = trim(line.c_str() + 3, line.size() - 3);
            continue;
        }
        if (parse_code(line, &cur)) continue;
        if (plain)
        {
            finish(list, &cur, &added);
            cur.name = line;
        }
    }
    finish(list, &cur, &added);
    return added;
}

int h64_cheat_load_db(H64CheatList *list, const char *text, size_t len, u32 crc1, u32 crc2, u8 country)
{
    char key[40];
    const char *end = text + len, *p = text;
    size_t klen;
    sprintf(key, "crc %08X-%08X-C:%02X", (unsigned)crc1, (unsigned)crc2, (unsigned)country);
    klen = strlen(key);
    while (p + klen <= end)
    {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *next = nl ? nl + 1 : end;
        if (p[0] == 'c' && !memcmp(p, key, klen)) return read_cheats(list, next, end, 0);
        p = next;
    }
    return 0;
}

int h64_cheat_load_plain(H64CheatList *list, const char *text, size_t len)
{
    return read_cheats(list, text, text + len, 1);
}

// ---- The list ----
int h64_cheat_count(const H64CheatList *list) { return list ? (int)list->cheats.size() : 0; }
int h64_cheat_active(const H64CheatList *list) { return list ? list->active : 0; }

void h64_cheat_info(const H64CheatList *list, int index, H64CheatInfo *out)
{
    const Cheat &c = list->cheats[(size_t)index];
    out->name = c.name.c_str();
    out->note = c.note.c_str();
    out->id = c.id;
    out->options = (int)c.options.size();
    out->state = c.state;
}

const char *h64_cheat_option_name(const H64CheatList *list, int index, int option)
{
    const Cheat &c = list->cheats[(size_t)index];
    return option >= 0 && option < (int)c.options.size() ? c.options[(size_t)option].name.c_str() : "";
}

void h64_cheat_set(H64CheatList *list, int index, int state)
{
    Cheat *c;
    int max, i;
    if (index < 0 || index >= (int)list->cheats.size()) return;
    c = &list->cheats[(size_t)index];
    max = c->options.empty() ? 1 : (int)c->options.size();
    if (state < 0 || state > max) state = 0;
    if (state == c->state) return;
    if (!state) { c->undo = 1; list->undo = 1; }
    else if (!c->state) { c->fresh = 1; c->undo = 0; }
    c->state = state;
    list->active = 0;
    for (i = 0; i < (int)list->cheats.size(); i++) list->active += list->cheats[(size_t)i].state != 0;
}

void h64_cheat_states_write(const H64CheatList *list, char *out, size_t size)
{
    size_t i, n = 0;
    if (!size) return;
    out[0] = 0;
    for (i = 0; list && i < list->cheats.size(); i++)
    {
        const Cheat &c = list->cheats[i];
        char item[24];
        size_t k;
        if (!c.state) continue;
        sprintf(item, "%s%08X:%d", n ? "," : "", (unsigned)c.id, c.state);
        k = strlen(item);
        if (n + k + 1 > size) break;
        memcpy(out + n, item, k + 1);
        n += k;
    }
}

void h64_cheat_states_read(H64CheatList *list, const char *text)
{
    const char *p = text;
    while (p && *p)
    {
        u32 id;
        int state;
        size_t i;
        if (strlen(p) >= 10 && hex_word(p, 8, &id) && p[8] == ':')
        {
            state = atoi(p + 9);
            for (i = 0; i < list->cheats.size(); i++)
                if (list->cheats[i].id == id) h64_cheat_set(list, (int)i, state);
        }
        p = strchr(p, ',');
        if (p) p++;
    }
}

// ---- Running ----
// One write of a code; `plain` writes remember what they replace.
static void poke(const H64CheatMem *mem, Code *code, u32 addr, int bytes, u32 value, int keep)
{
    addr &= 0x7FFFFFu;
    if (bytes == 2) addr &= ~1u;
    if (keep && !code->saved)
    {
        code->old = (u16)mem->read(mem->user, addr, bytes);
        code->saved = 1;
    }
    mem->write(mem->user, addr, bytes, bytes == 1 ? (value & 0xFF) : (value & 0xFFFF));
}

static void run_cheat(Cheat *c, const H64CheatMem *mem, int gsButton)
{
    size_t i, n = c->codes.size();
    u16 pick = c->options.empty() ? 0 : c->options[(size_t)(c->state - 1)].value;
    for (i = 0; i < n; i++)
    {
        Code *code = &c->codes[i];
        u32 type = code->addr >> 24, value = code->pick ? pick : code->value;
        int skip = 0;
        switch (type)
        {
        case 0x80: case 0xA0: poke(mem, code, code->addr, 1, value, 1); break;
        case 0x81: case 0xA1: poke(mem, code, code->addr, 2, value, 1); break;
        case 0x88: if (gsButton) poke(mem, code, code->addr, 1, value, 0); break;
        case 0x89: if (gsButton) poke(mem, code, code->addr, 2, value, 0); break;
        case 0xF0: if (c->fresh) poke(mem, code, code->addr, 1, value, 0); break;
        case 0xF1: if (c->fresh) poke(mem, code, code->addr, 2, value, 0); break;
        case 0xD0: skip = mem->read(mem->user, code->addr & 0x7FFFFFu, 1) != (value & 0xFF); break;
        case 0xD1: skip = mem->read(mem->user, code->addr & 0x7FFFFEu, 2) != (value & 0xFFFF); break;
        case 0xD2: skip = mem->read(mem->user, code->addr & 0x7FFFFFu, 1) == (value & 0xFF); break;
        case 0xD3: skip = mem->read(mem->user, code->addr & 0x7FFFFEu, 2) == (value & 0xFFFF); break;
        case 0x50:
            // 5000NNSS IIII: the next code NN times, address + SS, value + IIII each time.
            if (i + 1 < n)
            {
                Code *next = &c->codes[i + 1];
                u32 count = (code->addr >> 8) & 0xFF, step = code->addr & 0xFF, k, t = next->addr >> 24;
                u32 a = next->addr & 0xFFFFFFu, v = next->pick ? pick : next->value;
                int bytes = (t == 0x81 || t == 0xA1) ? 2 : (t == 0x80 || t == 0xA0) ? 1 : 0;
                for (k = 0; bytes && k < count; k++)
                {
                    poke(mem, next, a, bytes, v, 0);
                    a += step;
                    v += value;
                }
                i++;
            }
            break;
        default: break;   // EE, DE, CC, FF...: nothing to do here
        }
        if (skip)
        {
            // The next code does not run (with its repeat, when it is one).
            if (i + 1 < n && (c->codes[i + 1].addr >> 24) == 0x50) i++;
            i++;
        }
    }
    c->fresh = 0;
}

static void undo_cheat(Cheat *c, const H64CheatMem *mem)
{
    size_t i;
    for (i = 0; i < c->codes.size(); i++)
    {
        Code *code = &c->codes[i];
        u32 type = code->addr >> 24;
        if (!code->saved) continue;
        if (type == 0x80 || type == 0xA0) mem->write(mem->user, code->addr & 0x7FFFFFu, 1, code->old & 0xFF);
        else if (type == 0x81 || type == 0xA1) mem->write(mem->user, code->addr & 0x7FFFFEu, 2, code->old);
        code->saved = 0;
    }
    c->undo = 0;
}

void h64_cheat_run(H64CheatList *list, const H64CheatMem *mem, int gsButton)
{
    size_t i;
    if (!list || (!list->active && !list->undo)) return;
    for (i = 0; i < list->cheats.size(); i++)
    {
        Cheat *c = &list->cheats[i];
        if (c->undo && !c->state) undo_cheat(c, mem);
        if (c->state) run_cheat(c, mem, gsButton);
    }
    list->undo = 0;
}

// ---- On a machine ----
static u32 sys_read(void *user, u32 addr, int bytes)
{
    H64System *sys = (H64System *)user;
    if (addr + (u32)bytes > H64_RDRAM_SIZE) return 0;
    return bytes == 1 ? sys->rdram[addr] : ((u32)sys->rdram[addr] << 8) | sys->rdram[addr + 1];
}

static void sys_write(void *user, u32 addr, int bytes, u32 value)
{
    H64System *sys = (H64System *)user;
    if (addr + (u32)bytes > H64_RDRAM_SIZE) return;
    if (bytes == 1) sys->rdram[addr] = (u8)value;
    else
    {
        sys->rdram[addr] = (u8)(value >> 8);
        sys->rdram[addr + 1] = (u8)value;
    }
    h64_jit_notify_write(sys, addr, (u32)bytes);   // codes may patch the game's code
}

void h64_cheat_frame(H64System *sys)
{
    H64CheatMem mem;
    if (!sys->cheats || (!sys->cheats->active && !sys->cheats->undo)) return;
    mem.user = sys;
    mem.read = sys_read;
    mem.write = sys_write;
    h64_cheat_run(sys->cheats, &mem, sys->cheatButton);
}
