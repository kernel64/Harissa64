// Harissa64 V2 - render test scenes: playback (h64_rtscene.h).
#include "h64_rtscene.h"

#include <string.h>

#include "../rdp/h64_rdp.h"
#include "../rdp/h64_rdp_state.h"
#include "../system/h64_system.h"
#include "../../render/api.h"

static int rd(H64RtScene *s, u32 *v)
{
    const u8 *p;
    if (s->pos + 4 > s->size) return 0;
    p = s->data + s->pos;
    *v = (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
    s->pos += 4;
    return 1;
}

int h64_rtscene_open(H64RtScene *s, const u8 *data, u32 size)
{
    u32 ram, hid;
    memset(s, 0, sizeof(*s));
    s->data = data;
    s->size = size;
    if (size < 16 || memcmp(data, "RDPDUMP2", 8)) return -1;
    s->pos = 8;
    if (!rd(s, &ram) || !rd(s, &hid) || ram != H64_RDRAM_SIZE || hid != H64_RDRAM_SIZE / 2) return -1;
    return 0;
}

u32 h64_rtscene_max_color_width(const u8 *data, u32 size)
{
    H64RtScene s;
    u32 cmd, a, b, hi, best = 0;
    if (h64_rtscene_open(&s, data, size)) return 0;
    while (rd(&s, &cmd) && cmd != 6)
    {
        if (cmd == 1 || cmd == 8)
        {
            if (!rd(&s, &a) || !rd(&s, &b) || s.pos + b > size) break;
            s.pos += b;
        }
        else if (cmd == 2)
        {
            if (!rd(&s, &a) || !rd(&s, &b) || s.pos + b * 4 > size || !b) break;
            if (!rd(&s, &hi)) break;
            if (((hi >> 24) & 0x3F) == 0x3F && (hi & 0x3FF) + 1 > best) best = (hi & 0x3FF) + 1;
            s.pos += (b - 1) * 4;
        }
        else if (cmd == 3) { if (!rd(&s, &a) || !rd(&s, &b)) break; }
        else if (cmd == H64_RT_REC_HLE_TRIANGLE) { if (s.pos + 33 * 4 > size) break; s.pos += 33 * 4; }
        else if (cmd != 4 && cmd != 5 && cmd != 7 && cmd != 9) break;
    }
    return best;
}

static float as_float(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static int read_vertex(H64RtScene *s, H64RenderVertex *v)
{
    u32 w[10], k;
    for (k = 0; k < 10; k++)
        if (!rd(s, &w[k])) return 0;
    v->x = as_float(w[0]); v->y = as_float(w[1]); v->z = as_float(w[2]); v->invw = as_float(w[3]);
    v->r = as_float(w[4]); v->g = as_float(w[5]); v->b = as_float(w[6]); v->a = as_float(w[7]);
    v->s = as_float(w[8]); v->t = as_float(w[9]);
    return 1;
}

static int bad(H64RtScene *s)
{
    s->error = 1;
    return -1;
}

int h64_rtscene_play_frame(H64RtScene *s, H64System *sys, H64Renderer *r)
{
    u64 words[22];
    u32 cmd, a, b, k;
    int haveTri = 0;
    H64RenderVertex tv[3];
    u32 tflags = 0, ttile = 0, tlevels = 0;
    if (s->error) return -1;
    for (;;)
    {
        if (!rd(s, &cmd)) return 0;
        switch (cmd)
        {
        case 1:   // RDRAM bytes
            if (!rd(s, &a) || !rd(s, &b) || (a | b) & 3 || a + b > H64_RDRAM_SIZE || a + b < a || s->pos + b > s->size) return bad(s);
            for (k = 0; k < b; k++) sys->rdram[a + k] = s->data[s->pos + (k ^ 3)];
            s->pos += b;
            break;
        case 8:   // hidden RDRAM bytes
            if (!rd(s, &a) || !rd(s, &b) || a + b > H64_RDRAM_SIZE / 2 || a + b < a || s->pos + b > s->size) return bad(s);
            h64_rdp_state(sys);   // allocates the hidden RDRAM
            memcpy(sys->rdramHidden + a, s->data + s->pos, b);
            s->pos += b;
            break;
        case H64_RT_REC_HLE_TRIANGLE:
            if (!rd(s, &tflags) || !rd(s, &ttile) || !rd(s, &tlevels)) return bad(s);
            for (k = 0; k < 3; k++)
                if (!read_vertex(s, &tv[k])) return bad(s);
            haveTri = 1;
            break;
        case 2:   // RDP command
            if (!rd(s, &a) || !rd(s, &b) || b < 2 || b > 44 || (b & 1)) return bad(s);
            for (k = 0; k < b / 2; k++)
            {
                u32 hi, lo;
                if (!rd(s, &hi) || !rd(s, &lo)) return bad(s);
                words[k] = (u64)hi << 32 | lo;
            }
            if (haveTri && r)
            {
                r->triangle(r->user, &tv[0], &tv[1], &tv[2], tflags, ttile, tlevels);
                s->triangles++;
            }
            else if (r) r->rdp(r->user, words, b / 2);
            else h64_rdp_command(sys, words, b / 2);
            haveTri = 0;
            s->commands++;
            break;
        case 3:   // VI register
            if (!rd(s, &a) || !rd(s, &b)) return bad(s);
            if (a < 14) sys->vi.regs[a] = b;
            break;
        case 4:
            s->frame++;
            return 1;
        case 6:
            return 0;
        case 5: case 7: case 9:
            break;
        default:
            return bad(s);
        }
    }
}
