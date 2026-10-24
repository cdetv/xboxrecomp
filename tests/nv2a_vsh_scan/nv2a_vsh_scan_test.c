/*
 * nv2a_vsh_scan: programs found in a made-up memory image, and look-alikes
 * that must not be.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "nv2a_vsh_scan.h"

static uint32_t mem[4096];
static uint32_t at;                       /* next word */

typedef struct { uint32_t va; int kind; uint32_t count; uint64_t hash; } Found;
static Found found[16];
static int nfound;

static void on_found(void *ctx, uint32_t va, int kind, const uint32_t *ins,
                     uint32_t count)
{
    (void)ctx;
    if (nfound < 16) {
        found[nfound].va = va;
        found[nfound].kind = kind;
        found[nfound].count = count;
        found[nfound].hash = nv2a_vsh_scan_hash(ins, count);
        nfound++;
    }
}

static void put(uint32_t w) { mem[at++] = w; }

/* One instruction: dword 0 as given (0 for a real one), some bits, final. */
static void ins(uint32_t d0, uint32_t seed, int final, uint32_t *copy)
{
    uint32_t w[4] = { d0, 0x0020001Bu + seed, 0x0836106Cu ^ seed,
                      (0x2070F800u + (seed << 4)) | (final ? 1u : 0u) };
    if (copy)
        memcpy(copy, w, sizeof w);
    put(w[0]); put(w[1]); put(w[2]); put(w[3]);
}

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    uint32_t pb_ins[10 * 4], bin_ins[3 * 4], pb_va, bin_va, i, n;

    memset(mem, 0xAB, sizeof mem);
    at = 4;

    /* A push buffer upload: _PROGRAM_LOAD 0, then 32 + 8 words. */
    put(0x00041E9Cu); put(0);
    pb_va = at * 4;
    put((32u << 18) | 0x0B00u);
    for (i = 0; i < 8; i++)
        ins(0, i, 0, &pb_ins[4 * i]);
    put((8u << 18) | 0x0B00u);
    ins(0, 8, 0, &pb_ins[32]);
    ins(0, 9, 1, &pb_ins[36]);

    /* A shader binary of 3. */
    bin_va = at * 4;
    put(0x00032078u);
    ins(0, 20, 0, &bin_ins[0]);
    ins(0, 21, 0, &bin_ins[4]);
    ins(0, 22, 1, &bin_ins[8]);

    /* Look-alikes: a chunk whose instruction has dword 0 set; a binary
     * with the final bit on its second of three; a chunk with no final bit
     * and no chunk after it; a binary with count 0. */
    put((4u << 18) | 0x0B00u);
    ins(0x12345678u, 30, 1, NULL);
    put(0x00032078u);
    ins(0, 31, 0, NULL); ins(0, 32, 1, NULL); ins(0, 33, 1, NULL);
    put((4u << 18) | 0x0B00u);
    ins(0, 34, 0, NULL);
    put(0x0000ABCDu);
    put(0x00002078u);

    n = nv2a_vsh_scan((const uint8_t *)mem, 0, at * 4, on_found, NULL);

    check(n == 2 && nfound == 2, "two programs found, look-alikes not");
    check(nfound >= 1 && found[0].va == pb_va && found[0].kind == NV2A_VSH_SCAN_PB
          && found[0].count == 10
          && found[0].hash == nv2a_vsh_scan_hash(pb_ins, 10),
          "push buffer program: 10 instructions over two chunks");
    check(nfound >= 2 && found[1].va == bin_va
          && found[1].kind == NV2A_VSH_SCAN_BIN && found[1].count == 3
          && found[1].hash == nv2a_vsh_scan_hash(bin_ins, 3),
          "shader binary: 3 instructions");

    /* A range that cuts the push buffer program short finds nothing of it. */
    nfound = 0;
    n = nv2a_vsh_scan((const uint8_t *)mem, pb_va, pb_va + 100, on_found, NULL);
    check(n == 0, "program cut by the range end: not found");

    return failures ? 1 : 0;
}
