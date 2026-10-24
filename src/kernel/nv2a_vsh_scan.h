/**
 * Finding vertex programs in guest memory, for shader pre-compilation.
 *
 * A title's programs mostly sit in memory whole long before they are
 * drawn: inside ready-made push buffers loaded with a level, and as Xbox
 * shader binaries (Conker 2026-10-23: 389 of 458 programs in 219 push
 * buffers that came with the level files). nv2a_vsh_scan walks a range
 * and reports each program it finds in either form:
 *
 *   NV2A_VSH_SCAN_PB   TRANSFORM_PROGRAM (method 0x0B00, increasing, 4..32
 *                      words, a multiple of 4) chunks back to back, up to
 *                      the instruction with the final bit -- what D3D
 *                      writes after a _PROGRAM_LOAD.
 *   NV2A_VSH_SCAN_BIN  a shader binary: header word 0x2078 | count << 16,
 *                      then count instructions, the final bit on the last.
 *
 * Every instruction must have dword 0 zero (the NV2A ignores it and D3D
 * writes 0) and only the last one the final bit, which keeps vertex data
 * and other words from passing as programs. Pure: reads memory only, so it
 * can run on any thread while the title writes (a torn read is at worst a
 * program nobody draws).
 */
#ifndef XBOXRECOMP_NV2A_VSH_SCAN_H
#define XBOXRECOMP_NV2A_VSH_SCAN_H

#include <stdint.h>

#define NV2A_VSH_SCAN_PB   1
#define NV2A_VSH_SCAN_BIN  2
#define NV2A_VSH_SCAN_MAX_INS 136

/* One program: its first word's address, the form, and count instructions
 * of 4 words each (valid only during the call). */
typedef void (*Nv2aVshScanFn)(void *ctx, uint32_t va, int kind,
                              const uint32_t *ins, uint32_t count);

/* Scans [lo, hi) (4-byte steps; host address of va = mem + va) and calls
 * fn for each program. Returns the number found. */
uint32_t nv2a_vsh_scan(const uint8_t *mem, uint32_t lo, uint32_t hi,
                       Nv2aVshScanFn fn, void *ctx);

/* The hash nv2a_vsh_decoded gives the same program once loaded: FNV-1a
 * over dwords 1-3 of each instruction. */
uint64_t nv2a_vsh_scan_hash(const uint32_t *ins, uint32_t count);

/* ---- at run time (nv2a_vsh_prescan.c, Windows) ---------------------------
 *
 * RECOMP_VSH_SCAN=1: nv2a_vsh_prescan_request (from the title's glue, after
 * a level load, say) scans all of guest RAM through the contiguous window on
 * a thread of its own and prints a [VSHSCAN] line: programs found, by form,
 * distinct, new since the earlier scans, already drawn, the time it took.
 * Each program then drawn for the first time prints whether a scan had
 * found it, and which. Count only for now: nothing is compiled. Unset: the
 * request does nothing. */
void nv2a_vsh_prescan_request(const char *why);

#endif
