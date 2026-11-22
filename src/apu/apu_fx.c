/*
 * Native GP effects: the title's DSP effects image run on the host.
 *
 * On the console the GP DSP runs the effects image on every 32-sample frame:
 * each effect reads its input buffers from X memory and writes its outputs
 * there, and the mix bins are X memory too (bin b: 0x1400 + 0x20 b to read,
 * 0xC00 + 0x20 b to write, the same storage). No DSP56300 core runs here: the
 * effects XDK 5849 images use are recognised by their code and run as C
 * (gain, mixer, filter, crosstalk, I3DL2 reverb); others (the sample rate
 * converters that feed DMA outputs) are left out and logged.
 *
 * Each effect reads its state words (buffer addresses, gains, coefficients)
 * from GP X memory every frame, where the doorbell's copies (apu_dsp.c) and
 * SetEffectData's immediate writes put them, as the DSP code does. The
 * reverb's parameters come from the I3DL2 listener (apu_fx_set_listener),
 * converted the library's own way.
 *
 * Processing (Standard = the monitor mix, Full DSP = this chain's speakers)
 * is switched live by xbox_ApuSetAudioMode, crossfaded (apu_fx_frame_begin).
 * So is the output's Stereo / Headphones (the crosstalk's cross term faded
 * out: the HRTF ears straight to their sides); 5.1 surround is fixed at the
 * start and always plays Full DSP.
 *
 * Switches (read once):
 *   RECOMP_APU_PROCESSING=full   Full DSP until the first xbox_ApuSetAudioMode
 *   RECOMP_APU_OUTPUT=stereo|headphones|surround
 *                                the output until the first xbox_ApuSetAudioMode;
 *                                surround only from the start (apu_core.c makes
 *                                six channels, the kernel tells the game)
 *   RECOMP_APU_DSP_HRTF_NORM=1   Full DSP with the power-normalised HRTF ears
 *                                (A/B; default the raw ears, as the console)
 *   RECOMP_APU_FX_LOG=1          [APUFX] at the load (each effect and its kind)
 *                                and every 5 s of audio (time, reverb, peaks);
 *                                runs the chain even without Full DSP (its
 *                                output then goes nowhere); 2 adds each
 *                                effect's state words at the load
 *   RECOMP_APU_FX_OFF=<kind>[,..] gain, mixer, filter, reverb: not run;
 *                                crosstalk: run without its cross term
 *
 * Based on yacineolivier-maker/ConkerLR-Recomp sdk/runtime/xdk/dsp_image.cpp
 * (commit 2013160), ported to C. Left out: the image cipher (DirectSound has
 * already decrypted the code in its shared copy) and the state kept in guest
 * memory (read from GP X memory here).
 *
 * MIT License
 *
 * Copyright (c) 2026 yacineolivier-maker and the ConkerLR-Recomp contributors
 * Copyright (c) 2026 the conker-lr-recomp contributors (the C port)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "apu_fx.h"
#include "apu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#define FX_BARRIER() _ReadWriteBarrier()
#else
#include <time.h>
#define FX_BARRIER() __sync_synchronize()
#endif

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define GP_X_CPU     0xFE830000u   /* GP X memory in the CPU's APU window */
#define GP_X_WORDS   0x1400u       /* X memory below the mix buffers */
#define X_BIN_READ   0x1400u
#define X_BIN_WRITE  0x0C00u
#define X_BIN_STRIDE 0x20u
#define X_ADDR_MAX   0x2000u       /* buffer addresses the mirror holds */
#define X_SLOTS      256
#define FX_MAX       32
#define CODE_AREA    0x818u        /* image offset of the first effect's code */
#define SHARED_MAX   0x20000u

/* State words every effect has. */
#define W_FLAGS 4
#define W_PTR   5            /* the first buffer pointer */
#define F_ADD   2u           /* flag bit 1: outputs are added to, not overwritten */

enum { FX_NONE, FX_GAIN, FX_MIXER, FX_FILTER, FX_REVERB, FX_CROSSTALK, FX_KINDS };
static const char *const k_kind[FX_KINDS] = {
    "not reproduced", "gain", "mixer", "filter", "reverb", "crosstalk"
};

static float frac24(uint32_t w) { return (float)(int32_t)(w << 8) / 2147483648.0f; }
static float limit(float x) { return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x; }

/* ── Switches ───────────────────────────────────────────────────────────── */

static int s_cfg, s_full, s_log, s_off, s_hrtf_norm, s_out_start, s_surround;
static const char *const k_out[3] = {
    "stereo", "headphones (no speaker crosstalk)", "5.1 surround"
};

static int out_of(const char *q)
{
    return !q ? APU_OUTPUT_STEREO
         : !strcmp(q, "surround") ? APU_OUTPUT_SURROUND
         : !strcmp(q, "headphones") ? APU_OUTPUT_HEADPHONES : APU_OUTPUT_STEREO;
}

