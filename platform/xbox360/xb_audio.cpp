// Harissa64 V2 - XAudio2 output (see xb_audio.h).
#include "xb_audio.h"

#include <xtl.h>
#include <xaudio2.h>
#include <string.h>

#include "../../core/common/h64_log.h"

#define SLOTS 32
#define SLOT_BYTES 16384

static IXAudio2 *s_xa;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_voice;
static u32 s_rate;
static u8 s_slots[SLOTS][SLOT_BYTES];
static u32 s_slotBytes[SLOTS];
static int s_next;
static DWORD s_lastSubmit;
static XbAudioStats s_stats;

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
    s_voice->Start(0, XAUDIO2_COMMIT_NOW);
    s_rate = rate;
    H64_INFO("[audio] voice at %u Hz", rate);
    return 0;
}

void xb_audio_sink(void *user, const u8 *samples, u32 len, u32 rate)
{
    XAUDIO2_BUFFER b;
    XAUDIO2_VOICE_STATE vs;
    (void)user;
    if (!s_xa || !s_master || len == 0 || rate < 4000 || rate > 96000) return;
    if (!s_voice || rate != s_rate)
        if (create_voice(rate)) return;
    s_voice->GetState(&vs);
    if (vs.BuffersQueued >= SLOTS - 1) return;   // full: drop rather than overwrite a queued slot
    if (vs.BuffersQueued == 0 && s_stats.buffers) s_stats.underruns++;
    if (len > SLOT_BYTES) len = SLOT_BYTES;
    len &= ~3u;
    memcpy(s_slots[s_next], samples, len);
    s_slotBytes[s_next] = len;
    memset(&b, 0, sizeof(b));
    b.AudioBytes = len;
    b.pAudioData = s_slots[s_next];
    s_voice->SubmitSourceBuffer(&b, NULL);
    s_next = (s_next + 1) % SLOTS;
    s_stats.buffers++;
    s_lastSubmit = GetTickCount();
}

int xb_audio_queued_ms(void)
{
    XAUDIO2_VOICE_STATE vs;
    u32 bytes = 0, i, n;
    if (!s_voice || GetTickCount() - s_lastSubmit > 250) return -1;
    s_voice->GetState(&vs);
    // The queued slots are the last BuffersQueued submitted.
    n = vs.BuffersQueued;
    for (i = 1; i <= n && i <= SLOTS; i++) bytes += s_slotBytes[(s_next + SLOTS - i) % SLOTS];
    return (int)((u64)bytes * 1000 / ((u64)s_rate * 4));
}

void xb_audio_stats(XbAudioStats *out, int reset)
{
    *out = s_stats;
    if (reset) memset(&s_stats, 0, sizeof(s_stats));
}
