// Harissa64 V2 - a small JSON reader (h64_json.h).
#include "h64_json.h"


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

static int json_value(const char **pp, const char *end, int depth, const std::string &k1, const std::string &k2, H64JsonFn fn, void *user);

static int json_object(const char **pp, const char *end, int depth, const std::string &k1, H64JsonFn fn, void *user)
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

static int json_value(const char **pp, const char *end, int depth, const std::string &k1, const std::string &k2, H64JsonFn fn, void *user)
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

int h64_json_walk(const char *text, u32 len, H64JsonFn fn, void *user)
{
    const char *p = text;
    std::string none;
    return json_value(&p, text + len, 0, none, none, fn, user);
}
