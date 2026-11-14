/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* Restart when the output stops draining. Under Wine XAudio2 is FAudio,
 * whose thread just exits when the sound device goes away (USB / Bluetooth
 * / HDMI sink changes): no OnCriticalError, sound gone for the session
 * (r-linux-proton.md R5a). Windows' XAudio2 stops the same way on a device
 * change. Our queue then stays full; after XA2_STALL_MS of that (not
 * paused) the engine and voices are made again on the default device. The
 * whole queue is ~64 ms, so a draining output is never full that long.
 * RECOMP_XA2_STALL_TEST=<s>: stop the voice <s> seconds after the start,
 * as a lost device would, to try it. */
#define XA2_STALL_MS      500
#define XA2_RETRY_MS      2000   /* between attempts while no device works */
static SRWLOCK                 g_xa2_lock = SRWLOCK_INIT;
static int                     g_xa2_paused;
static ULONGLONG               g_xa2_full_since;  /* 0: not seen full */
static ULONGLONG               g_xa2_next_try;    /* after a failed restart */
static ULONGLONG               g_xa2_started;
static int                     g_xa2_restarts;
static int                     g_xa2_stall_test = -1;   /* -1: not read */
static int                     g_xa2_want;        /* init asked for output */

static void xa2_teardown(void);

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_want = 1;
    g_xa2_full_since = 0;
    g_xa2_started = GetTickCount64();

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_teardown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

/* For good: no restart after this. */
void xa2_shutdown(void)
{
    AcquireSRWLockExclusive(&g_xa2_lock);
    g_xa2_want = 0;
    xa2_teardown();
    ReleaseSRWLockExclusive(&g_xa2_lock);
}

static void xa2_teardown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* The queue has been full for XA2_STALL_MS: make everything again. Called
 * with g_xa2_lock held, on the APU frame thread (COM is up there). */
static void xa2_restart(ULONGLONG now, ULONGLONG stalled_ms)
{
    int restarts = g_xa2_restarts + 1;
    int frames = g_xa2_frames_written;
    int paused = g_xa2_paused;

    if (restarts <= 10)
        fprintf(stderr, "[XA2] output stalled %llu ms (sound device lost?):"
                " restarting XAudio2 (restart %d)\n",
                (unsigned long long)stalled_ms, restarts);
    xa2_teardown();
    if (xa2_init()) {
        g_xa2_frames_written = frames;
        if (paused) {
            IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
            g_xa2_paused = 1;
        }
    } else {
        g_xa2_next_try = now + XA2_RETRY_MS;
        if (restarts <= 10)
            fprintf(stderr, "[XA2] restart failed: trying again every %d ms\n",
                    XA2_RETRY_MS);
    }
    g_xa2_restarts = restarts;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;
    ULONGLONG now;

    AcquireSRWLockExclusive(&g_xa2_lock);
    now = GetTickCount64();
    if (!g_xa2_initialized && g_xa2_want && now >= g_xa2_next_try)
        xa2_restart(now, 0);
    if (!g_xa2_initialized || !g_xa2_source) {
        ReleaseSRWLockExclusive(&g_xa2_lock);
        return 0;
    }
    if (g_xa2_stall_test < 0) {
        const char *e = getenv("RECOMP_XA2_STALL_TEST");
        g_xa2_stall_test = e ? atoi(e) : 0;
    }
    if (g_xa2_stall_test > 0 && !g_xa2_restarts
            && now - g_xa2_started > (ULONGLONG)g_xa2_stall_test * 1000) {
        fprintf(stderr, "[XA2] RECOMP_XA2_STALL_TEST: voice stopped as a lost device would\n");
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        g_xa2_stall_test = 0;
    }

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        if (g_xa2_paused)
            g_xa2_full_since = 0;
        else if (!g_xa2_full_since)
            g_xa2_full_since = now;
        else if (now - g_xa2_full_since >= XA2_STALL_MS)
            xa2_restart(now, now - g_xa2_full_since);
        ReleaseSRWLockExclusive(&g_xa2_lock);
        return 0;
    }
    g_xa2_full_since = 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&g_xa2_lock);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    ReleaseSRWLockExclusive(&g_xa2_lock);
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

int xa2_queued_buffers(void)
{
    XAUDIO2_VOICE_STATE state;
    int n = -1;

    AcquireSRWLockExclusive(&g_xa2_lock);
    if (g_xa2_initialized && g_xa2_source) {
        IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        n = (int)state.BuffersQueued;
    }
    ReleaseSRWLockExclusive(&g_xa2_lock);
    return n;
}

void xa2_set_paused(int on)
{
    AcquireSRWLockExclusive(&g_xa2_lock);
    g_xa2_paused = on;
    g_xa2_full_since = 0;
    if (g_xa2_initialized && g_xa2_source) {
        if (on)
            IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        else
            IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    }
    ReleaseSRWLockExclusive(&g_xa2_lock);
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
int  xa2_queued_buffers(void)                         { return -1; }
void xa2_set_paused(int on)                           { (void)on; }

#endif /* _WIN32 */
