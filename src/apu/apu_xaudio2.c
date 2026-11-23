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
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3      /* queued at most, by default */
#define XA2_MAX_BUFS      6      /* output pacing (apu_core.c): xa2_set_max_buffers */
#define XA2_MAX_CH        6      /* 5.1: FL FR C LFE BL BR */
#define XA2_MAX_DEV       32     /* device channels our output matrix covers */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_MAX_BUFS][XA2_BUF_SAMPLES * XA2_MAX_CH];
static int                     g_xa2_want_ch = XA2_CHANNELS; /* xa2_set_channels */
static int                     g_xa2_src_ch = XA2_CHANNELS;  /* the source voice's */
static int                     g_xa2_dev_ch;                 /* 0: not asked */
static float                   g_xa2_fold_lfe;
static float                   g_xa2_matrix[XA2_MAX_CH * XA2_MAX_DEV];
static int                     g_xa2_num_bufs = XA2_NUM_BUFS;
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

/* Restart when the output stops draining. Under Wine XAudio2 is FAudio,
 * whose thread just exits when the sound device goes away (USB / Bluetooth
 * / HDMI sink changes): no OnCriticalError, sound gone for the session
 * (r-linux-proton.md R5a). Windows' XAudio2 stops the same way on a device
 * change. Our queue then stays full; after XA2_STALL_MS of that (not
 * paused) the engine and voices are made again on the default device. The
 * whole queue is ~128 ms (~64 ms with RECOMP_APU_PACE=0: 3 buffers), so a
 * draining output is never full that long.
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

/* A default sound output exists. Wine's XAudio2 (FAudio) dereferences null
 * in CreateMasteringVoice when there is none (E_NOTFOUND from mmdevapi; seen
 * in WSL2 with Wine 11.21, 2026-11-14), where Windows' just fails: so no
 * engine is made without one -- at the start (the APU falls back to waveOut)
 * and in a restart after a lost device (retried until one is back). The
 * GUIDs are spelt out here so no extra import library is needed. */
