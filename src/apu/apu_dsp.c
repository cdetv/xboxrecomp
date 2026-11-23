/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are mixed straight down to stereo (speaker table below)
 * - GP effects processing is skipped
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
#include "apu_fx.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* g_xbox_mem_offset, through the .exe's table (apu_host.h). */
#include "apu_host.h"
extern const ApuHost *g_apu_host;

#define APU_VA         0xFE800000u

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
 * command succeeded instantly; the only work it does is the copy commands 2
 * and 3 put the effect states into X memory (dsp_command below). Anything
 * that needs DSP code to run is still missing; a native effect engine would read
 * those states instead. It exists so audio init stops blocking everything
 * behind it.
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

/* STEREO MIXDOWN OF THE 32 MIXBINS.
 *
 * On hardware the GP and EP turn the VP's mixbins into speaker output (the
 * GP runs the title's effects image: I3DL2 reverb, crosstalk; the EP
 * downmixes/encodes). Both are stubs here, so this table stands in for
 * them. The bins follow DirectSound's DSMIXBIN layout (XDK dsound.h):
 *
 *   0 FRONT_LEFT   1 FRONT_RIGHT   2 FRONT_CENTER   3 LOW_FREQUENCY
 *   4 BACK_LEFT    5 BACK_RIGHT    6-9 XTLK_FRONT_LEFT/RIGHT, BACK_LEFT/RIGHT
 *   10 I3DL2 (reverb send)        11-30 FXSEND_0..19 (effect sends)
 *
 * The previous mixdown summed even bins left and odd bins right. That put
 * the centre bin (where titles route mono dialogue) and the I3DL2 reverb
 * send wholly on the left, and LFE on the right: Conker's voices and
 * effects came out far louder on the left, and 3D voices, which DirectSound
 * also sends to LFE and I3DL2 at about their direct level, were summed two
 * or three times over (positional loops too loud).
 *
 * Here: fronts to their side, centre to both at -3 dB, backs to their side
 * at -3 dB (ITU-R BS.775 stereo downmix), crosstalk bins as their speakers.
 * LFE and I3DL2 are sends to DSP stages that do not exist (subwoofer
 * crossover, reverb); feeding them dry adds full-band copies of sounds that
 * are already in the mix, so they default to 0. FXSEND bins are effect
 * sends of unknown purpose: centred at -3 dB so nothing a title routes
 * there is lost.
 *
 *   RECOMP_APU_MIXDOWN=xemu (default) | speakers (this table) | evenodd (the
 *   old sum) | front (bins 0/1 only; RECOMP_APU_MIXDOWN_ALL=0 also selects it)
 *
 * xemu is the default because it is what players compare against: xemu with
 * use_dsp off (its default) plays the VP monitor mix -- each voice at its
 * loudest bin gain on both channels, raw HRTF on 3D voices. Measured on
 * Conker's field and Birdy scenes against a recording of xemu: within 0.5 dB
 * on median, p90, p99 and peak. The speaker table put 3D voices at full level
 * in the near ear: Birdy's creaks 4-6 dB louder than xemu, ambient loops
 * 1.5-3 dB, while xemu's music (four channel voices summed) is louder than
 * this table makes it. The table stays for a real-DSP-like stereo image.
 *   RECOMP_APU_MIX_LFE=<gain>, RECOMP_APU_MIX_I3DL2=<gain>: feed those bins
 *   to both sides at that gain (default 0). */
static float s_mix_l[NUM_MIXBINS], s_mix_r[NUM_MIXBINS];
static int s_mix_ready, s_mix_xemu;

static float env_gain(const char *name, float def)
{
    const char *e = getenv(name);
    return (e && *e) ? (float)atof(e) : def;
}

