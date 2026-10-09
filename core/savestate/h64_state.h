// Harissa64 V2 - save states: the whole machine in one portable buffer.
//
// Every field is written big-endian one by one, so a state made on the
// console loads in Xenia and on the hosts (and the other way round) for the
// same version of the format and the same ROM. Large memories (RDRAM, its
// hidden bits, save media) are run-length coded.
//
// A state is taken only at a quiet point: no HLE RSP task in progress (as
// mupen64plus does, the RSP HLE keeps nothing between tasks that a state
// would need). An LLE RSP task may be running: its registers and memories are
// part of the state. Loading resets what is derived from the machine state:
// the recompiler's code, the RSP HLE, and (platform side) the renderer's caches.
#ifndef H64_STATE_H
#define H64_STATE_H

#include <vector>

#include "../common/h64_types.h"

struct H64System;

#define H64_STATE_VERSION 2   // 2: RSP pipeline (version 1 still loads)

// 1 when a state can be taken now (no HLE task in progress).
int h64_state_quiet(H64System *sys);
// Appends the state to `out`. Returns 0, or -1 when not quiet.
int h64_state_save(H64System *sys, std::vector<u8> *out);
// Restores a state. Returns 0, or -1 (bad or truncated data, other format
// version, another ROM): then the machine is unchanged.
int h64_state_load(H64System *sys, const u8 *data, u32 size);

// Run-length coding used for the big blocks (exposed for the unit tests).
void h64_rle_encode(const u8 *in, u32 size, std::vector<u8> *out);
// Returns the number of input bytes consumed, or 0 on bad data.
u32 h64_rle_decode(const u8 *in, u32 inSize, u8 *out, u32 outSize);

#endif
