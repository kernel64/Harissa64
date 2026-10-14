// Harissa64 V2 - controller remapping (see h64_padmap.h).
#include "h64_padmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../system/h64_system.h"

struct SrcInfo { const char *name, *label; };
static const SrcInfo s_src[H64_SRC_COUNT] = {
    { "xa", "A" }, { "xb", "B" }, { "xx", "X" }, { "xy", "Y" }, { "lb", "LB" }, { "rb", "RB" }, { "lt", "LT" }, { "rt", "RT" },
    { "start", "START" }, { "dup", "D-pad up" }, { "ddown", "D-pad down" }, { "dleft", "D-pad left" }, { "dright", "D-pad right" },
    { "ls", "Left stick click" }, { "rs", "Right stick click" },
    { "lup", "Left stick up" }, { "ldown", "Left stick down" }, { "lleft", "Left stick left" }, { "lright", "Left stick right" },
    { "rup", "Right stick up" }, { "rdown", "Right stick down" }, { "rleft", "Right stick left" }, { "rright", "Right stick right" },
};

struct N64Info { const char *name, *label; u16 bit; };
static const N64Info s_n64[H64_N64_COUNT] = {
    { "a", "A", 0x8000 }, { "b", "B", 0x4000 }, { "z", "Z", 0x2000 }, { "start", "START", 0x1000 },
    { "dup", "D-pad up", 0x0800 }, { "ddown", "D-pad down", 0x0400 }, { "dleft", "D-pad left", 0x0200 }, { "dright", "D-pad right", 0x0100 },
    { "l", "L", 0x0020 }, { "r", "R", 0x0010 },
    { "cup", "C-up", 0x0008 }, { "cdown", "C-down", 0x0004 }, { "cleft", "C-left", 0x0002 }, { "cright", "C-right", 0x0001 },
};

#define BIT(s) (1u << (s))

void h64_padmap_default(H64PadMap *m)
{
    memset(m, 0, sizeof(*m));
    m->src[H64_N64_A] = BIT(H64_SRC_A);
    m->src[H64_N64_B] = BIT(H64_SRC_B);
    m->src[H64_N64_Z] = BIT(H64_SRC_X) | BIT(H64_SRC_Y) | BIT(H64_SRC_LT) | BIT(H64_SRC_RT);
    m->src[H64_N64_START] = BIT(H64_SRC_START);
    m->src[H64_N64_DUP] = BIT(H64_SRC_DUP);
    m->src[H64_N64_DDOWN] = BIT(H64_SRC_DDOWN);
    m->src[H64_N64_DLEFT] = BIT(H64_SRC_DLEFT);
    m->src[H64_N64_DRIGHT] = BIT(H64_SRC_DRIGHT);
    m->src[H64_N64_L] = BIT(H64_SRC_LB);
    m->src[H64_N64_R] = BIT(H64_SRC_RB);
    m->src[H64_N64_CUP] = BIT(H64_SRC_RUP);
    m->src[H64_N64_CDOWN] = BIT(H64_SRC_RDOWN);
    m->src[H64_N64_CLEFT] = BIT(H64_SRC_RLEFT);
    m->src[H64_N64_CRIGHT] = BIT(H64_SRC_RRIGHT);
    m->stick = H64_STICK_LEFT;
    m->deadzone = H64_PAD_DEADZONE_DEFAULT;
    m->range = H64_PAD_RANGE_DEFAULT;
}

int h64_padmap_is_default(const H64PadMap *m)
{
    H64PadMap d;
    int i;
    h64_padmap_default(&d);
    for (i = 0; i < H64_N64_COUNT; i++)
        if (m->src[i] != d.src[i]) return 0;
    return m->stick == d.stick && m->deadzone == d.deadzone && m->range == d.range;
}

u32 h64_padmap_sources(const H64PadInput *in)
{
    static const u16 buttons[] = { H64_XB_A, H64_XB_B, H64_XB_X, H64_XB_Y, H64_XB_LEFT_SHOULDER, H64_XB_RIGHT_SHOULDER };
    const int t = H64_PAD_STICK_THRESHOLD;
    u32 s = 0;
    int i;
    for (i = 0; i < 6; i++)
        if (in->buttons & buttons[i]) s |= BIT(H64_SRC_A + i);
    if (in->lt > H64_PAD_TRIGGER_THRESHOLD) s |= BIT(H64_SRC_LT);
    if (in->rt > H64_PAD_TRIGGER_THRESHOLD) s |= BIT(H64_SRC_RT);
    if (in->buttons & H64_XB_START) s |= BIT(H64_SRC_START);
    if (in->buttons & H64_XB_DPAD_UP) s |= BIT(H64_SRC_DUP);
    if (in->buttons & H64_XB_DPAD_DOWN) s |= BIT(H64_SRC_DDOWN);
    if (in->buttons & H64_XB_DPAD_LEFT) s |= BIT(H64_SRC_DLEFT);
    if (in->buttons & H64_XB_DPAD_RIGHT) s |= BIT(H64_SRC_DRIGHT);
    if (in->buttons & H64_XB_LEFT_THUMB) s |= BIT(H64_SRC_LTHUMB);
    if (in->buttons & H64_XB_RIGHT_THUMB) s |= BIT(H64_SRC_RTHUMB);
    if (in->ly > t) s |= BIT(H64_SRC_LUP);
    if (in->ly < -t) s |= BIT(H64_SRC_LDOWN);
    if (in->lx < -t) s |= BIT(H64_SRC_LLEFT);
    if (in->lx > t) s |= BIT(H64_SRC_LRIGHT);
    if (in->ry > t) s |= BIT(H64_SRC_RUP);
    if (in->ry < -t) s |= BIT(H64_SRC_RDOWN);
    if (in->rx < -t) s |= BIT(H64_SRC_RLEFT);
    if (in->rx > t) s |= BIT(H64_SRC_RRIGHT);
    return s;
}

