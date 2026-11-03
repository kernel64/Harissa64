// Harissa64 V2 - XAudio2 output (see xb_audio.h).
//
// Continuity, as V1 does it (xenon/xbox360_audio.cpp on master):
//  - Rate control: when the emulation runs a little slow (or fast), the
//    playback rate is steered toward a TARGET_MS fill of the queue with
//    SetFrequencyRatio (RATE_MIN..RATE_MAX, smoothed per submit), so a few
//    percent of speed difference stretches the sound instead of cutting it.
//    The fill is measured just before each submit.
//  - Re-buffering: after an underrun the voice is paused until START_MS is
//    queued again, which turns many tiny gaps into one short pause.
#include "xb_audio.h"

#include <xtl.h>
#include <xaudio2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/common/h64_log.h"

#define SLOTS 64   // one game buffer (~23 ms) per slot: 500 ms of margin needs ~22
#define SLOT_BYTES 16384
static int s_targetMs = XB_AUDIO_DEFAULT_MS;   // fill the rate control aims for (the pacing threshold)
#define TARGET_MS s_targetMs
#define START_MS (s_targetMs * 3 / 5)          // queued before (re)starting playback
#define RATE_GAIN 0.10f                    // rate change at an empty queue
#define RATE_MIN 0.90f
#define RATE_MAX 1.02f
#define RATE_SMOOTH 0.05f                  // per submit, to avoid audible wobble

static IXAudio2 *s_xa;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_voice;
static u32 s_rate;
static u8 s_slots[SLOTS][SLOT_BYTES];
static int s_next;
static DWORD s_lastSubmit;
static XbAudioStats s_stats;
static u64 s_framesSubmitted;
static int s_playing;
static float s_ratio, s_ratioApplied;
static u64 s_periodFrames;   // frames submitted since the last stats reset
static char s_dumpPath[64];  // wavdump=: where the sound goes, and the buffer filling up
static u8 *s_dump;
static u32 s_dumpBytes, s_dumpCap, s_dumpRate, s_dumpSec;

void xb_audio_dump(const char *path, int seconds)
{
    if (seconds <= 0 || seconds > 120) return;
    s_dumpSec = (u32)seconds;
    s_dumpCap = (u32)seconds * 48000u * 4u;
    s_dump = (u8 *)malloc(s_dumpCap);
    s_dumpBytes = 0;
    if (s_dump) { strncpy(s_dumpPath, path, sizeof(s_dumpPath) - 1); s_dumpPath[sizeof(s_dumpPath) - 1] = 0; }
}