static void mixdown_init(void)
{
    const float h = 0.70710678f;
    const char *mode = getenv("RECOMP_APU_MIXDOWN");
    const char *all = getenv("RECOMP_APU_MIXDOWN_ALL");
    const char *name = "speakers";

    memset(s_mix_l, 0, sizeof(s_mix_l));
    memset(s_mix_r, 0, sizeof(s_mix_r));
    if ((mode && !strcmp(mode, "front")) || (all && *all && !atoi(all))) {
        name = "front";
        s_mix_l[0] = 1.0f;
        s_mix_r[1] = 1.0f;
    } else if (!mode || !*mode || !strcmp(mode, "xemu")) {
        /* The VP monitor mix (apu_vp.c) is the output; this table is unused. */
        name = "xemu (VP monitor mix, as xemu with use_dsp off)";
        s_mix_xemu = 1;
    } else if (mode && !strcmp(mode, "evenodd")) {
        name = "evenodd";
        for (int b = 0; b < NUM_MIXBINS; b++) {
            if (b & 1) s_mix_r[b] = 1.0f;
            else       s_mix_l[b] = 1.0f;
        }
    } else {
        float lfe = env_gain("RECOMP_APU_MIX_LFE", 0.0f);
        float rev = env_gain("RECOMP_APU_MIX_I3DL2", 0.0f);
        s_mix_l[0] = 1.0f;               s_mix_r[1] = 1.0f;
        s_mix_l[2] = h;                  s_mix_r[2] = h;
        s_mix_l[3] = lfe;                s_mix_r[3] = lfe;
        s_mix_l[4] = h;                  s_mix_r[5] = h;
        s_mix_l[6] = 1.0f;               s_mix_r[7] = 1.0f;
        s_mix_l[8] = h;                  s_mix_r[9] = h;
        s_mix_l[10] = rev;               s_mix_r[10] = rev;
        for (int b = 11; b <= 30; b++) {
            s_mix_l[b] = h;
            s_mix_r[b] = h;
        }
    }
    fprintf(stderr, "[APU] stereo mixdown: %s (LFE %.2f, I3DL2 %.2f)\n", name,
            s_mix_l[3], s_mix_l[10]);
    s_mix_ready = 1;
}

/* ── The doorbell's copy commands ─────────────────────────────────────────
 *
 * The doorbell is the command word at +0x810 of DirectSound's shared copy of
 * the DSP image (XDK 5849: dsp_download_image writes the image header words
 * 0x800-0x814 and the image from +0x818 into it; Conker's copy is at
 * 0x82AC8000). The words before it are the command's block:
 *
 *   +0x800  X destination, in words from X:0x80 (where the effect states start)
 *   +0x804  the image header's code size in words (the state area starts at
 *           image +0x818 + 4 x this)
 *   +0x808  source: byte offset in the shared copy
 *   +0x80C  word count
 *   +0x810  command: 3 = load and start (at boot: the whole state area),
 *                    2 = copy (CommitDeferredSettings: an effect's changed
 *                        words, e.g. the I3DL2 reverb's converted listener)
 *
 * On the console the GP's loader copies those words into its X memory, where
 * the effects read their state. Acknowledging without copying left X memory
 * empty: only SetEffectData's immediate writes ever landed there, never the
 * load or a deferred change. Doing the copy puts in X memory what the
 * console's GP has, so native effects can read their state where the
 * DSP code reads it. Code (P) and Y are not copied: no DSP code runs.
 *
 *   RECOMP_APU_DSP_COPY=0   acknowledge only (the old stub, for A/B)
 *   RECOMP_APU_DSP_LOG=1    [APUDSP] per command (first 20, then every 100th)
 *                           and the effect list at command 3; =2 adds the
 *                           words each logged command copied */
#define DSP_CMD_BLOCK  0x800u             /* doorbell - 0x10 */
#define DSP_STATE_X    0x80u              /* +0x800 counts from here */
#define DSP_XMEM_WORDS (NV_PAPU_GPMIXBUF / 4u)   /* GP X memory: 0x1400 words */
#define DSP_SHARED_MAX 0x20000u           /* sanity bound on +0x808 / +0x80C */

static int s_dsp_copy = -1, s_dsp_log = -1;

static uint32_t rd32(const uint8_t *p)
{
    return *(const volatile uint32_t *)p;
}

static void dsp_log_fx_table(const uint8_t *shared, uint32_t code_words,
                             uint32_t state_words)
{
    /* The effect descriptors follow the state area: a count, a word, then
     * {code, bytes, state, bytes, Y, bytes, scratch, bytes} per effect
     * (_work/r3/sound/dspstate.py reads the same table in the image file). */
    uint32_t t = 0x818u + (code_words + state_words) * 4u;
    uint32_t n;

    if (t + 8u > DSP_SHARED_MAX)
        return;
    n = rd32(shared + t);
    fprintf(stderr, "[APUDSP] effect list at +0x%X: %u effect(s)\n", t, n);
    if (n > 64 || t + 8u + n * 32u > DSP_SHARED_MAX)
        return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = shared + t + 8u + i * 32u;
        uint32_t st = rd32(e + 8);
        char where[24] = "";
        if (st >= APU_VA + 0x30000u && st < APU_VA + 0x30000u + DSP_XMEM_WORDS * 4u)
            snprintf(where, sizeof where, " = X:0x%X", (st - APU_VA - 0x30000u) / 4u);
        fprintf(stderr, "[APUDSP] effect %2u: code 0x%08X %5u B, state 0x%08X%s"
                        " %5u B, Y 0x%08X %u B, scratch 0x%08X %u B\n",
                i, rd32(e), rd32(e + 4), st, where, rd32(e + 12), rd32(e + 16),
                rd32(e + 20), rd32(e + 24), rd32(e + 28));
    }
}

