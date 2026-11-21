// Harissa64 V2 - render test scenes (docs-local/render_tests_study.md).
//
// A scene is a ParaLLEl-RDP RDPDUMP2 file (rdp_dump_write.cpp, MIT) with two
// differences in meaning, so only our tools play it (h64test --rt-play,
// tools/rdp_oracle):
//  - RDRAM records (1) are the bytes the CPU, the RSP and DMAs changed since
//    the previous command (exact runs of 32-bit words), applied at once;
//    UpdateDramFlush (7) only marks the end of a group. The recording runs
//    in the GPU-renderer configuration (the software RDP keeps state and
//    draws nothing), so RDRAM never holds what the RDP drew: each renderer
//    keeps its own drawing, as on the console.
//  - Record 0x48 (H64_RT_HLE_TRIANGLE) comes before the RDP triangle command
//    that stands for an HLE triangle: flags, tile, levels, then the three
//    H64RenderVertex (10 floats each, bit patterns). The software RDP and the
//    oracles draw the RDP command; a GPU renderer takes the triangle and
//    skips the command after it.
// A scene starts with the whole RDRAM and hidden RDRAM, TMEM loaded through
// RDRAM block 0, and the last command of each kind that sets RDP state.
// Frames end with the 14 VI registers and EndFrame (one per VI).
#ifndef H64_RTDUMP_H
#define H64_RTDUMP_H

#include "../../core/common/h64_types.h"

struct H64System;
struct H64RenderVertex;

#define H64_RT_HLE_TRIANGLE 0x48u

// Recording (h64test --rdp-dump FIRST:COUNT:FILE). The hooks are called for
// every command and triangle a GPU renderer receives, from the start of the
// run (the state cache needs them); recording starts at VI frame `first`.
int h64_rtdump_open(const char *path, u32 first, u32 count);
void h64_rtdump_rdp(H64System *sys, const u64 *words, u32 n);
void h64_rtdump_triangle(H64System *sys, const H64RenderVertex *a, const H64RenderVertex *b,
                         const H64RenderVertex *c, u32 flags, u32 tile, u32 levels);
void h64_rtdump_vi(H64System *sys);   // at each VI event
int h64_rtdump_active(void);          // 1 while recording or before it starts
void h64_rtdump_close(void);

// Replay with the software RDP (h64test --rt-play DUMP OUTDIR [--frames all|last|N,..]):
// writes OUTDIR/soft_f<N>.rdram (N64 byte order) and .hidden, as rdp-oracle does.
int h64_rtdump_play(int argc, char **argv);

#endif
