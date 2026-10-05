#include "h64_scheduler.h"

static void recompute(H64Scheduler *s)
{
    int i;
    s->next = H64_NEVER;
    for (i = 0; i < H64_EV_COUNT; i++)
        if (s->when[i] < s->next)
            s->next = s->when[i];
}

void h64_sched_init(H64Scheduler *s)
{
    int i;
    for (i = 0; i < H64_EV_COUNT; i++)
        s->when[i] = H64_NEVER;
    s->next = H64_NEVER;
}

void h64_sched_set(H64Scheduler *s, int ev, u64 when)
{
    s->when[ev] = when;
    recompute(s);
}

void h64_sched_cancel(H64Scheduler *s, int ev)
{
    s->when[ev] = H64_NEVER;
    recompute(s);
}

int h64_sched_pop_due(H64Scheduler *s, u64 now)
{
    int i, best = -1;
    if (s->next > now)
        return -1;
    for (i = 0; i < H64_EV_COUNT; i++)
        if (s->when[i] <= now && (best < 0 || s->when[i] < s->when[best]))
            best = i;
    if (best >= 0)
    {
        s->when[best] = H64_NEVER;
        recompute(s);
    }
    return best;
}
