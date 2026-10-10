/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

/* ── GP / EP memory and the boot ROM ────────────────────────────────────
 *
 * The GP (APU +0x30000) and EP (+0x50000) regions used to be ignored: reads
 * returned 0 and writes vanished. DirectSound reads one of them back.
 * CMcpxAPU::ServiceDeferredCommandsLow, run from every DirectSoundDoWork,
 * checks that EP program memory word 6 (APU +0x5A018) holds 0xCCCCCC and
 * otherwise resets both DSPs: XCNTMODE off (SECTL), GPRST/EPRST 1 then 3, a
 * 10 ms KeStallExecutionProcessor, XCNTMODE back on. Reading 0, it reset them
 * on every call, so the sample counter was off ~10 ms of every ~16 ms and
 * the frame thread skipped about two se_frames in three -- silence between
 * the slices that did play (Conker: Live & Reloaded, crackling audio).
 *
 * The X/Y/P memories stay plain guest memory (xbox_memory_layout.c leaves
 * them untrapped: Burnout 3 bulk-copies them with rep movsd). Only each
 * window's last page, the control registers (GPRST/EPRST at +0xFFFC), is
 * trapped and lands here, stored in gp.regs / ep.regs by byte offset like
 * xemu. A DSPRST 0->1 edge on GPRST/EPRST does what xemu's dsp_bootstrap
 * does: the boot ROM copies the first 0x800 words of the DSP's scratch
 * memory (the GPSADDR/EPSADDR page list) into program memory. No DSP code
 * runs; this only puts what the guest loaded where it reads it back. */
#define DSP_BOOT_WORDS 0x800
#define APU_VA         0xFE800000u

extern ptrdiff_t g_xbox_mem_offset;

static void dsp_bootstrap(MCPXAPUState *d, uint32_t *regs, uint32_t window,
                          uint32_t pmem, hwaddr sge_base, uint32_t max_sge,
                          const char *name)
{
    static int reported[2];
    int which = (regs == d->ep.regs);
    volatile uint32_t *p = (volatile uint32_t *)
        ((uintptr_t)g_xbox_mem_offset + APU_VA + window + pmem);

    if (!sge_base) {
        if (!reported[which]++)
            fprintf(stderr, "[APU] %s boot: no scratch page list yet\n", name);
        return;
    }
    for (uint32_t i = 0; i < DSP_BOOT_WORDS; i++) {
        uint32_t byte = i * 4;
        uint32_t entry = byte / TARGET_PAGE_SIZE;
        if (entry > max_sge)
            break;
        uint32_t page = ldl_le_phys(address_space_memory, sge_base + entry * 8);
        p[i] = ldl_le_phys(address_space_memory, page + byte % TARGET_PAGE_SIZE)
               & 0x00FFFFFF;
    }
    reported[which]++;
    if (reported[which] <= 3 || reported[which] % 1000 == 0)
        fprintf(stderr, "[APU] %s boot #%d: scratch SGE 0x%08X (max %u) -> P:0..7FF,"
                " P:6=%06X\n", name, reported[which], (uint32_t)sge_base, max_sge,
                p[6]);
}

static void dsp_rst_write(MCPXAPUState *d, uint32_t *regs, uint32_t rst,
                          uint32_t val, uint32_t window, uint32_t pmem,
                          hwaddr sge_base, uint32_t max_sge, const char *name)
{
    /* xemu gp_write/ep_write: RST or DSPRST low holds the core in reset; a
     * DSPRST rising edge with RST high runs the boot ROM. */
    if ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST) &&
        !(regs[rst] & NV_PAPU_GPRST_GPDSPRST))
        dsp_bootstrap(d, regs, window, pmem, sge_base, max_sge, name);
    regs[rst] = val;
}

uint32_t mcpx_apu_gp_ep_read(MCPXAPUState *d, hwaddr addr)
{
    if (addr >= 0x30000 && addr < 0x40000)
        return d->gp.regs[(addr - 0x30000) & ~3u];
    if (addr >= 0x50000 && addr < 0x60000)
        return d->ep.regs[(addr - 0x50000) & ~3u];
    return 0;
}

void mcpx_apu_gp_ep_write(MCPXAPUState *d, hwaddr addr, uint32_t val)
{
    if (addr >= 0x30000 && addr < 0x40000) {
        uint32_t off = (uint32_t)(addr - 0x30000) & ~3u;
        if (off == NV_PAPU_GPRST)
            dsp_rst_write(d, d->gp.regs, off, val, 0x30000, NV_PAPU_GPPMEM,
                          d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE], "GP");
        else
            d->gp.regs[off] = val;
    } else if (addr >= 0x50000 && addr < 0x60000) {
        uint32_t off = (uint32_t)(addr - 0x50000) & ~3u;
        if (off == NV_PAPU_EPRST)
            dsp_rst_write(d, d->ep.regs, off, val, 0x50000, NV_PAPU_EPPMEM,
                          d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE], "EP");
        else
            d->ep.regs[off] = val;
    }
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
