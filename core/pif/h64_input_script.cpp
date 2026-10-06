// Harissa64 V2 - scripted controller input (see h64_input_script.h).
#include "h64_input_script.h"

#include <stdlib.h>
#include <string.h>

#include "../common/h64_log.h"
#include "../system/h64_system.h"

int h64_input_script_parse(H64InputScript *s, const char *script)
{
    static const struct { const char *name; u16 bit; } names[] = {
        { "A", 0x8000 }, { "B", 0x4000 }, { "Z", 0x2000 }, { "START", 0x1000 }, { "DU", 0x0800 }, { "DD", 0x0400 },
        { "DL", 0x0200 }, { "DR", 0x0100 }, { "L", 0x0020 }, { "R", 0x0010 }, { "CU", 0x0008 }, { "CD", 0x0004 },
        { "CL", 0x0002 }, { "CR", 0x0001 } };
    const char *p = script;
    while (*p && s->count < 64)
    {
        H64InputEvent *e = &s->events[s->count];
        char name[16];
        int n = 0;
        unsigned i;
        memset(e, 0, sizeof(*e));
        while (*p && *p != '@' && *p != '=' && n < 15) name[n++] = *p++;
        name[n] = 0;
        if (*p == '=') { e->axis = name[0] == 'X' ? 1 : 2; e->value = (int)strtol(p + 1, (char **)&p, 10); }
        else
            for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strcmp(name, names[i].name)) e->buttons = names[i].bit;
        if (!e->buttons && !e->axis) { H64_ERROR("[input] bad event \"%s\"", name); return -1; }
        if (*p != '@') { H64_ERROR("[input] event \"%s\" needs @frame", name); return -1; }
        e->first = e->last = (u32)strtoul(p + 1, (char **)&p, 10);
        if (*p == '-') e->last = (u32)strtoul(p + 1, (char **)&p, 10);
        if (*p == ',') p++;
        s->count++;
    }
    return 0;
}

void h64_input_script_apply(const H64InputScript *s, H64System *sys)
{
    int i;
    sys->pad[0].buttons = 0;
    sys->pad[0].x = sys->pad[0].y = 0;
    for (i = 0; i < s->count; i++)
        if (sys->vi.frames >= s->events[i].first && sys->vi.frames <= s->events[i].last)
        {
            sys->pad[0].buttons |= s->events[i].buttons;
            if (s->events[i].axis == 1) sys->pad[0].x = (s8)s->events[i].value;
            if (s->events[i].axis == 2) sys->pad[0].y = (s8)s->events[i].value;
        }
}
