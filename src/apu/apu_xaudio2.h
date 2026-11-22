/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* The same for 2 or 6 channels (6: FL FR C LFE BL BR, WAVE order). The
 * source voice takes them as they are when it has that many channels;
 * otherwise 6 are folded to stereo here (xa2_fold6) and 2 go to the front
 * pair of a 6-channel source. */
int xa2_submit_samples_ch(const int16_t *samples, int num_samples, int channels);

/* 5.1 output (to-do 50 step 4). Before xa2_init: 6 = open the mastering
 * voice with the device's own channels and, when the device has six or more
 * speakers (5.1, 7.1, or Windows Sonic / Dolby Atmos for Headphones, which
 * present 7.1), a 6-channel source voice placed on them by our own output
 * matrix (never XAudio2's default one); 2 (default) = today's stereo voices
 * on a stereo mastering voice, unchanged. Kept for restarts. */
void xa2_set_channels(int channels);
/* The source voice's channels (2 or 6) and the device's (0 when the
 * mastering voice was opened stereo, so the device was not asked). */
int xa2_source_channels(void);
int xa2_device_channels(void);
/* RECOMP_APU_FOLD_LFE's gain for xa2_fold6 when a 6-channel output meets a
 * stereo source voice (default 0: LFE left out, as Full DSP's fold). */
void xa2_set_fold_lfe(float lfe);

/* The stereo fold of six channels (FL FR C LFE BL BR): L = FL + C/sqrt2 +
 * BL/sqrt2 + lfe x LFE, R likewise, clamped. Full DSP's stereo fold
 * (apu_dsp.c) does the same before its clamp. */
static inline void xa2_fold6(const int16_t *in6, int16_t *out2, int n, float lfe)
{
    const float h = 0.70710678f;
    for (int i = 0; i < n; i++, in6 += 6, out2 += 2) {
        float c = h * in6[2] + lfe * in6[3];
        float l = in6[0] + c + h * in6[4];
        float r = in6[1] + c + h * in6[5];
        out2[0] = (int16_t)(l > 32767.0f ? 32767.0f : l < -32768.0f ? -32768.0f : l);
        out2[1] = (int16_t)(r > 32767.0f ? 32767.0f : r < -32768.0f ? -32768.0f : r);
    }
}

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);
/* How many buffers may be queued at most (3 unless set; output pacing
 * asks for 6). Set before xa2_init; ignored once it is up. */
void xa2_set_max_buffers(int n);
int xa2_max_buffers(void);
/* Buffers XAudio2 still has queued (0 = it ran dry), or -1 if inactive. */
int xa2_queued_buffers(void);
/* The same, with the samples the voice has played since it started
 * (RECOMP_APU_PROFILE; a restart begins them again from 0). */
int xa2_queue_probe(unsigned long long *played);

/* Stop the output where it is (1), keeping what is queued, or start it
 * again from there (0). */
void xa2_set_paused(int on);

#endif /* APU_XAUDIO2_H */