static void dsp_command(MCPXAPUState *d, uint32_t doorbell, uint32_t cmd)
{
    static unsigned s_count;
    const uint8_t *shared = d->ram_ptr + (doorbell - (DSP_CMD_BLOCK + 0x10u));
    const uint8_t *blk = shared + DSP_CMD_BLOCK;
    uint32_t xdst = rd32(blk), code_words = rd32(blk + 4);
    uint32_t src = rd32(blk + 8), n = rd32(blk + 12);
    uint32_t x = DSP_STATE_X + xdst;
    int show;

    if (s_dsp_copy < 0) {
        const char *e = getenv("RECOMP_APU_DSP_COPY");
        const char *l = getenv("RECOMP_APU_DSP_LOG");
        s_dsp_copy = !(e && *e == '0');
        s_dsp_log = (l && *l) ? atoi(l) : 0;
        fprintf(stderr, "[APUDSP] doorbell copies %s\n",
                s_dsp_copy ? "on (X memory gets the effect states)"
                           : "off (RECOMP_APU_DSP_COPY=0: acknowledged only)");
    }
    if (!s_dsp_copy || (cmd != 2 && cmd != 3))
        return;
    s_count++;
    show = s_dsp_log && (s_count <= 20 || s_count % 100 == 0);

    if (n == 0 || n > DSP_XMEM_WORDS || xdst >= DSP_XMEM_WORDS || src & 3u ||
        src < 0x818u || src > DSP_SHARED_MAX || src + n * 4u > DSP_SHARED_MAX ||
        x + n > DSP_XMEM_WORDS) {
        static int refused;
        if (refused++ < 10)
            fprintf(stderr, "[APUDSP] command %u refused: %u words +0x%X -> X:0x%X"
                            " (out of range; acknowledged only)\n", cmd, n, src, x);
        return;
    }

    volatile uint32_t *xmem = (volatile uint32_t *)
        ((uintptr_t)*g_apu_host->mem_offset + APU_VA + 0x30000u + NV_PAPU_GPXMEM);
    const uint8_t *from = shared + src;
    for (uint32_t i = 0; i < n; i++)
        xmem[x + i] = rd32(from + i * 4u) & 0x00FFFFFFu;

    if (show) {
        fprintf(stderr, "[APUDSP] command %u #%u: %u words +0x%X -> X:0x%X\n",
                cmd, s_count, n, src, x);
        if (s_dsp_log >= 2)
            for (uint32_t i = 0; i < n; i += 8) {
                char line[128];
                int len = snprintf(line, sizeof line, "[APUDSP]   X:0x%04X", x + i);
                for (uint32_t k = i; k < n && k < i + 8 && len < (int)sizeof line - 8; k++)
                    len += snprintf(line + len, sizeof line - len, " %06X", xmem[x + k]);
                fprintf(stderr, "%s\n", line);
            }
    }
    if (cmd == 3) {
        if (s_dsp_log)
            dsp_log_fx_table(shared, code_words, n);
        /* The effect list follows the state area (dsp_log_fx_table). */
        apu_fx_load(shared, 0x818u + (code_words + n) * 4u,
                    (volatile uint32_t *)((uintptr_t)*g_apu_host->mem_offset +
                                          APU_VA + 0x30000u + NV_PAPU_GPXMEM));
    }
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        volatile uint32_t *slot = (volatile uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        uint32_t cmd = *slot;
        if (cmd) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], cmd);
            /* Copy before clearing: DirectSound spins until the word is 0,
             * so it never changes the block while the copy runs. */
            dsp_command(d, s_dsp_ack[i], cmd);
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

static void dsp_bootstrap(MCPXAPUState *d, uint32_t *regs, uint32_t window,
                          uint32_t pmem, hwaddr sge_base, uint32_t max_sge,
                          const char *name)
{
    static int reported[2];
    int which = (regs == d->ep.regs);
    volatile uint32_t *p = (volatile uint32_t *)
        ((uintptr_t)*g_apu_host->mem_offset + APU_VA + window + pmem);

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
     * GP/EP should run in realtime or cached mode; with use_dsp off (its
     * default) it selects the VP monitor mix. RECOMP_APU_MIXDOWN=xemu does
     * the same here, to hear and measure what xemu players hear. */
    if (!s_mix_ready)
        mixdown_init();
    /* Standard or Full DSP (apu_fx.c, switched live): the monitor mix rests
     * while only Full DSP is heard, and runs again for a crossfade back. */
    apu_fx_frame_begin();
    if (s_mix_xemu)
        d->monitor.point = apu_fx_full_only() ? MCPX_APU_DEBUG_MON_AC97
                                              : MCPX_APU_DEBUG_MON_VP;
}