static void cfg(void)
{
    const char *p, *l, *o, *n;
    if (s_cfg)
        return;
    s_cfg = 1;
    p = getenv("RECOMP_APU_PROCESSING");
    l = getenv("RECOMP_APU_FX_LOG");
    o = getenv("RECOMP_APU_FX_OFF");
    n = getenv("RECOMP_APU_DSP_HRTF_NORM");
    s_full = p && !strcmp(p, "full");
    s_out_start = out_of(getenv("RECOMP_APU_OUTPUT"));
    s_surround = s_out_start == APU_OUTPUT_SURROUND;
    s_hrtf_norm = n && atoi(n) != 0;
    s_log = (l && *l) ? atoi(l) : 0;
    if (o && *o)
        for (int k = 1; k < FX_KINDS; k++)
            if (strstr(o, k_kind[k]))
                s_off |= 1 << k;
    fprintf(stderr, "[APUFX] processing %s, output %s%s%s\n",
            s_surround ? "full (5.1 surround always plays the effects image's chain)"
            : s_full ? "full (the effects image's chain plays)"
                     : "standard (the monitor mix plays)",
            k_out[s_out_start],
            s_log ? ", log on" : "", s_off ? ", some effects off (RECOMP_APU_FX_OFF)" : "");
}

int apu_fx_surround(void)
{
    cfg();
    return s_surround;
}

/* The crosstalk's cross term at this frame's start and end: 1 for Stereo
 * (and 5.1: as the game sets it), 0 for Headphones, faded between over
 * XF_FRAMES frames (apu_fx_frame_begin). */
static float s_xt0 = 1.0f, s_xt1 = 1.0f;

/* ── X memory buffers ───────────────────────────────────────────────────
 * A float mirror of the 32-sample buffers the effects name by X address,
 * one per address as on the DSP (a bin's write address is its read
 * address). Nothing on the guest reads them. */
static uint16_t s_slot_of[X_ADDR_MAX];   /* 0 = none yet, else slot + 1 */
static float s_buf[X_SLOTS][APU_FX_FRAME];
static float s_dummy[APU_FX_FRAME];
static int s_slots;

static float *buf_at(uint32_t a)
{
    uint16_t s;
    if (a >= X_BIN_WRITE && a < X_BIN_READ)
        a += X_BIN_READ - X_BIN_WRITE;
    if (a >= X_ADDR_MAX)
        return memset(s_dummy, 0, sizeof s_dummy);
    s = s_slot_of[a];
    if (!s) {
        if (s_slots >= X_SLOTS)
            return memset(s_dummy, 0, sizeof s_dummy);
        s = s_slot_of[a] = (uint16_t)++s_slots;
        memset(s_buf[s - 1], 0, sizeof s_buf[0]);
    }
    return s_buf[s - 1];
}

/* ── The I3DL2 reverb ───────────────────────────────────────────────────
 * The XDK's reverb as its DSP program runs it: a 24 kHz core, delays in
 * samples at that rate. */
typedef struct { float a, b, z; } Pole;
typedef struct { float *s; uint32_t n, w; } Line;
typedef struct {
    uint32_t refl, tap[4], in[4], chain[8], comb[4], echo;
    float early[4], late, diff[8], rest, cross;
    Pole in_lp, chain_lp[3], loop_lp[4], echo_lp[2];
} RevParams;
typedef struct {
    Line line[14];
    RevParams now, next;
    int on, pending, idle, silent, next_silent;
    float fade, held[4];
    uint32_t tick;
} Reverb;

static const int k_len[14] = { 6000, 6000, 6000, 6000, 680, 612, 640, 720,
                               1392, 1684, 2044, 2476, 24000, 24000 };
static float s_rev_mem[6000 * 4 + 680 + 612 + 640 + 720 + 1392 + 1684 + 2044 + 2476
                       + 24000 * 2];
static Reverb s_rev;

static float pole_run(Pole *p, float x)
{
    return p->z = p->a * p->z + p->b * x + 1e-20f;   /* the offset keeps out denormals */
}

/* A delay of the whole line reads its oldest sample. */
static float line_at(const Line *l, uint32_t d)
{
    return l->s[(l->w + l->n - d % l->n) % l->n];
}

static void line_put(Line *l, float x)
{
    l->s[l->w] = x;
    if (++l->w == l->n)
        l->w = 0;
}

static float lin(float mb) { return powf(10.0f, mb / 2000.0f); }

/* y = a * y1 + b * x: mb millibels at 0 Hz, hf more at ref Hz. */
static Pole design(int mb, int hf, float ref, float rate)
{
    Pole p = { 0.0f, 0.0f, 0.0f };
    float g, c;
    if (mb <= -10000)
        return p;
    p.b = mb ? lin((float)mb) : -1.0f;   /* 1.0 is past the DSP's fractions: -1 */
    if (hf >= 0)
        return p;
    p.b = lin((float)mb);
    g = lin(2.0f * (float)hf);
    c = cosf(6.2831853f * ref / rate);
    p.a = (1 - g * c - sqrtf(2 * g * (1 - c) - g * g * (1 - c * c))) / (1 - g);
    p.b *= 1 - p.a;
    return p;
}

/* A loop of n samples loses 60 dB in the decay time, high frequencies sooner
 * by the ratio. */
