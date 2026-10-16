/* mcpx_apu_dsp_frame must mix every bin the guest routed to down to stereo,
 * each on the speaker side DirectSound's DSMIXBIN layout gives it. See
 * README.md for the measurements that found both defects.
 *
 * Run with: ctest --test-dir <build>   (one arm per mixdown; see CMakeLists.txt) */
#include "apu_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

/* The bins a title's 3D positional voices actually land in, from JSRF's own
 * V0BIN..V3BIN: XTLK front/back left/right (6..9) and the I3DL2 send (10). */
#define EFFECT_BIN_LO 6
#define EFFECT_BIN_HI 10

static MCPXAPUState *d;
static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

/* Signal 0.5 in bins lo..hi only; returns the output's peak per side. */
static void run(int lo, int hi, int *left, int *right)
{
    int i, b;
    memset(mixbins, 0, sizeof(mixbins));
    for (b = lo; b <= hi; ++b)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i)
            mixbins[b][i] = 0.5f;
    mcpx_apu_dsp_frame(d, mixbins);
    *left = *right = 0;
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        if (abs(d->monitor.frame_buf[i][0]) > *left)  *left  = abs(d->monitor.frame_buf[i][0]);
        if (abs(d->monitor.frame_buf[i][1]) > *right) *right = abs(d->monitor.frame_buf[i][1]);
    }
}

static int fails;

static void check(int ok, const char *what, int l, int r)
{
    printf("%s: %s (left %d, right %d)\n", ok ? "PASS" : "FAIL", what, l, r);
    if (!ok) fails++;
}

int main(void)
{
    const char *arm = getenv("APU_MIXDOWN_ARM");
    int l, r;

    if (!arm) arm = "speakers";
    /* Zeroed is deliberately enough: ram_ptr NULL makes dsp_ack_frame return
     * before it touches guest memory, and monitor.point 0 is MON_AC97, which
     * is not MON_VP, so the mixdown runs. */
    d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) { fprintf(stderr, "out of memory\n"); return 2; }

    if (!strcmp(arm, "speakers")) {
        /* Bins 0 and 1 stay silent, so anything that arrives can only have
         * come from the wider mixdown. */
        run(EFFECT_BIN_LO, EFFECT_BIN_HI, &l, &r);
        check(l && r, "3D bins 6..10 reach both channels", l, r);
        /* Conker: Live & Reloaded routes dialogue to the centre bin; the
         * even/odd sum put it on the left only. */
        run(2, 2, &l, &r);
        check(l && l == r, "centre bin lands equally on both sides", l, r);
        run(6, 6, &l, &r);
        check(l && !r, "XTLK front left stays left", l, r);
        run(7, 7, &l, &r);
        check(!l && r, "XTLK front right stays right", l, r);
        /* LFE and I3DL2 are sends to DSP stages that do not exist; 3D voices
         * also carry them at their direct level, so dry they double up. */
        run(3, 3, &l, &r);
        check(!l && !r, "LFE send not fed dry by default", l, r);
        run(10, 10, &l, &r);
        check(!l && !r, "I3DL2 send not fed dry by default", l, r);
        run(11, 30, &l, &r);
        check(l && l == r, "FXSEND bins centred", l, r);
    } else if (!strcmp(arm, "evenodd")) {
        /* The negative control: the old sum must fail the centre check,
         * or that check proves nothing. */
        run(2, 2, &l, &r);
        check(l && !r, "old even/odd sum puts the centre bin left only", l, r);
    } else if (!strcmp(arm, "front")) {
        /* RECOMP_APU_MIXDOWN_ALL=0: the original two-bin read. The same 3D
         * signal must be lost, or the speakers arm proves nothing. */
        run(EFFECT_BIN_LO, EFFECT_BIN_HI, &l, &r);
        check(!l && !r, "bins 0/1 only: 3D bins discarded", l, r);
    } else if (!strcmp(arm, "monitor") || !strcmp(arm, "monitor_old")) {
        /* The VP monitor mix's placement (mcpx_apu_monitor_place). Conker's
         * 5.1 boss song: the centre + LFE pair is one stereo voice, column 0
         * (vocals) to bin 2 at -10 dB, column 1 to bin 3, silent. */
        static const int c_bin[8] = { 2, 3, 2, 3, 2, 3, 2, 3 };
        static const int lr_bin[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };
        float c_gain[8] = { 0.316f, 0, 0, 0, 0, 0, 0, 0 };
        float lr_gain[8] = { 0.5f, 0.5f, 0, 0, 0, 0, 0, 0 };
        float out[2][2];
        mcpx_apu_monitor_place(c_bin, c_gain, 2, -1, out);
        l = (int)(out[0][0] * 1000); r = (int)(out[0][1] * 1000);
        if (!strcmp(arm, "monitor_old")) {
            /* The negative control: xemu's placement must show the defect. */
            check(l && !r, "xemu: centre column on the left only", l, r);
            l = (int)(out[1][0] * 1000); r = (int)(out[1][1] * 1000);
            check(!l && r, "xemu: silent LFE column on the right at the centre's gain", l, r);
        } else {
            check(l && l == r, "centre column of a stereo voice on both sides", l, r);
            check(fabsf(out[0][0] * out[0][0] * 2 - 0.316f * 0.316f) < 1e-4f,
                  "centre keeps its power", l, r);
            l = (int)(out[1][0] * 1000); r = (int)(out[1][1] * 1000);
            check(!l && !r, "LFE column the title left silent stays silent", l, r);
            mcpx_apu_monitor_place(lr_bin, lr_gain, 2, -1, out);
            l = (int)(out[0][0] * 1000); r = (int)(out[0][1] * 1000);
            check(l == 500 && !r, "front-left column stays left", l, r);
            l = (int)(out[1][0] * 1000); r = (int)(out[1][1] * 1000);
            check(!l && r == 500, "front-right column stays right", l, r);
            /* Mono voices unchanged: the loudest bin's gain, both sides. */
            mcpx_apu_monitor_place(c_bin, c_gain, 1, -1, out);
            l = (int)(out[0][0] * 1000); r = (int)(out[1][1] * 1000);
            check(l == 316 && r == 316 && !out[0][1] && !out[1][0],
                  "mono voice at its loudest gain, columns to their sides as xemu", l, r);
        }
    } else {
        fprintf(stderr, "unknown APU_MIXDOWN_ARM %s\n", arm);
        fails++;
    }

    free(d);
    return fails ? 1 : 0;
}