/* RECOMP_APU_MIXDOWN's table (mixdown_init) at sample i, unclamped. */
static void table_mix(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], int i,
                      float *l, float *r)
{
    float left = 0.0f, right = 0.0f;
    for (int b = 0; b < NUM_MIXBINS; ++b) {
        left  += s_mix_l[b] * mixbins[b][i];
        right += s_mix_r[b] * mixbins[b][i];
    }
    *l = left;
    *r = right;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: mix the bins down to stereo (see mixdown_init) and write
     * the result to the monitor frame buffer as the final EP output. */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (g_apu_prof_on) {
        for (int b = 0; b < NUM_MIXBINS; ++b) {
            float peak = 0.0f;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float a = fabsf(mixbins[b][i]);
                if (a > peak) peak = a;
            }
            if (peak > 0.0f) {
                g_apu_prof.bin_frames[b]++;
                if (peak > g_apu_prof.bin_peak[b]) g_apu_prof.bin_peak[b] = peak;
            }
        }
    }

    /* The effects image's chain (apu_fx.c): in Full DSP its speaker bins
     * replace the monitor mix's slice, folded to stereo (L = FL + C/sqrt2 +
     * BL/sqrt2, R likewise; LFE left out unless RECOMP_APU_FOLD_LFE gives a
     * gain) at RECOMP_APU_DSP_GAIN_DB (default -8.5, Standard's level in
     * play). While the processing switches, the two are crossfaded:
     * Standard's slice is the monitor mix the VP left there (or the table's
     * mix). With only RECOMP_APU_FX_LOG it runs for its log and plays
     * nothing. */
    int full = 0, room = 0;
    float full_l[NUM_SAMPLES_PER_FRAME], full_r[NUM_SAMPLES_PER_FRAME];
    /* Room reverb in Standard (xbox_ApuSetRoomReverb, Conker to-do 232): the
     * reverb's wet output folded as Full DSP folds its speakers, at Full
     * DSP's output gain (Standard's level in play), added to Standard's
     * slice. Nothing while no room reverb is set. */
    float room_l[NUM_SAMPLES_PER_FRAME], room_r[NUM_SAMPLES_PER_FRAME];
    if (apu_fx_wanted()) {
        static float gain = -1.0f, lfe, room_gain = -1.0f;
        float spk[APU_FX_SPEAKERS][APU_FX_FRAME], wet[4][APU_FX_FRAME];
        apu_fx_run(mixbins, spk);
        if (apu_fx_room_wet(wet)) {
            const float h = 0.70710678f;
            if (room_gain < 0.0f) {
                const char *e = getenv("RECOMP_APU_DSP_GAIN_DB");
                room_gain = powf(10.0f, ((e && *e) ? (float)atof(e) : -8.5f) / 20.0f);
            }
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                room_l[i] = (wet[0][i] + h * wet[2][i]) * room_gain;
                room_r[i] = (wet[1][i] + h * wet[3][i]) * room_gain;
            }
            room = 1;
        }
        if (apu_fx_full() && apu_out_channels() == 6) {
            /* 5.1 (to-do 50 step 4): the six speakers as they are, at Full
             * DSP's gain. While Full DSP fades in (or before the effects
             * image is loaded) Standard's stereo plays on the front pair:
             * frame_buf's slice, which apu_core.c adds there. */
            const int mon = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
            if (gain < 0.0f) {
                const char *e = getenv("RECOMP_APU_DSP_GAIN_DB");
                gain = powf(10.0f, ((e && *e) ? (float)atof(e) : -8.5f) / 20.0f);
                lfe = env_gain("RECOMP_APU_FOLD_LFE", 0.0f);
                fprintf(stderr, "[APU] Full DSP: 5.1, six channels, output gain %.2f\n", gain);
            }
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float w = apu_fx_share(i), o[APU_FX_SPEAKERS], sl = 0.0f, sr = 0.0f;
                for (int c = 0; c < APU_FX_SPEAKERS; c++)
                    o[c] = spk[c][i] * gain * w;
                if (w < 1.0f && !mon) {
                    table_mix(mixbins, i, &sl, &sr);
                    sl = sl > 1.0f ? 1.0f : sl < -1.0f ? -1.0f : sl;
                    sr = sr > 1.0f ? 1.0f : sr < -1.0f ? -1.0f : sr;
                } else if (w < 1.0f) {
                    sl = d->monitor.frame_buf[off + i][0] / 32767.0f;
                    sr = d->monitor.frame_buf[off + i][1] / 32767.0f;
                }
                if (w < 1.0f && room) {
                    sl += room_l[i];
                    sr += room_r[i];
                }
                o[0] += sl * (1.0f - w);
                o[1] += sr * (1.0f - w);
                for (int c = 0; c < APU_FX_SPEAKERS; c++) {
                    float x = o[c] > 1.0f ? 1.0f : o[c] < -1.0f ? -1.0f : o[c];
                    d->monitor.frame_buf6[off + i][c] = (int16_t)(x * 32767.0f);
                }
                /* Taken into the front pair above: not twice. */
                d->monitor.frame_buf[off + i][0] = 0;
                d->monitor.frame_buf[off + i][1] = 0;
            }
            g_dbg.gp.cycles = 0;
            g_dbg.ep.cycles = 0;
            return;
        }
        if (apu_fx_full()) {
            const float h = 0.70710678f;
            /* -8.5 dB (to-do 50 step 3): Full DSP as loud as Standard in
             * play. At 0 dB it matched a recording of xemu with "Use DSP"
             * (Conker's opening movie, field, Birdy) within 0.3 dB in every
             * band, so the chain keeps the console's balance: its movie is
             * 3 dB quieter against play than Standard's. LFE out matched the
             * recording too (the field sends a lot to it). */
            if (gain < 0.0f) {
                const char *e = getenv("RECOMP_APU_DSP_GAIN_DB");
                gain = powf(10.0f, ((e && *e) ? (float)atof(e) : -8.5f) / 20.0f);
                lfe = env_gain("RECOMP_APU_FOLD_LFE", 0.0f);
                fprintf(stderr, "[APU] Full DSP: stereo fold, output gain %.2f, LFE %.2f\n",
                        gain, lfe);
            }
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                float c = h * spk[2][i] + lfe * spk[3][i];
                full_l[i] = (spk[0][i] + c + h * spk[4][i]) * gain;
                full_r[i] = (spk[1][i] + c + h * spk[5][i]) * gain;
            }
            full = 1;
        }
    }
    if (full) {
        const int mon = d->monitor.point == MCPX_APU_DEBUG_MON_VP;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float w = apu_fx_share(i), l = full_l[i], r = full_r[i];
            if (w < 1.0f) {
                float sl, sr;
                if (mon) {
                    sl = d->monitor.frame_buf[off + i][0] / 32767.0f;
                    sr = d->monitor.frame_buf[off + i][1] / 32767.0f;
                } else {
                    table_mix(mixbins, i, &sl, &sr);
                    sl = sl > 1.0f ? 1.0f : sl < -1.0f ? -1.0f : sl;
                    sr = sr > 1.0f ? 1.0f : sr < -1.0f ? -1.0f : sr;
                }
                if (room) {
                    sl += room_l[i];
                    sr += room_r[i];
                }
                l = sl + (l - sl) * w;
                r = sr + (r - sr) * w;
            }
            l = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
            r = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
            d->monitor.frame_buf[off + i][0] = (int16_t)(l * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(r * 32767.0f);
        }
    } else if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
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
             * Bins 6/7 and 8/9 arrive with matched counts: the guest's own
             * stereo pairing, kept by the speaker table above. */
            /* (The even-left/odd-right sum that followed was the cheapest
             * mixdown that stopped discarding audio; the table above
             * replaces it.) */
            float left, right;
            if (!s_mix_ready)
                mixdown_init();
            table_mix(mixbins, i, &left, &right);
            if (g_apu_prof_on) {
                float al = fabsf(left), ar = fabsf(right);
                float m = al > ar ? al : ar;
                g_apu_prof.mix_l2 += (double)left * left;
                g_apu_prof.mix_r2 += (double)right * right;
                g_apu_prof.mix_samples++;
                if (m > 1.0f) g_apu_prof.mix_clipped++;
                if (m > g_apu_prof.mix_peak) g_apu_prof.mix_peak = m;
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
    if (room && !full) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            for (int c = 0; c < 2; c++) {
                float x = d->monitor.frame_buf[off + i][c]
                        + (c ? room_r[i] : room_l[i]) * 32767.0f;
                x = x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x;
                d->monitor.frame_buf[off + i][c] = (int16_t)lrintf(x);
            }
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
