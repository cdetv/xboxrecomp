/* Translated vertex programs (nv2a_vsh_hlsl.c) on a D3D11 device against
 * the CPU interpreter (nv2a_vsh_run): the Xbox D3D epilogue, then random
 * programs as in nv2a_vsh_lanes_test (every MAC and ILU op, random sources,
 * swizzles, negates, masks, outputs, relative constants after an ARL).
 *
 * The GPU's rsq, exp2, log2 and divide are not the CPU's to the bit, so
 * results are compared with nv2a_vsh_outputs_close. Every program must
 * translate and compile, and on WARP (the default: the same results on
 * every machine) every vertex must be close: 47,645 of 47,645 were, once the
 * zeroes stopped being literals and lit's pow of 0 was spelled out
 * (nv2a_vsh_hlsl.c). "hw" as the argument runs on the default hardware
 * adapter instead, where a driver's maths may differ a little: under 0.5%
 * of vertices may differ there (0 on the development PC's GPU, 2026-10-18).
 *
 * VSH_TEST_SHOW=<program> [VSH_TEST_VERTEX=<n>]: that program's HLSL and a
 * step-by-step CPU run of one of its vertices, for a difference. */
#include "nv2a_vsh_interp.h"
#include "nv2a_vsh_hlsl.h"
#include "nv2a_vsh_gpu.h"
#include "nv2a_attr.h"

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_rng = 12345u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    uint32_t k = rnd() % 16;
    if (k == 0) return 0.0f;
    if (k == 1) return (float)((int)(rnd() % 7) - 3);
    return ((float)(rnd() % 20001) - 10000.0f) / 1000.0f;
}

static void random_instruction(uint32_t w[4], int final)
{
    w[0] = 0;
    w[1] = rnd() & 0x0FFFFFFFu;
    w[1] = (w[1] & ~(0xFFu << 13)) | ((rnd() % 200u) << 13);
    w[2] = rnd();
    w[3] = rnd() & ~1u;
    w[3] = (w[3] & ~(0xFFu << 3)) | ((rnd() % 13u) << 3);
    if (rnd() % 8)
        w[3] |= 1u << 11;
    if ((w[1] >> 21 & 15) >= 14)
        w[1] &= ~(15u << 21);
    if (!(w[1] >> 21 & 15) && !(w[1] >> 25 & 7))
        w[1] |= 4u << 21;
    if (final)
        w[3] |= 1u;
}

enum { T = 1, V = 2, C = 3 };
#define XYZW 0x1B
#define WWWW 0xFF
#define XXXX 0x00

static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (mux << 26) | (reg << 28); }
    else if (which == 1) { w[2] |= (swz << 17) | (mux << 11) | (reg << 13); }
    else { w[2] |= (swz << 2) | (reg >> 2); w[3] |= (mux << 28) | ((reg & 3) << 30); }
}

static void print_diff(const char *what, const Nv2aVshOutput *want,
                       const Nv2aVshOutput *got)
{
    /* The fields that are not close (one at a time into a copy of want). */
    const float *p = (const float *)want, *q = (const float *)got;
    int j;
    printf("%s:", what);
    for (j = 0; j < (int)(sizeof *want / sizeof(float)); j++) {
        Nv2aVshOutput t = *want;
        ((float *)&t)[j] = q[j];
        if (nv2a_vsh_outputs_close(want, &t) >= 0)
            printf(" %s %g vs %g", nv2a_vsh_output_name(j), p[j], q[j]);
    }
    putchar('\n');
}

static float s_in[1024][NV2A_VSH_INPUTS][4];
static Nv2aVshOutput s_got[1024];

/* ---- step 2: the GPU's vertex fetch against nv2a_attr_decode ----------- */

static uint8_t s_mem[4 << 20];
static uint32_t s_idx[1024];
static float s_fetched[1024][NV2A_VSH_INPUTS][4];

