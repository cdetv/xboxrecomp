/* nv2a_vsh_run_lanes against nv2a_vsh_run: random programs (every MAC and
 * ILU op, random sources, swizzles, negates, masks, outputs, relative
 * constants after an ARL), random constants and inputs, 1..NV2A_VSH_LANES
 * vertices at a time. Every vertex must come out in the same bits. */
#include "nv2a_vsh_interp.h"
#include <stdio.h>
#include <string.h>

static uint32_t s_rng = 12345u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    /* Mostly ordinary values, sometimes zero or a whole number, so the
     * compares, floors and the rcp/log zero cases are reached. */
    uint32_t k = rnd() % 16;
    if (k == 0) return 0.0f;
    if (k == 1) return (float)((int)(rnd() % 7) - 3);
    return ((float)(rnd() % 20001) - 10000.0f) / 1000.0f;
}

static void random_instruction(uint32_t w[4], int final)
{
    w[0] = 0;
    w[1] = rnd() & 0x0FFFFFFFu;     /* ilu, mac, const, input, A neg/swizzle */
    w[1] = (w[1] & ~(0xFFu << 13)) | ((rnd() % 200u) << 13);
    w[2] = rnd();
    w[3] = rnd() & ~1u;
    /* Mostly to o[0..12], so what the program computes reaches the output
     * compared; pure random addresses would mostly miss them. */
    w[3] = (w[3] & ~(0xFFu << 3)) | ((rnd() % 13u) << 3);
    if (rnd() % 8)
        w[3] |= 1u << 11;
    if ((w[1] >> 21 & 15) >= 14)    /* ops the hardware does not have */
        w[1] &= ~(15u << 21);
    if (!(w[1] >> 21 & 15) && !(w[1] >> 25 & 7))
        w[1] |= 4u << 21;           /* at least one unit: mad */
    if (final)
        w[3] |= 1u;
}

int main(void)
{
    static float in[NV2A_VSH_LANES][NV2A_VSH_INPUTS][4];
    Nv2aVshOutput got[NV2A_VSH_LANES], want;
    Nv2aVshOutput *outp[NV2A_VSH_LANES];
    int prog, l, i, j, bad = 0;
    long checked = 0;

    for (l = 0; l < NV2A_VSH_LANES; l++)
        outp[l] = &got[l];
    for (prog = 0; prog < 3000 && bad < 5; prog++) {
        int len = 1 + (int)(rnd() % 24), count = 1 + (int)(rnd() % NV2A_VSH_LANES);
        uint32_t w[4];

        for (i = 0; i < len; i++) {
            random_instruction(w, i == len - 1);
            /* Now and then an ARL into a0 first, so a0.x-relative
             * reads see more than 0. */
            if (i == 0 && rnd() % 3 == 0) {
                w[1] = (w[1] & ~(15u << 21) & ~(7u << 25)) | (13u << 21);
                w[2] = (w[2] & ~(3u << 26)) | (2u << 26);   /* A = input */
            }
            nv2a_vsh_set_instruction((uint32_t)i, w);
        }
        nv2a_vsh_set_start_slot(1);       /* new start slot: decode again */
        nv2a_vsh_set_start_slot(0);
        for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
            float c[4];
            for (j = 0; j < 4; j++) c[j] = rndf();
            nv2a_vsh_set_constant((uint32_t)i, c);
        }
        for (l = 0; l < count; l++)
            for (i = 0; i < NV2A_VSH_INPUTS; i++)
                for (j = 0; j < 4; j++)
                    in[l][i][j] = rndf();

        if (!nv2a_vsh_run_lanes((const float (*)[NV2A_VSH_INPUTS][4])in,
                                outp, count)) {
            printf("FAIL: program %d did not run\n", prog);
            return 1;
        }
        for (l = 0; l < count; l++) {
            nv2a_vsh_run((const float (*)[4])in[l], &want);
            checked++;
            if (memcmp(&want, &got[l], sizeof want)) {
                const float *p = (const float *)&want, *q = (const float *)&got[l];
                printf("FAIL: program %d (%d instructions) vertex %d:", prog,
                       len, l);
                for (j = 0; j < (int)(sizeof want / sizeof(float)); j++)
                    if (memcmp(&p[j], &q[j], sizeof(float)))
                        printf(" [%d] %g vs %g", j, p[j], q[j]);
                putchar('\n');
                bad++;
                break;
            }
        }
    }
    if (bad)
        return 1;
    printf("ok: %d programs, %ld vertices, same bits\n", prog, checked);
    return 0;
}