static void put32le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static void dump_write(void)
{
    u8 h[44];
    u32 i;
    FILE *f;
    memcpy(h, "RIFF", 4); put32le(h + 4, 36 + s_dumpBytes); memcpy(h + 8, "WAVEfmt ", 8);
    put32le(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0; put32le(h + 24, s_dumpRate);
    put32le(h + 28, s_dumpRate * 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); put32le(h + 40, s_dumpBytes);
    for (i = 0; i + 1 < s_dumpBytes; i += 2) { u8 t = s_dump[i]; s_dump[i] = s_dump[i + 1]; s_dump[i + 1] = t; }   // big- to little-endian
    f = fopen(s_dumpPath, "wb");
    if (f) { fwrite(h, 1, 44, f); fwrite(s_dump, 1, s_dumpBytes, f); fclose(f); }
    H64_INFO("[audio] wavdump: %u frames at %u Hz written to %s (%s)", s_dumpBytes / 4, s_dumpRate, s_dumpPath, f ? "ok" : "failed");
    free(s_dump);
    s_dump = NULL;
}

int xb_audio_init(void)
{
    if (FAILED(XAudio2Create(&s_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        H64_ERROR("[audio] XAudio2Create failed");
        s_xa = NULL;
        return -1;
    }
    if (FAILED(s_xa->CreateMasteringVoice(&s_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL)))
    {
        H64_ERROR("[audio] CreateMasteringVoice failed");
        s_master = NULL;
        return -1;
    }
    return 0;
}

static void destroy_voice(void)
{
    if (s_voice)
    {
        s_voice->Stop(0);
        s_voice->FlushSourceBuffers();
        s_voice->DestroyVoice();
        s_voice = NULL;
    }
}

void xb_audio_shutdown(void)
{
    destroy_voice();
    if (s_master) { s_master->DestroyVoice(); s_master = NULL; }
    if (s_xa) { s_xa->Release(); s_xa = NULL; }
}

static int create_voice(u32 rate)
{
    WAVEFORMATEX wf;
    destroy_voice();
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (FAILED(s_xa->CreateSourceVoice(&s_voice, &wf, 0, XAUDIO2_MAX_FREQ_RATIO, NULL, NULL, NULL)))
    {
        H64_ERROR("[audio] CreateSourceVoice(%u Hz) failed", rate);
        s_voice = NULL;
        return -1;
    }
    // Not started: playback begins once START_MS is queued.
    s_rate = rate;
    s_framesSubmitted = 0;
    s_playing = 0;
    s_ratio = s_ratioApplied = 1.0f;
    H64_INFO("[audio] voice at %u Hz", rate);
    return 0;
}

// Stereo frames queued and not played yet; *queued = buffers still held by the voice.
static u32 pending_frames(u32 *queued)
{
    XAUDIO2_VOICE_STATE vs;
    s_voice->GetState(&vs);
    *queued = vs.BuffersQueued;
    return vs.SamplesPlayed >= s_framesSubmitted ? 0 : (u32)(s_framesSubmitted - vs.SamplesPlayed);
}

void xb_audio_sink(void *user, const u8 *samples, u32 len, u32 rate)
{
    XAUDIO2_BUFFER b;
    u32 queued, pending, frames;
    (void)user;
    if (!s_xa || !s_master || len == 0 || rate < 4000 || rate > 96000) return;
    if (s_dump)
    {
        u32 n = len & ~3u;
        if (!s_dumpRate) s_dumpRate = rate;
        if (n > s_dumpCap - s_dumpBytes) n = s_dumpCap - s_dumpBytes;
        memcpy(s_dump + s_dumpBytes, samples, n);
        s_dumpBytes += n;
        if (s_dumpBytes >= s_dumpRate * 4u * s_dumpSec || s_dumpBytes == s_dumpCap) dump_write();
    }
    if (!s_voice || rate != s_rate)
        if (create_voice(rate)) return;
    pending = pending_frames(&queued);
    if (s_playing && queued == 0)
    {
        // Ran dry: pause and build a small reserve before playing again.
        s_voice->Stop(0);
        s_playing = 0;
        s_stats.underruns++;
    }
    if (queued >= SLOTS - 1) { s_stats.dropped++; return; }   // full: drop rather than overwrite a queued slot
    if (len > SLOT_BYTES) len = SLOT_BYTES;
    len &= ~3u;
    frames = len / 4;
    memcpy(s_slots[s_next], samples, len);
    memset(&b, 0, sizeof(b));
    b.AudioBytes = len;
    b.pAudioData = s_slots[s_next];
    s_voice->SubmitSourceBuffer(&b, NULL);
    s_next = (s_next + 1) % SLOTS;
    s_framesSubmitted += frames;
    s_periodFrames += frames;
    s_stats.buffers++;
    s_lastSubmit = GetTickCount();

    if (!s_playing)
    {
        if (pending + frames >= s_rate * START_MS / 1000 || queued + 1 >= SLOTS - 1)
        {
            s_voice->Start(0, XAUDIO2_COMMIT_NOW);
            s_playing = 1;
        }
        return;
    }
    {
        // Rate control on the fill before this submit.
        float fill = (float)pending / (float)(s_rate * TARGET_MS / 1000);
        float target = 1.0f + RATE_GAIN * (fill - 1.0f);
        if (target < RATE_MIN) target = RATE_MIN;
        if (target > RATE_MAX) target = RATE_MAX;
        s_ratio += (target - s_ratio) * RATE_SMOOTH;
        if (fabsf(s_ratio - s_ratioApplied) > 0.002f)
        {
            s_voice->SetFrequencyRatio(s_ratio, XAUDIO2_COMMIT_NOW);
            s_ratioApplied = s_ratio;
        }
        s_stats.ratioPermille = (u32)(s_ratio * 1000.0f + 0.5f);
        s_stats.fillMs = (u32)((u64)pending * 1000 / s_rate);
    }
}

int xb_audio_queued_ms(void)
{
    u32 queued, pending;
    if (!s_voice || GetTickCount() - s_lastSubmit > 250) return -1;
    pending = pending_frames(&queued);
    // A paused voice does not drain: never make the emulation wait on it.
    if (!s_playing) return 0;
    return (int)((u64)pending * 1000 / s_rate);
}

void xb_audio_stats(XbAudioStats *out, int reset)
{
    static u64 lastPlayed;
    static LARGE_INTEGER freq;
    static u64 lastTick;
    LARGE_INTEGER t;
    u64 now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    now = (u64)t.QuadPart * 1000 / (u64)freq.QuadPart;   // ms of real time (GetTickCount falls behind on the console)
    u64 played = 0;
    if (s_voice)
    {
        XAUDIO2_VOICE_STATE vs;
        s_voice->GetState(&vs);
        played = vs.SamplesPlayed;
    }
    s_stats.playedHz = (lastTick && now != lastTick && played >= lastPlayed) ? (u32)((played - lastPlayed) * 1000 / (now - lastTick)) : 0;
    s_stats.submittedHz = (lastTick && now != lastTick) ? (u32)(s_periodFrames * 1000 / (now - lastTick)) : 0;
    *out = s_stats;
    if (reset) { lastPlayed = played; lastTick = now; s_periodFrames = 0; }
    if (reset)
    {
        u32 ratio = s_stats.ratioPermille, fill = s_stats.fillMs;
        memset(&s_stats, 0, sizeof(s_stats));
        s_stats.ratioPermille = ratio;
        s_stats.fillMs = fill;
    }
}

void xb_audio_set_target_ms(int ms)
{
    s_targetMs = ms < 40 ? 40 : ms > 500 ? 500 : ms;
}

int xb_audio_target_ms(void) { return s_targetMs; }