static Pole loop_pole(int n, float decay, float ratio, float ref)
{
    float x = (float)n / 24000.0f / decay * -6000.0f;
    int lf = (int)x, hf = (int)(ratio < 1 ? x / ratio : x);
    return design(lf, hf - lf, ref, 24000.0f);
}

static int al(int n) { return (n + 1) & ~3; }
/* Taps are read in blocks of 4 samples, 16 back at least. */
static uint32_t tapn(int n) { int t = (n + 3) & ~3; return (uint32_t)(t > 16 ? t : 16); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void reverb_init(Reverb *r)
{
    float *m = s_rev_mem;
    memset(r, 0, sizeof *r);
    memset(s_rev_mem, 0, sizeof s_rev_mem);
    for (int i = 0; i < 14; i++) {
        r->line[i].s = m;
        r->line[i].n = (uint32_t)k_len[i];
        m += k_len[i];
    }
    r->fade = 1.0f;
}

/* Lines and pole histories back to silence (the reverb went quiet). */
static void reverb_clear(Reverb *r)
{
    memset(s_rev_mem, 0, sizeof s_rev_mem);
    for (int i = 0; i < 14; i++)
        r->line[i].w = 0;
    r->now.in_lp.z = 0;
    for (int k = 0; k < 4; k++) { r->now.loop_lp[k].z = 0; r->held[k] = 0; }
    for (int k = 0; k < 3; k++) r->now.chain_lp[k].z = 0;
    r->now.echo_lp[0].z = r->now.echo_lp[1].z = 0;
}

/* DSI3DL2LISTENER as 12 floats, the library's own conversion. */
static void reverb_set(Reverb *r, const float *p)
{
    static const float kGain[4] = { 0.57f, 0.52f, 0.54f, 0.6f };
    static const float kTap[4] = { 0.1052f, 0.0673f, 0.1737f, 0.2404f };
    static const float kComb[4] = { 0.2469f, 0.2209f, 0.2311f, 0.2608f };
    static const float kCombBase[4] = { 0.003651f, 0.003515f, 0.003673f, 0.003946f };
    static const float kChain[8] = { 0.040023f, 0.288186f, 0.603764f, 0.851905f,
                                     0.021224f, 0.37542f, 0.623583f, 0.759229f };
    static const float kSign[8] = { 1, -1, -1, 1, 1, 1, -1, -1 };
    static const int kInput[4] = { 0, 58, 134, 232 };
    float decay = fmaxf(p[3], 0.1f), ratio = fmaxf(p[4], 0.1f);
    float d = clampf(p[9] * 0.01f, 0.0f, 1.0f);
    float sc = clampf((2 - p[10] * 0.01f) * (decay - 1) * 5, 1.0f, 60.0f);
    float f = sc >= 2.5f ? 1.0f : (sc - 0.8f) * 0.5882353f;
    int refl, rev, echo, prev = 0, quiet;
    RevParams q;

    memset(&q, 0, sizeof q);
    q.in_lp = design(0, (int)p[1], p[11], 48000.0f);
    refl = al((int)(p[6] * 24000));
    rev = (int)(p[8] * 24000);
    echo = (int)(f * 24000);
    q.refl = tapn(refl);
    q.late = lin(p[7] + p[0]);
    for (int k = 0; k < 4; k++) {
        int a, b, m, c;
        float rr;
        q.early[k] = lin(p[5] + p[0]) * kGain[k];
        q.tap[k] = tapn(refl + al((int)(rev * kTap[k])));
        q.in[k] = tapn(refl + al(rev) + kInput[k]);
        c = al((int)((p[8] * kComb[k] + kCombBase[k]) * 24000));
        q.comb[k] = tapn(c < k_len[4 + k] - 1 ? c : k_len[4 + k] - 1);
        q.loop_lp[k] = loop_pole(k_len[8 + k], decay, ratio, p[11]);
        /* Echo times count from the input; a stage of the chain holds 250 ms. */
        a = al((int)(f * kChain[k] * 24000));
        b = al((int)(f * kChain[4 + k] * 24000));
        m = (a > b ? a : b) - prev;
        rr = m > 5999 ? 5999.0f / (float)m : 0.0f;
        q.chain[k * 2] = tapn(rr != 0.0f ? al((int)((a - prev) * rr)) : a - prev);
        q.chain[k * 2 + 1] = tapn(rr != 0.0f ? al((int)((b - prev) * rr)) : b - prev);
        prev = b;
    }
    q.echo = tapn(al(echo) < 23999 ? al(echo) : 23999);
    for (int k = 0; k < 3; k++)
        q.chain_lp[k] = loop_pole(echo >> 2, decay, ratio, p[11]);
    q.echo_lp[0] = q.echo_lp[1] = loop_pole(echo, decay, ratio, p[11]);
    for (int k = 0; k < 8; k++)
        q.diff[k] = d * kSign[k];
    q.rest = d > 0 ? 1 - d : -1;
    q.cross = 0.2f * d;

    /* Below -90 dB both ways: nothing to hear, so nothing to run (story
     * mode's preset 25 is -10000 mB room). */
    quiet = q.late < 3.2e-5f;
    for (int k = 0; k < 4; k++)
        quiet &= q.early[k] < 3.2e-5f;

    /* A change fades out, swaps, fades in. */
    if (r->on) {
        r->next = q;
        r->next_silent = quiet;
        r->pending = 1;
    } else {
        r->now = q;
        r->silent = quiet;
        r->pending = 0;
    }
    r->on = 1;
    r->idle = 0;
}

/* One 24 kHz sample in; front left, front right, back left, back right out. */
static void reverb_frame(Reverb *r, float x, float o[4])
{
    static const float kFeed[4] = { -0.4f, 0.4f, -0.4f, -0.4f };
    RevParams *q = &r->now;
    Line *l = r->line;
    float t = line_at(&l[0], q->refl), e[4], in[4], c[8], u[4], sum = 0, sa, sb, ea, eb;

    for (int k = 0; k < 4; k++) {
        e[k] = t + line_at(&l[0], q->tap[k]) + kFeed[k] * line_at(&l[4 + k], q->comb[k]);
        in[k] = line_at(&l[0], q->in[k]);
        c[k * 2] = line_at(&l[k], q->chain[k * 2]);
        c[k * 2 + 1] = line_at(&l[k], q->chain[k * 2 + 1]);
    }
    /* The echoes, front and back. */
    sa = c[1] + c[2] + c[4] + c[7];
    sb = c[0] + c[3] + c[5] + c[6];
    ea = q->rest * sa + line_at(&l[12], q->echo);
    eb = q->rest * sb + line_at(&l[13], q->echo);
    for (int j = 0; j < 4; j++) {
        u[j] = line_at(&l[8 + j], 0) + q->diff[j * 2] * in[(j & 1) ? 2 : 0]
             + q->diff[j * 2 + 1] * in[(j & 1) ? 3 : 1];
        sum += u[j];
    }
    for (int j = 0; j < 4; j++) {
        o[j] = q->early[j] * e[j] + q->late * u[j] + q->late * 0.5f * (j < 2 ? ea : eb);
        line_put(&l[4 + j], e[j]);
        line_put(&l[8 + j], pole_run(&q->loop_lp[j], u[j] - 0.5f * sum + q->cross * (sa + sb)));
    }
    line_put(&l[0], x);
    for (int k = 0; k < 3; k++)
        line_put(&l[k + 1], pole_run(&q->chain_lp[k], c[k * 2 + 1]));
    line_put(&l[12], pole_run(&q->echo_lp[0], ea));
    line_put(&l[13], pole_run(&q->echo_lp[1], eb));
}

/* The input at 48 kHz, halved, filtered, every second sample run; each
 * output sample is added twice. */
static void reverb_run(Reverb *r, const float *in, float *out[4])
{
    if (!r->on || r->idle)
        return;
    if (!r->pending && r->silent && r->fade >= 1.0f) {
        reverb_clear(r);   /* no stale tail when a preset comes back */
        r->idle = 1;
        return;
    }
    for (uint32_t i = 0; i < APU_FX_FRAME; i++) {
        float x = pole_run(&r->now.in_lp, in[i] * 0.5f);
        if (!(r->tick++ & 1)) {
            float o[4];
            if (r->pending && (r->fade -= 1 / 128.0f) <= 0) {
                r->now = r->next;
                r->silent = r->next_silent;
                r->pending = 0;
                r->fade = 0;
            } else if (!r->pending) {
                r->fade = fminf(r->fade + 1 / 128.0f, 1.0f);
            }
            reverb_frame(r, x, o);
            for (int k = 0; k < 4; k++)
                r->held[k] = o[k] * r->fade;
        }
        for (int k = 0; k < 4; k++)
            out[k][i] = limit(out[k][i] + r->held[k]);
    }
}

/* ── The effects ────────────────────────────────────────────────────────── */

typedef struct {
    int kind;
    uint32_t x;                 /* its state's X address */
    float bq[4];                /* filter: x1, x2, y1, y2 */
    float xt_line[2][2][11];    /* crosstalk: per pair, each side's output */
    float xt_lp[2][2];
    uint32_t xt_at[2];
} Fx;

static Fx s_fx[FX_MAX];
static int s_fx_count;
static volatile uint32_t *s_xmem;

static uint32_t word(const Fx *e, uint32_t i) { return s_xmem[e->x + i] & 0xFFFFFFu; }
static float *ptr(const Fx *e, uint32_t i) { return buf_at(word(e, W_PTR + i)); }

/* One input to up to 8 outputs, a gain each: pointers in, out x8, then the
 * output count and the gains. */
static void run_gain(const Fx *e)
{
    const float *in = ptr(e, 0);
    int add = (word(e, W_FLAGS) & F_ADD) != 0;
    uint32_t count = word(e, 14);
    if (count > 8)
        count = 8;
    for (uint32_t k = 0; k < count; k++) {
        float *out = ptr(e, 1 + k);
        float g = frac24(word(e, 15 + k));
        for (int i = 0; i < APU_FX_FRAME; i++)
            out[i] = limit((add ? out[i] : 0.0f) + g * in[i]);
    }
}

/* out = out + g0 * in0 + g1 * in1, saturating; without flag bit 1 the
 * output and in0 are not read. */
static void run_mixer(const Fx *e)
{
    const float *in0 = ptr(e, 0), *in1 = ptr(e, 1);
    float *out = ptr(e, 2);
    int add = (word(e, W_FLAGS) & F_ADD) != 0;
    float g0 = frac24(word(e, 8)), g1 = frac24(word(e, 9));
    for (int i = 0; i < APU_FX_FRAME; i++) {
        float sum = g1 * in1[i];
        if (add)
            sum += out[i] + g0 * in0[i];
        out[i] = limit(sum);
    }
}

/* A biquad whose coefficients (b0, b1, b2, a1, a2 from word 7) are stored
 * halved, run in scaling mode: the history holds full-scale values, the
 * output buffer gets the half-scale result. */
static void run_filter(Fx *e)
{
    const float *in = ptr(e, 0);
    float *out = ptr(e, 1);
    int add = (word(e, W_FLAGS) & F_ADD) != 0;
    float c[5], *h = e->bq;
    for (uint32_t k = 0; k < 5; k++)
        c[k] = frac24(word(e, 7 + k));
    for (int i = 0; i < APU_FX_FRAME; i++) {
        float x = in[i];
        float half = c[0] * x + c[1] * h[0] + c[2] * h[1] + c[3] * h[2] + c[4] * h[3];
        h[1] = h[0];
        h[0] = x;
        h[3] = h[2];
        h[2] = limit(2 * half);
        out[i] = limit((add ? out[i] : 0.0f) + half);
    }
}

/* Input pointer 0 (the second is never read); outputs front left, front
 * right, back left, back right. */
static void run_reverb(const Fx *e)
{
    float *out[4];
    for (uint32_t k = 0; k < 4; k++)
        out[k] = ptr(e, 2 + k);
    reverb_run(&s_rev, ptr(e, 0), out);
}

/* Two pairs (inputs 0, 1 and 2, 3; outputs 4, 5 and 6, 7): each side adds
 * the other side's output 11 samples back through a one-pole low pass
 * (constants the effect sets on its first run). Flag bit 1 keeps the cross
 * term; without it the inputs pass straight through. Output Headphones fades
 * the cross term out (s_xt0..s_xt1); its history runs on either way, so it
 * comes back without a jump. */
static void run_crosstalk(Fx *e, int cross)
{
    const float kPole = 0.924f;     /* 0x7645A1 */
    const float kFeed = -0.0585f;   /* 0xF88313 */
    cross = cross && (word(e, W_FLAGS) & F_ADD);
    for (uint32_t p = 0; p < 2; p++) {
        const float *inl = ptr(e, p * 2), *inr = ptr(e, p * 2 + 1);
        float *outl = ptr(e, 4 + p * 2), *outr = ptr(e, 4 + p * 2 + 1);
        float (*line)[11] = e->xt_line[p];
        float *lp = e->xt_lp[p];
        uint32_t at = e->xt_at[p];
        for (int i = 0; i < APU_FX_FRAME; i++) {
            float left, right;
            lp[0] = kPole * lp[0] + kFeed * line[0][at];
            lp[1] = kPole * lp[1] + kFeed * line[1][at];
            left = limit(inl[i] + lp[1]);
            right = limit(inr[i] + lp[0]);
            line[0][at] = left;
            line[1][at] = right;
            if (!cross) {
                left = inl[i];
                right = inr[i];
            } else if (s_xt0 < 1.0f || s_xt1 < 1.0f) {
                float w = s_xt0 + (s_xt1 - s_xt0) * (float)(i + 1) / APU_FX_FRAME;
                left = inl[i] + (left - inl[i]) * w;
                right = inr[i] + (right - inr[i]) * w;
            }
            outl[i] = limit(outl[i] + left);
            outr[i] = limit(outr[i] + right);
            at = (at + 1) % 11;
        }
        e->xt_at[p] = at;
    }
}

/* ── Recognising the effects ──────────────────────────────────────────── */

/* FNV-1a of the plain code with word 1 (the effect's state address) left out. */
static uint64_t code_hash(const uint8_t *code, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = (i >= 4 && i < 8) ? 0 : code[i];
        h = (h ^ b) * 0x100000001B3ull;
    }
    return h;
}

