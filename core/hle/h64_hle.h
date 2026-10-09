// Harissa64 V2 - RSP task HLE (high-level emulation of RSP microcode tasks).
//
// When the CPU starts the RSP on an OSTask, the HLE recognises the
// microcode and runs the whole task at once in C++; anything it does not
// recognise is left to the LLE RSP interpreter, so HLE is always optional.
// For now only audio tasks are recognised (alist ABIs and MusyX, port of
// mupen64plus-rsp-hle, see h64_hle_internal.h).
//
// Timing: the task's effects (RDRAM writes) happen when it starts, and the
// RSP stays busy (SP_STATUS halt clear) for a fixed time, after which it
// halts, sets its break/task-done bits and raises its interrupt like the
// real microcode. The time is an approximation (H64_HLE_AUDIO_CYCLES).
#ifndef H64_HLE_H
#define H64_HLE_H

#include "../common/h64_types.h"

struct H64System;
struct hle_t;

// CPU cycles an HLE task keeps the RSP busy.
#define H64_HLE_AUDIO_CYCLES 20000u
#define H64_HLE_GFX_CYCLES 100000u

struct hle_t *h64_hle_create(H64System *sys);
void h64_hle_free(struct hle_t *hle);

// Called when the RSP leaves the halt state. Returns 1 when the HLE ran the
// task: the RSP must then stay busy for `*busyCycles` CPU cycles and finish
// by setting the SP_STATUS bits in `*statusBits` (halt and break included),
// and raise the DP interrupt then if `*dpInterrupt` (graphics tasks that end
// with an RDP full sync). Returns 0 to let the LLE RSP run the task.
int h64_hle_try_task(H64System *sys, u32 *statusBits, u32 *busyCycles, int *dpInterrupt);
int h64_hle_gfx_measure(H64System *sys, u32 *verts, u32 *tris, u32 *commands);
u32 h64_hle_gfx_cost(H64System *sys);   // the last graphics task's estimated RSP time (0: timing off)

// Asynchronous graphics tasks: waits for the worker (if a task is pending).
void h64_hle_async_wait(H64System *sys);
// 1 when no HLE task is in progress on a worker (its end not consumed yet).
int h64_hle_idle(H64System *sys);
// At the end of an HLE task's busy time: 0 if it ran synchronously, 1 if it
// was an asynchronous graphics task, 2 an asynchronous audio task. Then
// *ran = 0 means it fell back (the LLE RSP must run it now), *fullSync whether
// to raise the DP interrupt, and for audio *statusBits the SP_STATUS bits the
// task set.
int h64_hle_async_finish(H64System *sys, int *ran, int *fullSync, u32 *statusBits);

// hleAudioCheck: called when the LLE RSP halts; compares its RDRAM output
// with the HLE's and logs the first difference.
void h64_hle_check_end(H64System *sys);

#endif
