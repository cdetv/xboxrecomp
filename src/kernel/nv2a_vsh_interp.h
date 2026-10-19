/**
 * NV2A vertex program interpreter, for the software pushbuffer executor.
 *
 * The executor (nv2a_pb_exec.c) could only draw vertices that arrive already
 * in screen space. A title running vertex programs -- every 3D scene on the
 * Xbox -- sends model-space positions and lets the GPU transform them, so
 * those batches were counted and skipped. This runs the uploaded program on
 * the CPU, one vertex at a time (nv2a_vsh_run) or a group of them per
 * instruction (nv2a_vsh_run_lanes), and hands back the outputs the
 * rasteriser needs.
 *
 * Programs and constants arrive through the pushbuffer exactly as the GPU
 * would take them (SET_TRANSFORM_PROGRAM_LOAD / _PROGRAM, _CONSTANT_LOAD /
 * _CONSTANT), so the executor forwards those methods here and nothing about
 * a title's D3D state has to be known.
 *
 * Instruction encoding: the field table at the top of nv2a_vsh_interp.c.
 * Portable C; no
 * graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_INTERP_H
#define XBOXRECOMP_NV2A_VSH_INTERP_H

#include <stdint.h>

#define NV2A_VSH_SLOTS      136     /* program memory, 128-bit instructions */
#define NV2A_VSH_CONSTANTS  192     /* c[0..191]; D3D's c0 is c[96]          */
#define NV2A_VSH_INPUTS     16      /* v0..v15                                */

/* What one vertex comes out as. Position is whatever the program wrote to
 * oPos: on Xbox D3D that is already screen space -- x, y in pixels, z in the
 * depth buffer's range -- because the runtime appends the viewport transform
 * and the divide by w to every shader, with w left as the clip-space w. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
    /* oPts.x: the point size in pixels a program writes, which a POINTS
     * batch draws its sprites at while SET_POINT_PARAMS_ENABLE is on. Last,
     * so the D3D11 input layout's offsets above it stay as they were. */
    float pts;
} Nv2aVshOutput;

void nv2a_vsh_set_load_slot(uint32_t slot);           /* _PROGRAM_LOAD      */
void nv2a_vsh_program_word(uint32_t word);            /* _PROGRAM(i)        */
void nv2a_vsh_set_start_slot(uint32_t slot);          /* _PROGRAM_START     */
void nv2a_vsh_set_constant_load(uint32_t index);      /* _CONSTANT_LOAD     */
void nv2a_vsh_constant_word(uint32_t word);           /* _CONSTANT(i)       */
void nv2a_vsh_set_cxt_write(uint32_t enable);         /* _CXT_WRITE_EN      */
int  nv2a_vsh_cxt_write(void);   /* programs may write c[]: no vertex cache */

/* Run the program from the start slot on one vertex. Inputs are the ten
 * attribute values as floats. Returns 0 if there is no program to run (no
 * FINAL flag within program memory), which the caller treats as "cannot
 * transform this batch" rather than drawing garbage. */
int  nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out);
/* Disassemble the current program and run one vertex through it printing
 * every instruction's reads and writes ([VSTEP]); for RECOMP_PIXEL_PROBE. */
void nv2a_vsh_trace(const float in[NV2A_VSH_INPUTS][4]);

/* The same program on up to NV2A_VSH_LANES vertices at once, same results
 * bit for bit, much faster. in[l] and out[l] are vertex l's; only the inputs
 * nv2a_vsh_prepare reports as used are read. Not for programs that may
 * write constants (nv2a_vsh_cxt_write). Safe on several threads at once
 * once nv2a_vsh_prepare has run on one. Returns 0 as nv2a_vsh_run does. */
#define NV2A_VSH_LANES 32
int  nv2a_vsh_prepare(uint32_t *inputs_used);  /* decode; 0 = no program */
int  nv2a_vsh_run_lanes(const float (*in)[NV2A_VSH_INPUTS][4],
                        Nv2aVshOutput *const *out, int count);

/* The current program as decoded once from the start slot to its FINAL,
 * for the HLSL translator (nv2a_vsh_hlsl.c). Fields as in the encoding
 * table at the top of nv2a_vsh_interp.c; `need` says which of the sources
 * A, B, C (bits 0-2) the ops read. */
typedef struct {
    uint8_t mux, reg, neg, sw[4];
} Nv2aVshSrc;

typedef struct {
    Nv2aVshSrc src[3];
    uint8_t mac, ilu, mac_mask, ilu_mask, tdst, omask;
    uint8_t out_o, out_ilu, rel, final, need;
    uint16_t oaddr, cidx, iidx;
} Nv2aVshIns;

/* NULL if there is no program to run (no FINAL); else the instructions,
 * their number and a 64-bit hash of the instruction words (by content:
 * titles reload different programs into the same slot). */
const Nv2aVshIns *nv2a_vsh_decoded(uint32_t *count, uint64_t *hash);

/* The range of constants written since the last call, inclusive: 1 and
 * lo/hi set if any, else 0. For uploading them to the GPU. */
int nv2a_vsh_constants_take_dirty(uint32_t *lo, uint32_t *hi);

/* Direct access, for the self-test. */
void nv2a_vsh_set_constant(uint32_t index, const float v[4]);
const float *nv2a_vsh_constant(uint32_t index);   /* for traces */
/* One component of a constant, raw bits: the viewport methods write c[58]
 * (scale) and c[59] (offset) a word at a time rather than via _CONSTANT. */
void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word);
void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4]);

#endif
