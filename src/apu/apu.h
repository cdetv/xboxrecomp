/*
 * MCPX APU Audio Emulation - Public API
 *
 * Extracted from xemu (LGPL v2+), adapted for standalone use.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MCPXAPUState MCPXAPUState;

/* Initialize the APU emulation.
 * ram_ptr: pointer to the base of Xbox physical RAM (64MB).
 * Returns the APU state, or NULL on failure. */
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);

/* Shut down and free the APU state. */
void mcpx_apu_shutdown(MCPXAPUState *d);

/* MMIO read from APU register space (addr is offset from 0xFE800000). */
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);

/* MMIO write to APU register space (addr is offset from 0xFE800000). */
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

/* Play a 440Hz test tone through the APU pipeline to verify audio output.
 * Directly programs a voice without going through DirectSound. */
void mcpx_apu_play_test_tone(MCPXAPUState *d);

/* Hold the APU while a host pause holds the title (a host menu's pause
 * key): 1 = the frame thread stops advancing voices and the output stops
 * where it is; 0 = both carry on from there. Any thread. */
void xbox_ApuSetPaused(int on);

/* A gain on everything the APU outputs, for a host volume setting: 1 =
 * untouched (default), 0 = silent, above 1 louder with a soft limiter (at
 * most 8, +18 dB). Changes glide over ~15 ms. Any thread. */
void xbox_ApuSetOutputGain(float gain);

/* The title's I3DL2 listener (a DSI3DL2LISTENER, 48 bytes) for the effects
 * image's reverb (apu_fx.c, Full DSP): the runtime can't find it on its own
 * (it sits in a DirectSound object), so the game's glue hands it over when
 * it changes. Any thread. */
void xbox_ApuSetI3dl2Listener(const void *listener48);

/* How the APU's output is made (any thread; applies at the frame thread's
 * next frame, crossfaded over ~21 ms, so a switch never clicks):
 *   processing: APU_PROCESSING_STANDARD = the VP monitor mix (xemu with
 *     use_dsp off; or RECOMP_APU_MIXDOWN's table), APU_PROCESSING_FULL = the
 *     effects image's chain (apu_fx.c) and its speaker bins, folded to the
 *     output;
 *   output: APU_OUTPUT_STEREO (speakers: Full DSP's crosstalk as the game
 *     sets it), APU_OUTPUT_HEADPHONES (Full DSP without the crosstalk's cross
 *     term: the HRTF ears straight to their sides; Standard has no crosstalk,
 *     so it plays as Stereo), APU_OUTPUT_SURROUND (5.1: only from the start,
 *     RECOMP_APU_OUTPUT=surround before the APU and the kernel's XC_AUDIO
 *     read it; always Full DSP, six channels where the device has six
 *     speakers, else Full DSP's stereo fold; asked later it waits for the
 *     next start, and a 5.1 run ignores Stereo / Headphones until then).
 * Until the first call RECOMP_APU_PROCESSING (standard | full) and
 * RECOMP_APU_OUTPUT (stereo | headphones | surround) decide. */
#define APU_PROCESSING_STANDARD 0
#define APU_PROCESSING_FULL     1
#define APU_OUTPUT_STEREO       0
#define APU_OUTPUT_HEADPHONES   1
#define APU_OUTPUT_SURROUND     2
void xbox_ApuSetAudioMode(int processing, int output);

/* ============================================================
 * Software mixer - DirectSound buffer bridge
 *
 * DirectSound buffers register here. The APU frame thread
 * mixes active buffers directly into the waveOut output,
 * bypassing the VP hardware voice pipeline.
 * ============================================================ */

#define APU_MIXER_MAX_VOICES 64

typedef struct APUMixerVoice {
    volatile int     active;       /* 1 = playing, 0 = stopped */
    volatile int     looping;      /* 1 = loop, 0 = one-shot */
    const int16_t   *pcm_data;     /* Pointer to 16-bit PCM samples */
    uint32_t         pcm_bytes;    /* Total buffer size in bytes */
    uint32_t         num_channels;  /* 1=mono, 2=stereo */
    uint32_t         sample_rate;   /* e.g. 22050, 44100, 48000 */
    uint64_t         play_offset; /* Source frame position, 16 fractional bits */
    float            volume;       /* 0.0 to 1.0 */
} APUMixerVoice;

/* Allocate a mixer voice slot. Returns slot index or -1 if full. */
int apu_mixer_alloc_voice(void);

/* Free a mixer voice slot. */
void apu_mixer_free_voice(int slot);

/* Get pointer to a mixer voice for configuration. */
APUMixerVoice *apu_mixer_get_voice(int slot);

/* Seek in PCM bytes (rounded down to a whole frame). Returns 0 if invalid. */
int apu_mixer_set_position(int slot, uint32_t byte_offset);

/* Snapshot playback state under the mixer lock; output pointers may be NULL. */
void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping);

/* Start/resume or stop playback without changing the current position. */
void apu_mixer_play(int slot, int looping);
void apu_mixer_stop(int slot);

#ifdef __cplusplus
}
#endif