static int kind_of_hash(uint64_t h)
{
    switch (h) {
    case 0xDDD7C789A714E88Bull: return FX_GAIN;       /* 36 words */
    case 0xFA00AFA73549BF9Bull: return FX_MIXER;      /* 42 words */
    case 0xA8ACC4A41FE77751ull: return FX_FILTER;     /* 48 words */
    case 0x13CF05D0E014E22Eull: return FX_REVERB;     /* 1211 words */
    case 0xF1E0A321578CFED9ull: return FX_CROSSTALK;  /* 116 words */
    default: return FX_NONE;
    }
}

static int kind_of_size(uint32_t words)
{
    switch (words) {
    case 36: return FX_GAIN;
    case 42: return FX_MIXER;
    case 48: return FX_FILTER;
    case 1211: return FX_REVERB;
    case 116: return FX_CROSSTALK;
    default: return FX_NONE;
    }
}

static uint32_t rd32(const uint8_t *p) { return *(const volatile uint32_t *)p; }

/* The listener, handed over from any thread (a sequence lock: odd while
 * written; one writer). */
static volatile uint32_t s_l_seq;
static volatile uint32_t s_l_raw[12];
static uint32_t s_l_taken;
static float s_l_now[12];
static int s_l_have;

static void listener_floats(const uint32_t raw[12], float p[12])
{
    for (int i = 0; i < 12; i++) {
        if (i == 0 || i == 1 || i == 5 || i == 7) {
            p[i] = (float)(int32_t)raw[i];
        } else {
            float f;
            memcpy(&f, &raw[i], 4);
            p[i] = f;
        }
    }
}

