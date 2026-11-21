// Harissa64 V2 - render test scenes: playback (docs-local/render_tests_study.md).
//
// A scene is a ParaLLEl-RDP RDPDUMP2 file (layout from its rdp_dump_write.cpp,
// MIT) as h64test --rdp-dump writes it (platform/host/h64_rtdump.h):
//   "RDPDUMP2", RDRAM size, hidden RDRAM size, then records of 32-bit
//   little-endian words: 1 RDRAM bytes (offset, size, data), 2 RDP command
//   (opcode, word count, words), 3 VI register (index, value), 4 end of
//   frame, 5 signal, 6 end of file, 7/9 group markers, 8 hidden RDRAM bytes,
//   0x48 HLE triangle (flags, tile, levels, three H64RenderVertex as 10 floats).
// RDRAM data is in the PC's 32-bit word order: N64 byte A is file byte A ^ 3.
// RDRAM records are applied at once (they are what the CPU, RSP and DMAs
// changed); the renderers keep their own drawing.
//
// Playback works on every target (the console plays scenes through Xenos).
#ifndef H64_RTSCENE_H
#define H64_RTSCENE_H

#include "../common/h64_types.h"

struct H64System;
struct H64Renderer;

#define H64_RT_REC_HLE_TRIANGLE 0x48u

struct H64RtScene
{
    const u8 *data;
    u32 size;
    u32 pos;        // next record
    u32 frame;      // frames ended so far
    u32 commands, triangles;
    int error;      // 1: bad data (playback stopped)
};

// Checks the header (8 MB RDRAM, 4 MB hidden RDRAM). Returns 0 or -1.
int h64_rtscene_open(H64RtScene *s, const u8 *data, u32 size);

// The widest colour image the scene sets (0 if none or bad data): a GPU
// replay picks its internal resolution from it.
u32 h64_rtscene_max_color_width(const u8 *data, u32 size);

// Plays records into `sys` (RDRAM, hidden RDRAM, VI registers) and the
// renderer `r` (NULL: the software RDP draws, HLE triangles as their RDP
// command; a renderer: it gets HLE triangles as triangles and every other
// command as RDP words) up to and including the next end of frame.
// Returns 1 at an end of frame, 0 at the end of the scene, -1 on bad data.
int h64_rtscene_play_frame(H64RtScene *s, H64System *sys, H64Renderer *r);

#endif