static int xa2_output_present(void)
{
    static const GUID clsid_enum = { 0xBCDE0395, 0xE52F, 0x467C,
        { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
    static const GUID iid_enum = { 0xA95664D2, 0x9614, 0x4F35,
        { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&clsid_enum, NULL, CLSCTX_ALL, &iid_enum, (void **)&en);
    if (FAILED(hr) || !en)
        return 1;           /* can't ask: let XAudio2 try, as before */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (dev)
        IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* A spatial sound format (Windows Sonic / Dolby Atmos / DTS for headphones)
 * is on for the default output: Windows' spatial client offers it movable
 * sound objects (0 when none is on: every endpoint answers the static mask
 * and "7.1 supported" alike, so those can't tell). Windows 10 then still
 * offers apps the device's own stereo mix format, so XAudio2's default
 * channels are 2 (seen 2026-11-22: Sonic on Bluetooth headphones), but a
 * 7.1 stream is rendered by the spatial renderer, its speakers placed around
 * the listener. Wine's spatial client answers 0 objects. */
static int xa2_spatial_on(void)
{
    static const GUID clsid_enum = { 0xBCDE0395, 0xE52F, 0x467C,
        { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
    static const GUID iid_enum = { 0xA95664D2, 0x9614, 0x4F35,
        { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
    static const GUID iid_spatial = { 0xBBF8E066, 0xAAAA, 0x49BE,
        { 0x9A, 0x4D, 0xFD, 0x2A, 0x85, 0x8E, 0xA2, 0x7F } };
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    ISpatialAudioClient *sp = NULL;
    UINT32 objects = 0;

    if (FAILED(CoCreateInstance(&clsid_enum, NULL, CLSCTX_ALL, &iid_enum, (void **)&en)) || !en)
        return 0;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) && dev) {
        if (SUCCEEDED(IMMDevice_Activate(dev, &iid_spatial, CLSCTX_INPROC_SERVER, NULL,
                                         (void **)&sp)) && sp) {
            if (FAILED(ISpatialAudioClient_GetMaxDynamicObjectCount(sp, &objects)))
                objects = 0;
            ISpatialAudioClient_Release(sp);
        }
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(en);
    return objects > 0;
}

/* A speaker's channel number in a device's channel mask, or -1. */
static int xa2_speaker(DWORD mask, DWORD bit, int channels)
{
    int at = 0;
    if (!(mask & bit))
        return -1;
    for (DWORD b = 1; b < bit; b <<= 1)
        if (mask & b)
            at++;
    return at < channels ? at : -1;
}

/* 5.1 output (xa2_set_channels(6)): the mastering voice was opened with the
 * device's own channels. Six or more with a front pair and a surround pair:
 * fills g_xa2_matrix (our 6 channels onto the device's, row-major
 * [device channel x 6 + ours]) and returns 1. The surrounds go on the side
 * speakers where the device has them (Windows' 5.1 and the 7.1 that Windows
 * Sonic / Dolby Atmos for Headphones present: 5.1's surrounds are side
 * speakers, 110 degrees), else on the back pair; a missing centre is shared
 * by the front pair at -3 dB, a missing LFE left out. Else 0: our stereo
 * fold plays (xa2_fold6). */
static int xa2_surround_plan(void)
{
    const float h = 0.70710678f;
    XAUDIO2_VOICE_DETAILS det;
    DWORD mask = 0;
    int n, fl, fr, c, lfe, sl, sr;
    const char *why = NULL, *backs = "side";

    memset(&det, 0, sizeof det);
    IXAudio2MasteringVoice_GetVoiceDetails(g_xa2_master, &det);
    if (FAILED(IXAudio2MasteringVoice_GetChannelMask(g_xa2_master, &mask)))
        mask = 0;
    n = (int)det.InputChannels;
    g_xa2_dev_ch = n;
    {
        int named = 0;
        for (DWORD b = mask; b; b &= b - 1)
            named++;
        /* No speakers named, or fewer than the voice has (a 7.1 voice on a
         * stereo device with spatial sound on still reports the device's
         * 0x3): Windows' usual layouts. */
        if (named < n)
            mask = n >= 8 ? 0x63F : n >= 6 ? 0x60F : n == 2 ? 0x3 : 0;
    }
    fl = xa2_speaker(mask, 0x1, n);
    fr = xa2_speaker(mask, 0x2, n);
    c = xa2_speaker(mask, 0x4, n);
    lfe = xa2_speaker(mask, 0x8, n);
    sl = xa2_speaker(mask, 0x200, n);
    sr = xa2_speaker(mask, 0x400, n);
    if (sl < 0 || sr < 0) {
        sl = xa2_speaker(mask, 0x10, n);
        sr = xa2_speaker(mask, 0x20, n);
        backs = "back";
    }
    if (n < 6)
        why = "fewer than six speakers: our stereo fold";
    else if (n > XA2_MAX_DEV)
        why = "more channels than our matrix covers: our stereo fold";
    else if (fl < 0 || fr < 0 || sl < 0 || sr < 0)
        why = "no front and surround pairs: our stereo fold";
    fprintf(stderr, "[XA2] device: %d channels, mask 0x%lX: %s%s%s\n", n,
            (unsigned long)mask, why ? why : "5.1 on its speakers, surrounds on the ",
            why ? "" : backs, why ? "" : (c < 0 ? " pair, centre shared by the front pair"
                                                : " pair"));
    if (why)
        return 0;
    memset(g_xa2_matrix, 0, sizeof g_xa2_matrix);
    g_xa2_matrix[fl * 6 + 0] = 1.0f;
    g_xa2_matrix[fr * 6 + 1] = 1.0f;
    if (c >= 0) {
        g_xa2_matrix[c * 6 + 2] = 1.0f;
    } else {
        g_xa2_matrix[fl * 6 + 2] = h;
        g_xa2_matrix[fr * 6 + 2] = h;
    }
    if (lfe >= 0)
        g_xa2_matrix[lfe * 6 + 3] = 1.0f;
    g_xa2_matrix[sl * 6 + 4] = 1.0f;
    g_xa2_matrix[sr * 6 + 5] = 1.0f;
    return 1;
}

void xa2_set_channels(int channels)
{
    if (!g_xa2_initialized)
        g_xa2_want_ch = channels == 6 ? 6 : XA2_CHANNELS;
}

int xa2_source_channels(void)
{
    return g_xa2_src_ch;
}

int xa2_device_channels(void)
{
    return g_xa2_dev_ch;
}

void xa2_set_fold_lfe(float lfe)
{
    g_xa2_fold_lfe = lfe;
}

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

    if (!xa2_output_present()) {
        static int said;
        if (!said++)
            fprintf(stderr, "[XA2] no sound output device: no XAudio2 engine"
                    " until there is one\n");
        goto fail;
    }

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    /* Stereo (the default): a stereo mastering voice, as always. 5.1: the
     * device's own channels, so we can see its speakers. */
    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        g_xa2_want_ch == 6 ? XAUDIO2_DEFAULT_CHANNELS : XA2_CHANNELS,
        XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }
    g_xa2_src_ch = XA2_CHANNELS;
    g_xa2_dev_ch = 0;
    if (g_xa2_want_ch == 6 && xa2_surround_plan())
        g_xa2_src_ch = 6;
    else if (g_xa2_want_ch == 6 && g_xa2_dev_ch < 6 && xa2_spatial_on()) {
        /* A stereo device with spatial sound on: 7.1 for Windows' spatial
         * renderer (xa2_spatial_on). Back to the device's own channels if
         * that fails. */
        IXAudio2MasteringVoice_DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
        fprintf(stderr, "[XA2] spatial sound on for this output (Windows Sonic / Dolby Atmos /"
                " DTS): opened as 7.1\n");
        hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master, 8, XA2_SAMPLE_RATE, 0,
                                           NULL, NULL, 0);
        if (FAILED(hr)) {
            fprintf(stderr, "[XA2] 7.1 mastering voice failed: 0x%08lX\n", hr);
            g_xa2_master = NULL;
            hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
                XAUDIO2_DEFAULT_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
            if (FAILED(hr)) {
                fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
                goto fail;
            }
            g_xa2_dev_ch = 0;
        }
        if (xa2_surround_plan())
            g_xa2_src_ch = 6;
    }

    if (g_xa2_src_ch == 6) {
        /* FL FR C LFE + the surround pair: Windows' 5.1 (side surrounds).
         * The mask only names the channels; our matrix places them. */
        static const GUID pcm = { 0x00000001, 0x0000, 0x0010,
            { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        WAVEFORMATEXTENSIBLE x;
        memset(&x, 0, sizeof x);
        x.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        x.Format.nChannels       = 6;
        x.Format.nSamplesPerSec  = XA2_SAMPLE_RATE;
        x.Format.wBitsPerSample  = 16;
        x.Format.nBlockAlign     = 6 * 2;
        x.Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * x.Format.nBlockAlign;
        x.Format.cbSize          = sizeof x - sizeof x.Format;
        x.Samples.wValidBitsPerSample = 16;
        x.dwChannelMask          = 0x60F;
        x.SubFormat              = pcm;
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &x.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
        if (SUCCEEDED(hr)) {
            hr = IXAudio2SourceVoice_SetOutputMatrix(g_xa2_source, NULL, 6,
                (UINT32)g_xa2_dev_ch, g_xa2_matrix, XAUDIO2_COMMIT_NOW);
            if (FAILED(hr))
                fprintf(stderr, "[XA2] SetOutputMatrix failed: 0x%08lX\n", hr);
        }
    } else {
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = XA2_CHANNELS;
        wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = XA2_CHANNELS * 2;
        wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    }
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

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz %s 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, g_xa2_src_ch == 6 ? "5.1" : "stereo", g_xa2_num_bufs,
            XA2_BUF_SAMPLES);
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
    return xa2_submit_samples_ch(samples, num_samples, XA2_CHANNELS);
}

int xa2_submit_samples_ch(const int16_t *samples, int num_samples, int channels)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;
    ULONGLONG now;

    if (channels != XA2_CHANNELS && channels != 6)
        return 0;
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
    if ((int)state.BuffersQueued >= g_xa2_num_bufs) {
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
    if (channels == g_xa2_src_ch) {
        memcpy(g_xa2_bufs[idx], samples, copy_samples * channels * sizeof(int16_t));
    } else if (channels == 6) {
        /* 5.1 asked, but this device (or the one after a restart) has
         * fewer speakers: our own fold, never Windows'. */
        xa2_fold6(samples, g_xa2_bufs[idx], copy_samples, g_xa2_fold_lfe);
    } else {
        /* Stereo into a 5.1 voice: the front pair. */
        int16_t *d = g_xa2_bufs[idx];
        memset(d, 0, copy_samples * 6 * sizeof(int16_t));
        for (int i = 0; i < copy_samples; i++) {
            d[i * 6] = samples[i * 2];
            d[i * 6 + 1] = samples[i * 2 + 1];
        }
    }

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * g_xa2_src_ch * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&g_xa2_lock);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % g_xa2_num_bufs;
    g_xa2_frames_written++;
    ReleaseSRWLockExclusive(&g_xa2_lock);
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

/* Before xa2_init only: the rotation through g_xa2_bufs assumes the count
 * stays put while buffers are queued. */
void xa2_set_max_buffers(int n)
{
    if (g_xa2_initialized)
        return;
    g_xa2_num_bufs = n < 2 ? 2 : n > XA2_MAX_BUFS ? XA2_MAX_BUFS : n;
}

int xa2_max_buffers(void)
{
    return g_xa2_num_bufs;
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

int xa2_queue_probe(unsigned long long *played)
{
    XAUDIO2_VOICE_STATE state;
    int n = -1;

    *played = 0;
    AcquireSRWLockExclusive(&g_xa2_lock);
    if (g_xa2_initialized && g_xa2_source) {
        IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
        n = (int)state.BuffersQueued;
        *played = (unsigned long long)state.SamplesPlayed;
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
int  xa2_submit_samples_ch(const int16_t *s, int n, int c) { (void)s; (void)n; (void)c; return 0; }
void xa2_set_channels(int c)                          { (void)c; }
int  xa2_source_channels(void)                        { return 2; }
int  xa2_device_channels(void)                        { return 0; }
void xa2_set_fold_lfe(float l)                        { (void)l; }
int  xa2_get_buffer_size(void)                        { return 0; }
void xa2_set_max_buffers(int n)                       { (void)n; }
int  xa2_max_buffers(void)                            { return 0; }
int  xa2_queued_buffers(void)                         { return -1; }
void xa2_set_paused(int on)                           { (void)on; }

#endif /* _WIN32 */
