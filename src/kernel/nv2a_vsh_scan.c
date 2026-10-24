/*
 * Finding vertex programs in guest memory (nv2a_vsh_scan.h).
 */
#include "nv2a_vsh_scan.h"

#include <string.h>

#define FINAL(ins) ((ins)[3] & 1u)

static uint32_t rd(const uint8_t *mem, uint32_t va)
{
    uint32_t w;
    memcpy(&w, mem + va, 4);
    return w;
}

/* A TRANSFORM_PROGRAM chunk header: words in it, or 0. */
static uint32_t pb_chunk(uint32_t w)
{
    uint32_t count = (w >> 18) & 0x7FFu;
    if ((w & 0xE0030003u) != 0u || (w & 0x1FFCu) != 0x0B00u)
        return 0;
    if (count < 4 || count > 32 || (count & 3u))
        return 0;
    return count;
}

/* Instructions ins[0..n): dword 0 zero in each, final bit on none. */
static int plain(const uint32_t *ins, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (ins[4 * i] != 0u || FINAL(&ins[4 * i]))
            return 0;
    return 1;
}

/* Chunks from va on: the program's instructions into ins, its count, and
 * the address past the chunk that ended it in *end; 0 if not a program. */
static uint32_t scan_pb(const uint8_t *mem, uint32_t va, uint32_t hi,
                        uint32_t *ins, uint32_t *end)
{
    uint32_t n = 0;

    for (;;) {
        uint32_t words, k;
        if (va + 4 > hi || !(words = pb_chunk(rd(mem, va))))
            return 0;
        va += 4;
        if (va + 4 * words > hi || n + words / 4 > NV2A_VSH_SCAN_MAX_INS)
            return 0;
        for (k = 0; k < words; k += 4, va += 16) {
            uint32_t *I = &ins[4 * n];
            memcpy(I, mem + va, 16);
            if (I[0] != 0u)
                return 0;
            n++;
            if (FINAL(I)) {
                *end = va + 16;
                return n;
            }
        }
    }
}

static uint32_t scan_bin(const uint8_t *mem, uint32_t va, uint32_t hi,
                         uint32_t *ins)
{
    uint32_t w = rd(mem, va), n = w >> 16;

    if ((w & 0xFFFFu) != 0x2078u || n == 0 || n > NV2A_VSH_SCAN_MAX_INS
        || va + 4 + 16 * n > hi)
        return 0;
    memcpy(ins, mem + va + 4, 16 * n);
    if (!plain(ins, n - 1) || ins[4 * (n - 1)] != 0u
        || !FINAL(&ins[4 * (n - 1)]))
        return 0;
    return n;
}

uint32_t nv2a_vsh_scan(const uint8_t *mem, uint32_t lo, uint32_t hi,
                       Nv2aVshScanFn fn, void *ctx)
{
    uint32_t ins[4 * NV2A_VSH_SCAN_MAX_INS];
    uint32_t va, found = 0;

    lo = (lo + 3u) & ~3u;
    for (va = lo; va + 4 <= hi; va += 4) {
        uint32_t w = rd(mem, va), n, end;
        if (pb_chunk(w)) {
            n = scan_pb(mem, va, hi, ins, &end);
            if (n) {
                fn(ctx, va, NV2A_VSH_SCAN_PB, ins, n);
                found++;
                va = end - 4;                /* past it: no suffixes */
            }
        } else if ((w & 0xFFFFu) == 0x2078u) {
            n = scan_bin(mem, va, hi, ins);
            if (n) {
                fn(ctx, va, NV2A_VSH_SCAN_BIN, ins, n);
                found++;
                va += 16 * n;                /* + 4 by the loop */
            }
        }
    }
    return found;
}

uint64_t nv2a_vsh_scan_hash(const uint32_t *ins, uint32_t count)
{
    uint64_t h = 14695981039346656037ull;
    uint32_t i, k;

    for (i = 0; i < count; i++)
        for (k = 1; k < 4; k++)
            h = (h ^ ins[4 * i + k]) * 1099511628211ull;
    return h;
}