void apu_fx_load(const uint8_t *shared, uint32_t table, volatile uint32_t *xmem)
{
    uint32_t n, code0 = 0;
    int by_size = 0;
    char kinds[512];
    int len = 0;

    cfg();
    s_fx_count = 0;
    if (table + 8u > SHARED_MAX)
        return;
    n = rd32(shared + table);
    if (n > FX_MAX || table + 8u + n * 32u > SHARED_MAX) {
        fprintf(stderr, "[APUFX] effect list of %u effects at +0x%X: not loaded\n", n, table);
        return;
    }
    s_xmem = xmem;
    memset(s_fx, 0, sizeof s_fx);
    memset(s_slot_of, 0, sizeof s_slot_of);
    s_slots = 0;
    reverb_init(&s_rev);
    if (s_l_have)
        reverb_set(&s_rev, s_l_now);   /* a reload keeps the listener */

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *d = shared + table + 8u + i * 32u;
        uint32_t code = rd32(d), code_bytes = rd32(d + 4);
        uint32_t state = rd32(d + 8), state_bytes = rd32(d + 12);
        uint32_t off;
        Fx *e = &s_fx[i];

        if (i == 0)
            code0 = code;
        /* The code blocks lie back to back from the image's code area, in
         * the shared copy already decrypted (DirectSound relocated the
         * descriptors to P memory addresses). */
        off = CODE_AREA + (code - code0);
        if (code >= code0 && off < SHARED_MAX && code_bytes <= SHARED_MAX - off)
            e->kind = kind_of_hash(code_hash(shared + off, code_bytes));
        if (e->kind == FX_NONE && kind_of_size(code_bytes / 4)) {
            e->kind = kind_of_size(code_bytes / 4);
            by_size = 1;
        }
        if (state < GP_X_CPU || (state - GP_X_CPU) / 4 + state_bytes / 4 > GP_X_WORDS ||
            state_bytes < 4 * 16)
            e->kind = FX_NONE;
        else
            e->x = (state - GP_X_CPU) / 4;
        if (len < (int)sizeof kinds - 40)
            len += snprintf(kinds + len, sizeof kinds - len, " %u %s,", i, k_kind[e->kind]);
        if (s_log >= 2 && e->kind != FX_NONE) {
            char line[256];
            int l2 = snprintf(line, sizeof line, "[APUFX]   %u %s X:0x%X words",
                              i, k_kind[e->kind], e->x);
            for (uint32_t k = 4; k < 16 && l2 < (int)sizeof line - 8; k++)
                l2 += snprintf(line + l2, sizeof line - l2, " %06X", word(e, k));
            fprintf(stderr, "%s\n", line);
        }
    }
    s_fx_count = (int)n;
    if (len)
        kinds[len - 1] = 0;
    fprintf(stderr, "[APUFX] effects image: %u effects (kinds by %s):%s\n", n,
            by_size ? "code size for some" : "code hash", kinds);
}

