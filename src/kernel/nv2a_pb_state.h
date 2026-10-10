/**
 * The pushbuffer executor's shared state, and the interface to whatever draws.
 *
 * nv2a_pb_exec.c is the front end: it decodes the NV097 methods the title
 * writes, keeps the GPU state they set (surfaces, blend, depth, stencil,
 * combiners, texture stages, vertex arrays) and assembles each BEGIN/END batch
 * into one form, whichever way its vertices arrived. What happens to a batch
 * -- and to a clear, and to a finished frame at the flip -- is a back end's
 * job, behind Nv2aPbBackend.
 *
 * nv2a_draw_sw.c is the software back end: the CPU rasteriser that draws into
 * guest memory and the window that shows it. It is the reference every other
 * back end is checked against, frame for frame.
 *
 * Internal to src/kernel: nothing outside the executor includes this.
 */
#ifndef NV2A_PB_STATE_H
#define NV2A_PB_STATE_H

#include <stdint.h>
#include "nv2a_combiner.h"
/* tex_size_from_format below needs the format tables. */
#include "../d3d/d3d8_swizzle.h"

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
/* One BEGIN/END batch. 4096 used to be the cap and indices past it were
 * dropped in silence: Conker's front-end bar scene draws a triangle strip
 * of 8540 in one batch, and lost more than half of it. */
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
} Texture;

typedef struct Nv2aPbState {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    uint32_t   idx_dropped;             /* indices past NV_MAX_INDICES */
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    /* Immediate mode: every attribute's current value, as the SET_VERTEX
     * methods leave it. Writing attribute 0 (position) emits a vertex. */
    float      imm_attr[NV_VERTEX_ATTRS][4];
    uint16_t   imm_used;                /* attributes written since BEGIN */
    uint16_t   imm_set;                 /* attributes ever written: their
                                         * imm_attr value is the constant an
                                         * array-less attribute takes */
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    /* ...and the shape of that surface. A title also draws into small
     * render targets (Burnout 3: 128x128 shadow/reflection maps, pitch 512),
     * often after the main scene, so "last surface drawn" alone picked one of
     * those and read it with the framebuffer's pitch: horizontal noise. The
     * biggest surface drawn since the last flip is the one being presented. */
    uint32_t drawn_pitch, drawn_x, drawn_y, drawn_w, drawn_h, drawn_bpp;
    int drawn_stale;      /* set at a flip: the next draw starts a new frame */
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color;
    uint32_t color_mask;        /* SET_COLOR_MASK: A<<24 R<<16 G<<8 B */
    /* Visibility tests (D3D's Begin/EndVisibilityTest): pixels that pass the
     * depth test while counting is on, reported by GET_REPORT. */
    uint32_t zpass_enable, zpass_count, reports;
    uint32_t blend_pairs[16];   /* sfactor<<16 | dfactor seen, for the report */
    int blend_npairs;
    Texture  texs[4];                   /* one per texture stage */
    /* Register combiners and the per-pixel state around them. rc_seen: the
     * title has programmed the combiners, so they decide every pixel's
     * colour; until then the old "stage 0 texel * diffuse" stands in. */
    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;           /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    /* Vertex programs (nv2a_vsh_interp.c) and the depth buffer, which
     * together are what a 3D scene needs and a 2D one never used. */
    uint32_t xf_mode;                   /* TRANSFORM_EXECUTION_MODE: 2 = program */
    uint32_t batches_program, verts_program, tris_behind;
    uint32_t verts_cached;                  /* program runs saved, vertex cache */
    /* Where program-path triangles go, so "nothing drew" has a reason. */
    uint32_t xf_degenerate, xf_offscreen, xf_drawn;
    uint64_t xf_depth_fail, xf_pixels;
    float xf_min[3], xf_max[3];
    uint32_t xf_fmt[16];  /* attribute formats seen: type | size<<4 | slot<<8 */
    int xf_nfmt, xf_seeded;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t zeta_offset, zstencil_clear;
    /* Stencil, as the methods give it: GL enums for the function and ops,
     * STENCIL_MASK the write mask and FUNC_MASK the read mask. */
    uint32_t stencil_test, stencil_func, stencil_ref, stencil_rmask;
    uint32_t stencil_wmask, stencil_op_fail, stencil_op_zfail, stencil_op_zpass;
} Nv2aPbState;
extern Nv2aPbState g_pb;

/* Frame trace (RECOMP_FRAME_TRACE): 0 idle, 2 tracing this frame, 3 done. */
extern int g_pb_ftrace;

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static inline int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

/* nv2a_pb_exec.c: where a DMA-object offset lives, and the guard that keeps
 * surface writes off the title's own image. */
uint32_t pb_dma_resolve(uint32_t offset);
int pb_surface_hits_image(uint32_t base, uint32_t bytes);
int pb_surface_write_refused(uint32_t base, uint32_t bytes, const char *what);

/* nv2a_draw_sw.c: vertex attribute reads, which the front end's statistics
 * and diagnostics share with the rasteriser; and whether this is the frame
 * the pixel probe (RECOMP_PROBE) is watching. */
int pb_fetch_attr(const VertexAttr *a, uint32_t index, float out[4]);
int pb_fetch_texcoord(uint32_t index, float out[2]);
int pb_probe_frame(void);

/* What a back end does. The front end has already updated g_pb when each of
 * these is called, and g_pb is all a back end reads. */
typedef struct {
    const char *name;
    /* NV097_CLEAR_SURFACE: param bit 0 depth, bit 1 stencil, 0xF0 colour,
     * over the current clip of the current surfaces. */
    void (*clear)(uint32_t param);
    /* One assembled batch: g_pb.prim, g_pb.idx[0..idx_count) and the vertex
     * arrays in g_pb.attr (or g_pb.inline_buf when g_pb.inline_active). */
    void (*draw)(void);
    /* NV097_FLIP_STALL: the title has finished a frame; show it. */
    void (*present)(void);
    /* nv2a_pb_exec_report: the back end's own lines of the periodic report. */
    void (*report)(void);
} Nv2aPbBackend;

extern const Nv2aPbBackend nv2a_pb_backend_sw;

#endif /* NV2A_PB_STATE_H */
