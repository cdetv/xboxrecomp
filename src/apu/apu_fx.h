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

/* 1 when the frame thread should run the chain: an image is loaded and
 * Full DSP is on (RECOMP_APU_PROCESSING=full) or RECOMP_APU_FX_LOG is set. */
int apu_fx_wanted(void);

/* 1 when the chain's speakers replace the monitor mix (RECOMP_APU_PROCESSING=full). */
int apu_fx_full(void);

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
