/*
 * Native GP effects: the title's DSP effects image run on the host instead of
 * a DSP56300 core (apu_fx.c says how). MIT; see apu_fx.c for the credit.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_FX_BINS    32   /* mix bins, DSMIXBIN 0..31 */
#define APU_FX_FRAME   32   /* samples per GP frame */
#define APU_FX_SPEAKERS 6   /* bins 0-5: FL FR C LFE BL BR */

/* At the doorbell's command 3 (load and start), on the APU frame thread:
 * shared = DirectSound's shared copy of the image (host pointer to its
 * byte 0), table = the effect list's byte offset in it, xmem = GP X memory
 * (host pointer to X:0). Reads each effect's kind from its (plain) code and
 * resets every effect's history. */
void apu_fx_load(const uint8_t *shared, uint32_t table, volatile uint32_t *xmem);

/* Processing as xbox_ApuSetAudioMode asks it (apu.h; any thread). */
void apu_fx_set_mode(int processing, int output);

/* Frame thread, at the start of every frame (before the VP): takes a new
 * processing mode and moves the crossfade on. The calls below describe the
 * frame it began. */
void apu_fx_frame_begin(void);

/* 1 when the frame thread should run the chain: an image is loaded and
 * Full DSP is heard (or fading) or RECOMP_APU_FX_LOG is set. */
int apu_fx_wanted(void);

/* 1 when Full DSP's output is heard in this frame (on, or crossfading). */
int apu_fx_full(void);

/* 1 when only Full DSP is heard (no crossfade): the monitor mix can rest. */
int apu_fx_full_only(void);

/* Full DSP's share of the output at sample i of this frame (0..1). */
float apu_fx_share(int i);

/* 1 when this run plays 5.1 surround (RECOMP_APU_OUTPUT=surround at the
 * start): Full DSP always, the chain's six speakers to the output stage. */
int apu_fx_surround(void);

/* 1 = Full DSP's 3D voices use the raw HRTF ears, as the console (default);
 * 0 = the power-normalised ones (RECOMP_APU_DSP_HRTF_NORM=1, A/B). */
int apu_fx_raw_ears(void);

/* One GP frame on the frame thread: bins = the voice processor's 32 bins,
 * out = speaker bins 0-5 after every effect. */
void apu_fx_run(float bins[APU_FX_BINS][APU_FX_FRAME],
                float out[APU_FX_SPEAKERS][APU_FX_FRAME]);

/* The I3DL2 listener (a DSI3DL2LISTENER, 48 bytes: lRoom, lRoomHF,
 * flRoomRolloffFactor, flDecayTime, flDecayHFRatio, lReflections,
 * flReflectionsDelay, lReverb, flReverbDelay, flDiffusion, flDensity,
 * flHFReference) for the reverb. Any thread; the frame thread takes it at
 * its next frame and fades over to it. */
void apu_fx_set_listener(const void *listener48);

#ifdef __cplusplus
}
#endif
