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

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);
/* How many buffers may be queued at most (3 by default; RECOMP_APU_PACE
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