/* ── Processing: Standard or Full DSP, switched live ──────────────────────
 * xbox_ApuSetAudioMode (any thread) asks; the frame thread takes it at its
 * next frame (apu_fx_frame_begin) and crossfades the two outputs over
 * XF_FRAMES frames (apu_dsp.c blends by apu_fx_share). Full DSP needs a
 * loaded image: until the doorbell's load it waits, then fades in. A switch
 * to Full from silence clears every effect's history (buffers, filters,
 * crosstalk, the reverb's lines), so nothing from the last time plays; a
 * switch back while it still fades out keeps it (no jump). */
#define XF_FRAMES 32                  /* 32 frames of 32 samples: 21 ms */
static volatile int s_req = -1;       /* asked: 1 full, 0 standard, -1 not yet */
static volatile int s_req_out = -1;   /* asked: APU_OUTPUT_*, -1 not yet */
static int s_out = -1;                /* the frame thread's output, -1 before the first frame */
static int s_out_asked = -1;          /* the last request it saw (log) */
static int s_on;                      /* the frame thread's target: Full */
static int s_xf;                      /* crossfade frames left */
static float s_w0, s_w1;              /* Full's share at this frame's start, end */

static void fx_reset(void)
{
    for (int s = 0; s < s_slots; s++)
        memset(s_buf[s], 0, sizeof s_buf[0]);
    for (int k = 0; k < s_fx_count; k++) {
        memset(s_fx[k].bq, 0, sizeof s_fx[k].bq);
        memset(s_fx[k].xt_line, 0, sizeof s_fx[k].xt_line);
        memset(s_fx[k].xt_lp, 0, sizeof s_fx[k].xt_lp);
    }
    if (s_rev.on)
        reverb_clear(&s_rev);
}

