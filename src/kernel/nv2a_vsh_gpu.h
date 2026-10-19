/**
 * NV2A vertex programs on the GPU (D3D11): the translated programs
 * (nv2a_vsh_hlsl.c) compiled once each, the constants uploaded when they
 * change, and a stream-out run that hands the GPU's results back, for
 * checking them against the CPU interpreter (RECOMP_VSH_GPU_CHECK, and
 * tests/nv2a_vsh_gpu).
 *
 * The plan (game repo, docs/handoff-notes.md "vertex programs on the GPU"):
 * step 1, this -- translate, compile, check; step 2 attribute fetch on the
 * GPU; step 3 drawing with them.
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

#endif
