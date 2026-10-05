// Harissa64 V2 - event scheduler.
//
// One timeline in CPU cycles (PClock, 93.75 MHz). Every timed device event
// (Count/Compare, VI lines, AI/PI/SI DMA completion, RSP/RDP tasks) is an
// entry with an absolute timestamp; the run loop executes CPU instructions
// until the earliest one is due. No hidden timing: everything that happens
// "later" goes through here.
#ifndef H64_SCHEDULER_H
#define H64_SCHEDULER_H

#include "../common/h64_types.h"

enum H64Event
{
    H64_EV_COMPARE = 0,   // Count reached Compare
    H64_EV_VI,            // VI reached VI_V_INTR
    H64_EV_AI,            // AI DMA buffer finished
    H64_EV_PI,            // PI DMA finished
    H64_EV_SI,            // SI DMA finished
    H64_EV_SP,            // RSP task finished (M1 stub)
    H64_EV_DP,            // RDP finished (M1 stub)
    H64_EV_COUNT
};

#define H64_NEVER 0xFFFFFFFFFFFFFFFFull

struct H64Scheduler
{
    u64 when[H64_EV_COUNT];
    u64 next;        // earliest of when[]
};

void h64_sched_init(H64Scheduler *s);
void h64_sched_set(H64Scheduler *s, int ev, u64 when);
void h64_sched_cancel(H64Scheduler *s, int ev);
// Returns the earliest event due at `now` (and clears it), or -1.
int h64_sched_pop_due(H64Scheduler *s, u64 now);

#endif