void apu_fx_set_mode(int processing, int output)
{
    s_req_out = output >= APU_OUTPUT_STEREO && output <= APU_OUTPUT_SURROUND
              ? output : APU_OUTPUT_STEREO;
    s_req = processing == APU_PROCESSING_FULL;
}

/* The output asked for, as it applies now: 5.1 is fixed at the start (the
 * game was told at boot, apu_core.c chose the channels), so a run started
 * in 5.1 stays there and 5.1 asked later waits for the next start (the
 * output stays as it is meanwhile). */
static void take_output(uint32_t frame)
{
    int asked = s_req_out >= 0 ? s_req_out : s_out_start;
    int out = asked;
    if (s_surround)
        out = APU_OUTPUT_SURROUND;
    else if (out == APU_OUTPUT_SURROUND)
        out = s_out >= 0 ? s_out : APU_OUTPUT_STEREO;
    if (asked != s_out_asked && s_out >= 0 && out == s_out && asked != out)
        fprintf(stderr, "[APUFX] output %s asked: from the next start (%s plays)\n",
                k_out[asked], k_out[out]);
    s_out_asked = asked;
    if (out != s_out) {
        if (s_out < 0)
            s_xt1 = out == APU_OUTPUT_HEADPHONES ? 0.0f : 1.0f;   /* no fade at the start */
        else
            fprintf(stderr, "[APUFX] output -> %s (crossfade %d ms) at APU frame %u\n",
                    k_out[out], XF_FRAMES * APU_FX_FRAME / 48, frame);
        s_out = out;
    }
    s_xt0 = s_xt1;
    if (s_out == APU_OUTPUT_HEADPHONES)
        s_xt1 = s_xt1 - 1.0f / XF_FRAMES > 0.0f ? s_xt1 - 1.0f / XF_FRAMES : 0.0f;
    else
        s_xt1 = s_xt1 + 1.0f / XF_FRAMES < 1.0f ? s_xt1 + 1.0f / XF_FRAMES : 1.0f;
}

/* xbox_ApuSetAudioMode (apu.h): the DLL's side. */
void xbox_ApuSetAudioMode(int processing, int output)
{
    apu_fx_set_mode(processing, output);
}

void apu_fx_frame_begin(void)
{
    static uint32_t frame;            /* APU frames so far: x 32 = output samples */
    int want;
    cfg();
    take_output(frame);
    want = (s_surround || (s_req >= 0 ? s_req : s_full)) && s_fx_count > 0;
    s_w0 = s_w1;
    if (want != s_on) {
        s_on = want;
        s_xf = XF_FRAMES - s_xf;      /* turned round mid-fade: from there */
        if (want && s_w0 <= 0.0f)
            fx_reset();
        fprintf(stderr, "[APUFX] processing -> %s (crossfade %d ms) at APU frame %u\n",
                want ? "full (the effects image's chain plays)"
                     : "standard (the monitor mix plays)",
                s_xf * APU_FX_FRAME / 48, frame);
    }
    frame++;
    if (s_xf > 0)
        s_xf--;
    s_w1 = s_on ? 1.0f - (float)s_xf / XF_FRAMES : (float)s_xf / XF_FRAMES;
}

int apu_fx_wanted(void)
{
    cfg();
    return s_fx_count > 0 && (s_on || s_w0 > 0.0f || s_w1 > 0.0f || s_log);
}

int apu_fx_full(void)
{
    return s_w0 > 0.0f || s_w1 > 0.0f;
}

int apu_fx_full_only(void)
{
    return s_w0 >= 1.0f && s_w1 >= 1.0f;
}

float apu_fx_share(int i)
{
    return s_w0 + (s_w1 - s_w0) * (float)(i + 1) / APU_FX_FRAME;
}

int apu_fx_raw_ears(void)
{
    cfg();
    return !s_hrtf_norm;
}

void apu_fx_set_listener(const void *listener48)
{
    const uint32_t *w = (const uint32_t *)listener48;
    s_l_seq++;
    FX_BARRIER();
    for (int i = 0; i < 12; i++)
        s_l_raw[i] = w[i];
    FX_BARRIER();
    s_l_seq++;
}

/* xbox_ApuSetI3dl2Listener (apu.h): the DLL's side. */
void xbox_ApuSetI3dl2Listener(const void *listener48)
{
    apu_fx_set_listener(listener48);
}