/* pb_fetch_attr, on a host array. */
static void fetch_ref(const Nv2aVshArray *a, uint32_t i, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->src || !a->size || !a->stride || i > a->last)
        return;
    if (!nv2a_attr_decode(a->src + (size_t)i * a->stride, a->type, a->size,
                          out))
        out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
}

/* How far apart two floats are in units in the last place; both NaN = 0. */
static uint32_t ulps(float x, float y)
{
    int32_t a, b;
    int64_t d;
    if (x != x || y != y)
        return x != x && y != y ? 0 : 0xFFFFFFFFu;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    d = (a < 0 ? (int64_t)INT32_MIN - a : a) - (b < 0 ? (int64_t)INT32_MIN - b : b);
    d = d < 0 ? -d : d;
    return d > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)d;
}

/* Random batches: interleaved vertices of 4..80 bytes with attributes at
 * any byte offset in them (so unaligned ubyte, short and even float reads),
 * separate arrays, every format and some no decoder knows, sizes 0..5,
 * INLINE_ARRAY-style ends, absent and unused inputs, indices anywhere in a
 * range. Floats are compared to the bit (they are copied, not converted);
 * the normalising divides may differ by an ulp. */
static int fetch_test(int hw)
{
    static const uint32_t types[] = {0, 1, 2, 4, 5, 6, 3, 7};
    long values = 0, inexact = 0, worst = 0;
    int b, k, l, c;
    uint32_t i;

    for (i = 0; i < sizeof s_mem; i++)
        s_mem[i] = (uint8_t)rnd();
    for (b = 0; b < 400; b++) {
        Nv2aVshArray a[NV2A_VSH_INPUTS];
        uint32_t stride = 4 + rnd() % 77, used = rnd() & 0xFFFFu;
        uint32_t count = 1 + rnd() % 1024, range = 1 + rnd() % 4000;
        uint32_t base = rnd() % 100000, lo = ~0u, hi = 0;
        uint8_t *vtx = s_mem + 1024 + rnd() % (1u << 20);

        if (b % 50 == 0)
            used = 0xFFFFu;
        memset(a, 0, sizeof a);
        for (k = 0; k < NV2A_VSH_INPUTS; k++) {
            uint32_t r = rnd() % 16;
            a[k].type = types[rnd() % (r < 2 ? 8 : 6)];
            a[k].size = r == 3 ? 0 : r == 4 ? 5 : 1 + rnd() % 4;
            a[k].last = r == 5 ? base + rnd() % (range + 1) : UINT32_MAX;
            if (r == 6) {
                continue;                       /* absent */
            } else if (r < 10) {                /* interleaved */
                a[k].stride = stride;
                a[k].src = vtx + rnd() % stride;
            } else {                            /* an array of its own */
                a[k].stride = nv2a_attr_bytes(a[k].type, a[k].size);
                a[k].stride += rnd() % 3;
                if (!a[k].stride)
                    a[k].stride = 4;
                a[k].src = s_mem + 1024 + (1u << 20) + rnd() % (1u << 20);
            }
            /* Element 0 lies `base` elements back: the copy must not need
             * it. Floats get values a program would see, mostly. */
            a[k].src -= (size_t)base * a[k].stride;
            if (a[k].type == 2 && rnd() % 4)
                for (i = base; i < base + range; i++)
                    for (c = 0; c < (int)a[k].size && c < 4; c++) {
                        float f = rndf();
                        memcpy((uint8_t *)a[k].src + (size_t)i * a[k].stride
                               + 4 * c, &f, 4);
                    }
        }
        for (l = 0; l < (int)count; l++) {
            s_idx[l] = base + rnd() % range;
            if (s_idx[l] < lo) lo = s_idx[l];
            if (s_idx[l] > hi) hi = s_idx[l];
        }
        if (!nv2a_vsh_gpu_inputs(a, used, lo, hi)
            || !nv2a_vsh_gpu_fetch(s_idx, count, s_fetched)) {
            printf("FAIL: fetch batch %d did not run\n", b);
            return 0;
        }
        for (l = 0; l < (int)count; l++)
            for (k = 0; k < NV2A_VSH_INPUTS; k++) {
                float want[4];
                if (!(used >> k & 1))
                    continue;
                fetch_ref(&a[k], s_idx[l], want);
                for (c = 0; c < 4; c++) {
                    uint32_t u = ulps(want[c], s_fetched[l][k][c]);
                    int exact = !memcmp(&want[c], &s_fetched[l][k][c], 4);
                    values++;
                    if (exact)
                        continue;
                    inexact++;
                    if ((long)u > worst)
                        worst = (long)u;
                    if (u > 1 || a[k].type == 2 || a[k].type == 5) {
                        printf("FAIL: fetch batch %d vertex %d (index %u)"
                               " input %d type %u size %u stride %u: %c cpu"
                               " %.9g gpu %.9g\n", b, l, s_idx[l], k,
                               a[k].type, a[k].size, a[k].stride, "xyzw"[c],
                               want[c], s_fetched[l][k][c]);
                        return 0;
                    }
                }
            }
    }
    {
        uint64_t batches, bytes, wraps;
        nv2a_vsh_gpu_input_stats(&batches, &bytes, &wraps);
        printf("fetch: %d batches, %ld values, %ld not to the bit (at most"
               " %ld ulp); %llu bytes copied, %llu ring restarts%s\n", b,
               values, inexact, worst, (unsigned long long)bytes,
               (unsigned long long)wraps, hw ? " (hw)" : "");
    }
    return 1;
}