// One stick axis: the dead zone cut off, the rest scaled to +-max (80 by
// default, the range of a real N64 stick).
static s8 stick_axis(int x, int dead, int range)
{
    int max = 80 * range / 100;
    if (max > 127) max = 127;
    if (max < 1) max = 1;
    if (dead < 0) dead = 0;
    if (dead > 32000) dead = 32000;
    if (x > -dead && x < dead) return 0;
    x = x > 0 ? (x - dead) * max / (32767 - dead) : (x + dead) * max / (32768 - dead);
    return (s8)(x > max ? max : x < -max ? -max : x);
}

void h64_padmap_apply(const H64PadMap *m, const H64PadInput *in, H64Pad *out)
{
    u32 s = h64_padmap_sources(in);
    u16 b = 0;
    int i;
    for (i = 0; i < H64_N64_COUNT; i++)
        if (s & m->src[i]) b |= s_n64[i].bit;
    out->buttons = b;
    if (m->stick == H64_STICK_LEFT)
    {
        out->x = stick_axis(in->lx, m->deadzone, m->range);
        out->y = stick_axis(in->ly, m->deadzone, m->range);
    }
    else if (m->stick == H64_STICK_RIGHT)
    {
        out->x = stick_axis(in->rx, m->deadzone, m->range);
        out->y = stick_axis(in->ry, m->deadzone, m->range);
    }
    else
        out->x = out->y = 0;
}

const char *h64_padmap_source_name(int src) { return src >= 0 && src < H64_SRC_COUNT ? s_src[src].name : ""; }
const char *h64_padmap_source_label(int src) { return src >= 0 && src < H64_SRC_COUNT ? s_src[src].label : ""; }
const char *h64_padmap_n64_name(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].name : ""; }
const char *h64_padmap_n64_label(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].label : ""; }
u16 h64_padmap_n64_bit(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].bit : 0; }

int h64_padmap_parse_sources(const char *text, u32 *mask)
{
    u32 m = 0;
    int any = 0;
    while (*text)
    {
        char tok[16];
        int n = 0, i;
        while (*text == ',' || *text == ' ' || *text == '\t' || *text == '+') text++;
        while (*text && *text != ',' && *text != ' ' && *text != '\t' && *text != '+')
        {
            char ch = *text++;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (n < (int)sizeof(tok) - 1) tok[n++] = ch;
        }
        tok[n] = 0;
        if (!n) continue;
        if (!strcmp(tok, "none")) { any = 1; continue; }
        for (i = 0; i < H64_SRC_COUNT; i++)
            if (!strcmp(tok, s_src[i].name)) { m |= BIT(i); any = 1; break; }
    }
    if (!any) return -1;
    *mask = m;
    return 0;
}

void h64_padmap_format_sources(u32 mask, char *out, int size)
{
    int i, len = 0;
    if (size <= 0) return;
    out[0] = 0;
    for (i = 0; i < H64_SRC_COUNT; i++)
    {
        int n;
        if (!(mask & BIT(i))) continue;
        n = (int)strlen(s_src[i].name);
        if (len + (len ? 1 : 0) + n >= size) break;
        if (len) out[len++] = ',';
        memcpy(out + len, s_src[i].name, n + 1);
        len += n;
    }
    if (!len && size > 4) strcpy(out, "none");
}

// A whole number in [lo, hi]; returns 0 when `text` is not one.
static int parse_int(const char *text, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

int h64_padmap_parse_key(H64PadMap *m, const char *key, const char *value)
{
    int i;
    if (strncmp(key, "map", 3)) return 0;
    if (!strcmp(key, "map"))
    {
        if (!strcmp(value, "default")) h64_padmap_default(m);
        return 1;
    }
    if (key[3] != '_') return 0;
    key += 4;
    if (!strcmp(key, "stick"))
    {
        if (!strcmp(value, "left")) m->stick = H64_STICK_LEFT;
        else if (!strcmp(value, "right")) m->stick = H64_STICK_RIGHT;
        else if (!strcmp(value, "none")) m->stick = H64_STICK_NONE;
        return 1;
    }
    if (!strcmp(key, "deadzone")) { parse_int(value, 0, 32000, &m->deadzone); return 1; }
    if (!strcmp(key, "range")) { parse_int(value, 50, 150, &m->range); return 1; }
    for (i = 0; i < H64_N64_COUNT; i++)
        if (!strcmp(key, s_n64[i].name))
        {
            h64_padmap_parse_sources(value, &m->src[i]);
            return 1;
        }
    return 0;
}