static void take_listener(void)
{
    uint32_t s1 = s_l_seq, raw[12];
    if (s1 == s_l_taken || (s1 & 1))
        return;
    FX_BARRIER();
    for (int i = 0; i < 12; i++)
        raw[i] = s_l_raw[i];
    FX_BARRIER();
    if (s_l_seq != s1)
        return;   /* written meanwhile: the next frame takes it */
    s_l_taken = s1;
    listener_floats(raw, s_l_now);
    s_l_have = 1;
    reverb_set(&s_rev, s_l_now);
    if (s_log)
        fprintf(stderr, "[APUFX] listener: room %.0f HF %.0f decay %.2f s ratio %.2f"
                " reflections %.0f mB %.3f s reverb %.0f mB %.3f s diffusion %.0f"
                " density %.0f HF ref %.0f -> %s\n",
                s_l_now[0], s_l_now[1], s_l_now[3], s_l_now[4], s_l_now[5],
                s_l_now[6], s_l_now[7], s_l_now[8], s_l_now[9], s_l_now[10],
                s_l_now[11], (s_rev.pending ? s_rev.next_silent : s_rev.silent)
                                 ? "silent" : "on");
}

/* ── One frame ─────────────────────────────────────────────────────────── */

static double now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
#endif
}

static double s_log_us;
static uint32_t s_log_frames;
static float s_log_peak_in[APU_FX_SPEAKERS], s_log_peak[APU_FX_SPEAKERS], s_log_rev_peak;

void apu_fx_run(float bins[APU_FX_BINS][APU_FX_FRAME],
                float out[APU_FX_SPEAKERS][APU_FX_FRAME])
{
    double t0 = s_log ? now_us() : 0.0;

    take_listener();
    /* The voice processor writes every bin fresh each frame. */
    for (uint32_t b = 0; b < APU_FX_BINS; b++) {
        float *d = buf_at(X_BIN_READ + b * X_BIN_STRIDE);
        for (int i = 0; i < APU_FX_FRAME; i++)
            d[i] = limit(bins[b][i]);
        if (s_log && b < APU_FX_SPEAKERS)
            for (int i = 0; i < APU_FX_FRAME; i++)
                if (fabsf(d[i]) > s_log_peak_in[b]) s_log_peak_in[b] = fabsf(d[i]);
    }
    for (int k = 0; k < s_fx_count; k++) {
        Fx *e = &s_fx[k];
        if (e->kind != FX_CROSSTALK && (s_off & (1 << e->kind)))
            continue;
        switch (e->kind) {
        case FX_GAIN:      run_gain(e); break;
        case FX_MIXER:     run_mixer(e); break;
        case FX_FILTER:    run_filter(e); break;
        case FX_REVERB:    run_reverb(e); break;
        case FX_CROSSTALK: run_crosstalk(e, !(s_off & (1 << FX_CROSSTALK))); break;
        default: break;
        }
    }
    for (uint32_t b = 0; b < APU_FX_SPEAKERS; b++)
        memcpy(out[b], buf_at(X_BIN_READ + b * X_BIN_STRIDE), sizeof out[b]);

    if (!s_log)
        return;
    for (int b = 0; b < APU_FX_SPEAKERS; b++)
        for (int i = 0; i < APU_FX_FRAME; i++)
            if (fabsf(out[b][i]) > s_log_peak[b]) s_log_peak[b] = fabsf(out[b][i]);
    for (int k = 0; k < 4; k++)
        if (fabsf(s_rev.held[k]) > s_log_rev_peak) s_log_rev_peak = fabsf(s_rev.held[k]);
    s_log_us += now_us() - t0;
    if (++s_log_frames >= 48000 * 5 / APU_FX_FRAME) {
        int run[FX_KINDS] = { 0 };
        char line[256];
        int len = 0;
        for (int k = 0; k < s_fx_count; k++)
            run[s_fx[k].kind]++;
        for (int k = 1; k < FX_KINDS; k++)
            if (run[k])
                len += snprintf(line + len, sizeof line - len, "%s%d %s%s", len ? ", " : "",
                                run[k], k_kind[k], (s_off & (1 << k)) ? " (off)" : "");
        fprintf(stderr, "[APUFX] 5 s: %s; %d not reproduced; %.0f us a second of audio;"
                " reverb %s, peak %.3f; speakers in/out FL %.2f/%.2f FR %.2f/%.2f"
                " C %.2f/%.2f LFE %.2f/%.2f BL %.2f/%.2f BR %.2f/%.2f; %d buffers\n",
                line, run[FX_NONE], s_log_us / 5.0,
                !s_rev.on ? "no listener yet" : s_rev.idle ? "silent (idle)"
                          : s_rev.pending ? "changing" : "on",
                s_log_rev_peak,
                s_log_peak_in[0], s_log_peak[0], s_log_peak_in[1], s_log_peak[1],
                s_log_peak_in[2], s_log_peak[2], s_log_peak_in[3], s_log_peak[3],
                s_log_peak_in[4], s_log_peak[4], s_log_peak_in[5], s_log_peak[5], s_slots);
        s_log_us = 0.0;
        s_log_frames = 0;
        s_log_rev_peak = 0.0f;
        memset(s_log_peak, 0, sizeof s_log_peak);
        memset(s_log_peak_in, 0, sizeof s_log_peak_in);
    }
}
