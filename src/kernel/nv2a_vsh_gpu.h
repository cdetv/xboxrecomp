/**
 * NV2A vertex programs on the GPU (D3D11): the translated programs
 * (nv2a_vsh_hlsl.c) compiled once each, the constants uploaded when they
 * change, and a stream-out run that hands the GPU's results back, for
 * checking them against the CPU interpreter (RECOMP_VSH_GPU_CHECK, and
 * tests/nv2a_vsh_gpu).
 *
 * The plan (game repo, docs/handoff-notes.md "vertex programs on the GPU"):
 * step 1, this -- translate, compile, check; step 2 attribute fetch on the
 * GPU (nv2a_vsh_gpu_inputs, checked with nv2a_vsh_gpu_fetch); step 3
 * drawing with them.
 *
 * Windows only. Not thread-safe: the executor thread (or one test thread).
 */
#ifndef XBOXRECOMP_NV2A_VSH_GPU_H
#define XBOXRECOMP_NV2A_VSH_GPU_H

#include <stdint.h>
#include "nv2a_vsh_interp.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

/* 0 if the buffers cannot be made; everything below then returns 0. */
int nv2a_vsh_gpu_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx);

/* The current program (nv2a_vsh_decoded), translated and compiled the first
 * time it is seen; *is_new (may be NULL) says that was now. 0: no program,
 * or it did not translate or compile (logged once per program). */
int nv2a_vsh_gpu_prepare(int *is_new);

/* Uploads the constants written since the last upload. 0 on failure. */
int nv2a_vsh_gpu_constants(void);

/* Runs the prepared program on count vertices' inputs (absent attributes
 * as the CPU fetch leaves them, 0,0,0,1) through stream-out and reads the
 * results back into out: a GPU wait, for checks only. Leaves no geometry
 * shader or stream-out target bound and no pixel shader; the caller sets
 * its own state afterwards. 0 on failure. */
int nv2a_vsh_gpu_run(const float (*in)[NV2A_VSH_INPUTS][4], uint32_t count,
                     Nv2aVshOutput *out);

/* Programs compiled, programs that failed, total compile time. */
void nv2a_vsh_gpu_stats(uint32_t *compiled, uint32_t *failed, double *ms);

/* ---- step 2: the inputs read on the GPU -------------------------------- */

/* One vertex attribute array for the GPU fetch: element i at src + i *
 * stride, NV097 `type` with `size` components. src NULL, a size or stride of
 * 0 or a type nv2a_attr_bytes does not know = absent (0,0,0,1), as in
 * pb_fetch_attr; so are indices past
 * `last` (an INLINE_ARRAY's end; UINT32_MAX for an array in memory). */
typedef struct {
    const uint8_t *src;
    uint32_t type, size, stride, last;
} Nv2aVshArray;

/* Copies indices lo..hi of every array in `used` (bit k = input k) into the
 * input ring and binds it and the fetch constants to the vertex shader
 * (nv2a_vsh_hlsl.h, t0 and b4). Arrays that share bytes, as interleaved
 * vertices do, are copied once. They stay bound until the next call. 0 if
 * the batch's bytes do not fit in the ring. */
int nv2a_vsh_gpu_inputs(const Nv2aVshArray a[NV2A_VSH_INPUTS], uint32_t used,
                        uint32_t lo, uint32_t hi);

/* After nv2a_vsh_gpu_inputs: the 16 inputs of each of count indices as the
 * GPU reads them (vs_fetch through stream-out, an indexed point list so
 * SV_VertexID is the index), into out. A GPU wait, for checks. Same state
 * rules as nv2a_vsh_gpu_run, and the index buffer is left changed too. */
int nv2a_vsh_gpu_fetch(const uint32_t *idx, uint32_t count,
                       float (*out)[NV2A_VSH_INPUTS][4]);

/* Since start: batches through nv2a_vsh_gpu_inputs, bytes copied, times the
 * ring was started again from the top (each one a D3D11 DISCARD). */
void nv2a_vsh_gpu_input_stats(uint64_t *batches, uint64_t *bytes,
                              uint64_t *wraps);

#endif
