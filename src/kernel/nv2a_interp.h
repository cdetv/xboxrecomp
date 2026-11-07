/**
 * Frame interpolation (RECOMP_FRAME_INTERP=<n>, 2-4; unset or 1 = off): the
 * title keeps its own frame rate and timing, and between two of its frames
 * the executor draws n - 1 more pictures, each the newer frame drawn again
 * with its vertex program constants (transforms, bones) blended toward the
 * frame before. See nv2a_interp.c.
 *
 * Internal to src/kernel: the executor (nv2a_pb_exec.c, nv2a_pb_scan.c) and
 * the back end it hands the pictures to.
 */
#ifndef NV2A_INTERP_H
#define NV2A_INTERP_H

#include <stdint.h>
#include "nv2a_pb_state.h"

/* Pictures per title frame: 1 when interpolation is off. */
int  nv2a_interp_n(void);
/* A recorded frame is being drawn again: no side effects the title sees. */
int  nv2a_interp_replaying(void);
/* Counts replays started: what a back end keeps per frame (by the flip
 * number) it keeps per replay too -- a replay runs with its frame's flip
 * number, after that frame (or another replay of it) already ran. */
unsigned nv2a_interp_replay_id(void);

/* The stream, as the executor runs it (not while replaying). */
void nv2a_interp_rec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_interp_rec_words(uint32_t method, const uint32_t *p, uint32_t n);
void nv2a_interp_rec_flush(void);

/* Before each back-end draw (draw_primitive): records the draw, or, while
 * replaying, sets the blended constants for it. */
void nv2a_interp_draw(void);
/* After it (draw_primitive): while replaying, a second draw of the same
 * vertices with the partner frame's index list, when the first armed one. */
void nv2a_interp_after_draw(const Nv2aPbBackend *be);

/* NV_HOST_FX_INTERP_CUT in the stream: the frame is a camera cut. */
void nv2a_interp_note_cut(void);
/* NV_HOST_FX_INTERP_HOLD_ON / _OFF met in the stream (1 / 0). */
void nv2a_interp_hold(int on);

/* NV097_FLIP_STALL, after the wait and before the back end presents: draws
 * and presents the in-between pictures (before), then the next frame's
 * recording starts (after). */
void nv2a_interp_flip_before(const Nv2aPbBackend *be);
void nv2a_interp_flip_after(void);

/* The executor's places to wait or to look up between commands: presents a
 * picture whose time has come. Cheap when none is due. */
void nv2a_interp_tick(void);
/* Milliseconds until the next picture is due, or -1 when none is waiting. */
int  nv2a_interp_due_ms(void);

#endif /* NV2A_INTERP_H */