int main(int argc, char **argv)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    int hw = argc > 1 && !strcmp(argv[1], "hw");
    int prog, l, i, j, k, shown = 0;
    long checked = 0, differ = 0;
    uint32_t compiled, failed;
    double ms;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, hw ? D3D_DRIVER_TYPE_HARDWARE
                                    : D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        printf("FAIL: D3D11CreateDevice 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    if (!nv2a_vsh_gpu_init(dev, ctx)) {
        puts("FAIL: nv2a_vsh_gpu_init");
        return 1;
    }

    /* The epilogue (nv2a_vsh_test.c): the rcc beside a MAC op lands in R1. */
    {
        uint32_t i0[4] = {0}, i1[4] = {0}, i2[4] = {0};
        const float c58[4] = {320, -240, 1000, 0}, c59[4] = {320, 240, 0, 0};
        Nv2aVshOutput want;
        i0[1] = (1u << 21);
        src(i0, 0, V, 0, XYZW);
        i0[3] |= (0xFu << 12) | (1u << 11);
        i1[1] = (3u << 25) | (2u << 21) | (58u << 13);
        src(i1, 0, T, 12, XYZW); src(i1, 1, C, 0, XYZW); src(i1, 2, T, 12, WWWW);
        i1[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);
        i2[1] = (4u << 21) | (59u << 13);
        src(i2, 0, T, 12, XYZW); src(i2, 1, T, 1, XXXX); src(i2, 2, C, 0, XYZW);
        i2[3] |= (0xEu << 12) | (1u << 11) | 1u;
        nv2a_vsh_set_instruction(0, i0);
        nv2a_vsh_set_instruction(1, i1);
        nv2a_vsh_set_instruction(2, i2);
        nv2a_vsh_set_constant(58, c58);
        nv2a_vsh_set_constant(59, c59);
        memset(s_in, 0, sizeof s_in[0]);
        s_in[0][0][0] = 1.0f; s_in[0][0][1] = 0.5f;
        s_in[0][0][2] = 0.25f; s_in[0][0][3] = 2.0f;
        if (!nv2a_vsh_gpu_prepare(NULL)
            || !nv2a_vsh_gpu_run((const float (*)[NV2A_VSH_INPUTS][4])s_in, 1,
                                 s_got)) {
            puts("FAIL: epilogue did not run on the GPU");
            return 1;
        }
        nv2a_vsh_run((const float (*)[4])s_in[0], &want);
        if (nv2a_vsh_outputs_close(&want, &s_got[0]) >= 0
            || fabsf(s_got[0].pos[0] - 480) > 1e-3f) {
            print_diff("FAIL: epilogue", &want, &s_got[0]);
            return 1;
        }
    }

    if (!fetch_test(hw))
        return 1;

    for (prog = 0; prog < 1500; prog++) {
        int len = 1 + (int)(rnd() % 24), count = 1 + (int)(rnd() % 64);
        uint32_t w[4];

        for (i = 0; i < len; i++) {
            random_instruction(w, i == len - 1);
            if (i == 0 && rnd() % 3 == 0) {
                w[1] = (w[1] & ~(15u << 21) & ~(7u << 25)) | (13u << 21);
                w[2] = (w[2] & ~(3u << 26)) | (2u << 26);
            }
            nv2a_vsh_set_instruction((uint32_t)i, w);
        }
        nv2a_vsh_set_start_slot(1);
        nv2a_vsh_set_start_slot(0);
        for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
            float c[4];
            for (j = 0; j < 4; j++) c[j] = rndf();
            nv2a_vsh_set_constant((uint32_t)i, c);
        }
        for (l = 0; l < count; l++)
            for (i = 0; i < NV2A_VSH_INPUTS; i++)
                for (j = 0; j < 4; j++)
                    s_in[l][i][j] = rndf();

        if (!nv2a_vsh_gpu_prepare(NULL)) {
            printf("FAIL: program %d (%d instructions) did not compile\n",
                   prog, len);
            return 1;
        }
        if (!nv2a_vsh_gpu_run((const float (*)[NV2A_VSH_INPUTS][4])s_in,
                              (uint32_t)count, s_got)) {
            printf("FAIL: program %d did not run\n", prog);
            return 1;
        }
        /* VSH_TEST_SHOW=<n>: program n's HLSL, and its first vertex (or
         * VSH_TEST_VERTEX) step by step on the CPU ([VSTEP] on stderr). */
        if (getenv("VSH_TEST_SHOW") && atoi(getenv("VSH_TEST_SHOW")) == prog) {
            static char text[96 * 1024];
            uint32_t n;
            uint64_t h;
            const Nv2aVshIns *ins = nv2a_vsh_decoded(&n, &h);
            nv2a_vsh_hlsl(ins, n, text, sizeof text);
            fprintf(stderr, "%s\n", text);
            {
                Nv2aVshOutput want;
                int sv = getenv("VSH_TEST_VERTEX")
                         ? atoi(getenv("VSH_TEST_VERTEX")) % count : 0;
                nv2a_vsh_trace((const float (*)[4])s_in[sv]);
                nv2a_vsh_run((const float (*)[4])s_in[sv], &want);
                print_diff("shown vertex, cpu vs gpu", &want, &s_got[sv]);
                for (j = 0; j < 33; j++)
                    fprintf(stderr, "  %s cpu %g gpu %g\n",
                            nv2a_vsh_output_name(j), ((float *)&want)[j],
                            ((float *)&s_got[sv])[j]);
            }
        }
        for (l = 0, k = 0; l < count; l++) {
            Nv2aVshOutput want;
            nv2a_vsh_run((const float (*)[4])s_in[l], &want);
            checked++;
            if (nv2a_vsh_outputs_close(&want, &s_got[l]) >= 0) {
                char what[64];
                differ++;
                if (!k++ && shown++ < 25) {        /* one per program */
                    snprintf(what, sizeof what, "program %d (%d instructions)"
                             " vertex %d", prog, len, l);
                    print_diff(what, &want, &s_got[l]);
                }
            }
        }
    }
    nv2a_vsh_gpu_stats(&compiled, &failed, &ms);
    printf("%d programs (%u compiled, %.0f ms), %ld vertices, %ld differ"
           " (%.3f%%)\n", prog, compiled, ms, checked, differ,
           100.0 * (double)differ / (double)checked);
    if (failed || (hw ? differ * 200 > checked : differ > 0)) {
        puts("FAIL");
        return 1;
    }
    puts("ok");
    return 0;
}
