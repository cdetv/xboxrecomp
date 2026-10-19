/**
 * NV2A vertex programs as HLSL, so the GPU can run them (RECOMP_VSH_GPU).
 *
 * The CPU interpreter (nv2a_vsh_interp.c) stays the reference: the text
 * this writes is run_program, instruction for instruction -- the same
 * register starting values, the same "both units read before either
 * writes" order, the ILU-to-R1 rule of a paired instruction, a0-relative
 * constant reads with out-of-range indices reading c[0], unwritten outputs
 * as the CPU leaves them and the colour clamps at the end. Where GPU maths
 * cannot give the same bits (rsq, exp2, log2, a divide) it gives the same
 * value to a few units in the last place; nv2a_vsh_outputs_close says how
 * near counts as the same.
 *
 * Portable C; the D3D11 side is nv2a_vsh_gpu.c.
 */
#ifndef XBOXRECOMP_NV2A_VSH_HLSL_H
#define XBOXRECOMP_NV2A_VSH_HLSL_H

#include <stddef.h>
#include "nv2a_vsh_interp.h"

/* The constants' register: cbuffer VshC : register(b3) { float4 vc[193]; },
 * c[0..191] and then one row that must stay all zero (the temps' start). */
#define NV2A_VSH_HLSL_CBUFFER 3
#define NV2A_VSH_HLSL_CROWS   (NV2A_VSH_CONSTANTS + 1)

/* Writes into buf (cap bytes, NUL-terminated) the shared definitions -- the
 * VshOut struct (Nv2aVshOutput's fields), the constants' cbuffer, helpers --
 * and `VshOut vsh_prog(float4 v[16])`, the program. Then the stream-out
 * entry point vs_check (inputs ATTR0..15, outputs TEXCOORD0..8 in
 * Nv2aVshOutput's order and layout, 33 floats). Returns the length, or 0 if
 * the program cannot be translated (count 0) or buf is too small. */
size_t nv2a_vsh_hlsl(const Nv2aVshIns *ins, uint32_t count, char *buf,
                     size_t cap);

/* Whether a GPU result b matches the interpreter's a, field by field (33
 * floats): both NaN, or the same infinity, or within a relative 1e-4 plus
 * an absolute 1e-3. Returns -1 when they match, else the first field that
 * does not (an index into the floats of Nv2aVshOutput). */
int nv2a_vsh_outputs_close(const Nv2aVshOutput *a, const Nv2aVshOutput *b);

/* A field index's name, for logs: "pos.x", "d0.w", "t2.y", "pts". */
const char *nv2a_vsh_output_name(int field);

#endif
