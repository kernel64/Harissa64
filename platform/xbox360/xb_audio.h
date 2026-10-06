// Harissa64 V2 - XAudio2 output of the AI buffers (Xbox 360).
//
// Each AI buffer is copied into a ring of slots and queued unmodified: the
// N64 writes big-endian 16-bit stereo, which is the console's native PCM.
// The source voice runs at the game's DAC rate (XAudio2 resamples) and is
// recreated when the rate changes. The emulation is paced by the queue: the
// main loop waits while more than XB_AUDIO_MAX_QUEUED_MS are queued.
#ifndef XB_AUDIO_H
#define XB_AUDIO_H

#include "../../core/common/h64_types.h"

#define XB_AUDIO_MAX_QUEUED_MS 80

int xb_audio_init(void);
void xb_audio_shutdown(void);
// The AI sink (H64System::aiSink).
void xb_audio_sink(void *user, const u8 *samples, u32 len, u32 rate);
// Milliseconds of sound queued and not played yet (-1: no sound queued for
// a while, pace on the clock instead).
int xb_audio_queued_ms(void);

struct XbAudioStats { u32 buffers, underruns; };
void xb_audio_stats(XbAudioStats *out, int reset);

#endif
