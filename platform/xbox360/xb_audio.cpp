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
#include <string.h>

#include "../../core/common/h64_log.h"

#define SLOTS 32
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
    if (queued >= SLOTS - 1) return;   // full: drop rather than overwrite a queued slot
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
    static DWORD lastTick;
    DWORD now = GetTickCount();
    u64 played = 0;
    if (s_voice)
    {
        XAUDIO2_VOICE_STATE vs;
        s_voice->GetState(&vs);
        played = vs.SamplesPlayed;
    }
    s_stats.playedHz = (lastTick && now != lastTick && played >= lastPlayed) ? (u32)((played - lastPlayed) * 1000 / (now - lastTick)) : 0;
    *out = s_stats;
    if (reset) { lastPlayed = played; lastTick = now; }
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
