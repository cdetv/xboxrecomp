/**
 * The software back end of the pushbuffer executor: a CPU rasteriser that
 * draws each batch straight into guest memory, and the window that shows the
 * result.
 *
 * nv2a_pb_exec.c decodes the methods and keeps the state (nv2a_pb_state.h);
 * this draws with it. It was one file with the front end until the D3D11 back
 * end needed somewhere to plug in, and it stays the reference that back end
 * is checked against, frame for frame.
 *
 * Batches that run a vertex program go through the title's own program
 * (nv2a_vsh_interp.c), and pixels through the register combiners
 * (nv2a_combiner.c), with depth, stencil, blend, alpha test and fog on top.
 * RECOMP_RASTER_TEST draws one known triangle after every clear, which
 * separates "the pixel path is broken" from "the title has not given us any
 * vertices". RECOMP_FB_DUMP=<prefix> writes the surface to <prefix>NNN.bmp, so
 * the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#include "nv2a_pb_state.h"
#if defined(_WIN32)
#include <windows.h>
#define NV_TLS __declspec(thread)
#else
#define NV_TLS __thread
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

/* Repeat dumps are numbered from well past the first-use sequence, so a
 * listing sorts them after the textures they came from and no first-use file
 * is ever overwritten by one. */
#define TEX_DUMP_SEQ_BASE 1000u
#define TEX_DUMP_SEQ_MAX  40u

static void note_texture_use(void)
{
    int i;

    if (!g_pb.texs[0].valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == g_pb.texs[0].offset
         && s_tex_use[i].color  == g_pb.texs[0].color) {
            s_tex_use[i].batches++;
            /* Dump a surface that is redrawn, every Nth time it is bound.
             *
             * First use alone cannot tell a decode error that is wrong in
             * every frame from one that accumulates across them. A block
             * transform that is wrong is wrong on its own, in the keyframe
             * as much as anywhere; motion compensation that is wrong starts
             * from a clean keyframe and smears further with each predicted
             * frame after it. In a single frame the two look identical, and
             * in a sequence they look nothing alike -- so the sequence is
             * what has to be captured.
             *
             * It belongs on this side of the return: a video surface keeps
             * one address for the whole film, so after the first frame it is
             * only ever found here, and the first-use dump below never fires
             * for it again. RECOMP_TEX_DUMP_EVERY=<n> sets the interval, and
             * RECOMP_TEX_DUMP still names the files. */
            {
                static int every = -1;
                static unsigned binds, seq;
                if (every < 0) {
                    const char *e = getenv("RECOMP_TEX_DUMP_EVERY");
                    every = e ? atoi(e) : 0;
                }
                if (every > 0 && ++binds % (unsigned)every == 0
                    && seq < TEX_DUMP_SEQ_MAX)
                    dump_texture_bmp(TEX_DUMP_SEQ_BASE + seq++);
            }
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = g_pb.texs[0].offset;
        s_tex_use[s_tex_use_count].color   = g_pb.texs[0].color;
        s_tex_use[s_tex_use_count].width   = g_pb.texs[0].width;
        s_tex_use[s_tex_use_count].height  = g_pb.texs[0].height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
int pb_fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (g_pb.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)g_pb.inline_count * 4)
            return 0;
        p = (const uint8_t *)g_pb.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static void note_drawn(void)
{
    if (!g_pb.drawn_stale && g_pb.drawn_offset
        && g_pb.clip_w * g_pb.clip_h < g_pb.drawn_w * g_pb.drawn_h)
        return;
    g_pb.drawn_stale = 0;
    g_pb.drawn_offset = g_pb.color_offset;
    g_pb.drawn_pitch = g_pb.pitch;
    g_pb.drawn_bpp = pb_surface_bpp();
    g_pb.drawn_x = g_pb.clip_x; g_pb.drawn_y = g_pb.clip_y;
    g_pb.drawn_w = g_pb.clip_w; g_pb.drawn_h = g_pb.clip_h;
}

/* Mean R, G, B (0..255) of the current colour surface over its clip, from a
 * 32 x 32 grid of samples, and the brightest channel seen. For the frame
 * trace only: cheap enough per batch, and exact enough to say which batch
 * turned a picture white or black. */
static void surface_mean(uint32_t *r, uint32_t *g, uint32_t *b, uint32_t *mx)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = pb_surface_bpp(), sx, sy, n = 0;
    uint64_t tr = 0, tg = 0, tb = 0;

    *r = *g = *b = *mx = 0;
    if (!g_pb.color_offset || !g_pb.clip_w || !g_pb.clip_h
        || (bpp != 2 && bpp != 4))
        return;
    for (sy = 0; sy < 32; sy++) {
        uint32_t y = g_pb.clip_y + (g_pb.clip_h * (2 * sy + 1)) / 64;
        const uint8_t *row = mem + pb_dma_resolve(g_pb.color_offset)
                           + (size_t)y * g_pb.pitch;
        for (sx = 0; sx < 32; sx++) {
            uint32_t x = g_pb.clip_x + (g_pb.clip_w * (2 * sx + 1)) / 64;
            uint32_t cr, cg, cb;
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[x];
                cr = (v >> 16) & 0xFF; cg = (v >> 8) & 0xFF; cb = v & 0xFF;
            } else {
                uint16_t v = ((const uint16_t *)row)[x];
                cr = ((v >> 11) & 0x1F) << 3;
                cg = ((v >> 5) & 0x3F) << 2;
                cb = (v & 0x1F) << 3;
            }
            tr += cr; tg += cg; tb += cb; n++;
            if (cr > *mx) *mx = cr;
            if (cg > *mx) *mx = cg;
            if (cb > *mx) *mx = cb;
        }
    }
    *r = (uint32_t)(tr / n); *g = (uint32_t)(tg / n); *b = (uint32_t)(tb / n);
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
/* Set while RECOMP_FB_DUMP_FLIPS is dumping every flip: the report and
 * after-draw dumps stand down then, or they would put duplicate frames into
 * the sequence. */
static int s_flip_dumping;

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int drawn = g_pb.drawn_offset != 0;
    uint32_t offset = drawn ? g_pb.drawn_offset : g_pb.color_offset;
    uint32_t pitch = drawn ? g_pb.drawn_pitch : g_pb.pitch;
    uint32_t cx = drawn ? g_pb.drawn_x : g_pb.clip_x;
    uint32_t cy = drawn ? g_pb.drawn_y : g_pb.clip_y;
    static int seq;
    char path[512];
    uint32_t w = drawn ? g_pb.drawn_w : g_pb.clip_w;
    uint32_t h = drawn ? g_pb.drawn_h : g_pb.clip_h, y, x;
    uint32_t bpp = drawn ? g_pb.drawn_bpp : pb_surface_bpp();
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + pb_dma_resolve(offset)
                           + (size_t)(cy + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[cx + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[cx + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, g_pb.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);
static void surface_swizzle_setup(void);
static uint8_t *surface_pixel(uint8_t *base, uint32_t bpp, int x, int y);
/* The current surface's swizzle layout; see surface_swizzle_setup. */
static struct { int on; uint32_t w, h, mask_x, mask_y; } s_swz;

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = pb_surface_bpp();
    uint32_t y, x;
    uint16_t v16;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!g_pb.color_offset || !g_pb.pitch || !g_pb.clip_h || bpp == 0)
        return;
    {
        uint32_t base = pb_dma_resolve(g_pb.color_offset);
        if (pb_surface_write_refused(base,
                                  (g_pb.clip_y + g_pb.clip_h) * g_pb.pitch,
                                  "clear"))
            return;
        g_pb.color_base = base;
    }

    /* The clear value is in the surface's own pixel format, not A8R8G8B8
     * (xemu pgraph_get_clear_color): a 16-bit surface takes its low half
     * as is. Conker clears its R5G6B5 lighting targets with 0x0000FFFF,
     * which is white; read as A8R8G8B8 it was cyan, and the floor they
     * light kept only its red channel's shading -- the whole front end
     * came out green. */
    v16 = (uint16_t)g_pb.clear_color;
    surface_swizzle_setup();
    for (y = 0; y < g_pb.clip_h; y++) {
        uint8_t *row = mem + g_pb.color_base
                     + (size_t)(g_pb.clip_y + y) * g_pb.pitch;
        if (s_swz.on) {
            /* Swizzled: pixel by pixel, each to its Morton address. */
            for (x = 0; x < g_pb.clip_w; x++) {
                uint8_t *p = surface_pixel(mem + g_pb.color_base, bpp,
                                           (int)(g_pb.clip_x + x),
                                           (int)(g_pb.clip_y + y));
                if (!p)
                    continue;
                if (bpp == 4)
                    *(uint32_t *)p = g_pb.clear_color;
                else if (bpp == 2)
                    *(uint16_t *)p = v16;
            }
        } else if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + g_pb.clip_x;
            for (x = 0; x < g_pb.clip_w; x++)
                p[x] = g_pb.clear_color;
        } else if (bpp == 2) {
            uint16_t *p = (uint16_t *)row + g_pb.clip_x;
            for (x = 0; x < g_pb.clip_w; x++)
                p[x] = v16;
        }
    }
    g_pb.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((g_pb.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", g_pb.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == g_pb.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = g_pb.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    g_pb.clear_color, g_pb.color_offset, pb_surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    static int raster_test = -1;
    if (raster_test < 0)
        raster_test = getenv("RECOMP_RASTER_TEST") != NULL;
    if (raster_test) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (g_pb.clip_w && g_pb.clip_h) {
            float a[2], b[2], c[2];
            a[0] = g_pb.clip_w * 0.5f; a[1] = g_pb.clip_h * 0.15f;
            b[0] = g_pb.clip_w * 0.85f; b[1] = g_pb.clip_h * 0.85f;
            c[0] = g_pb.clip_w * 0.15f; c[1] = g_pb.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    g_pb.color_offset, pb_surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Only until the title flips. Following the draw surface on every scan
     * shows the buffer being written right now, half a frame at a time; past
     * the first flip the window is repointed at the finished one instead. */
    if (g_pb.flips == 0)
        xbox_FramebufferWindowSet(pb_dma_resolve(g_pb.color_offset), g_pb.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    /* Coordinates are signed: a bilinear tap one texel left of or above
     * texel 0 is -1, which must wrap to the far edge or clamp to 0 -- as an
     * unsigned value it clamped to the far edge instead. */
    int32_t sc = (int32_t)c, n = (int32_t)size;
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return (uint32_t)(((sc % n) + n) % n);
    return sc < 0 ? 0u : (sc >= n ? size - 1 : c);   /* clamp, and the rest */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

/* Sample texel (u, v) of stage texture `t`; `face` offsets a cube map. */
static int sample_tex(const Texture *t, uint32_t face_offset,
                      uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt, base = t->offset + face_offset;

    if (!t->valid)
        return 0;
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        /* Decoded blocks, direct-mapped by address. Every texel of a DXT
         * texture decodes its whole 4x4 block, and bilinear reads four
         * neighbours that nearly always share one -- this was a tenth of
         * the executor's time. Keyed on the block's bytes as well as its
         * address, so a texture rewritten in place (a render target reused,
         * a streamed mip) never returns stale texels.
         * ponytail: single-threaded executor, so a plain static table. */
        static NV_TLS struct { uintptr_t key; uint32_t fmt; uint64_t raw[2];
                        uint32_t px[16]; } cache[4096];
        uint32_t bb = d3d8_format_dxt_block_bytes(fmt);
        uint32_t bx = u >> 2, by = v >> 2, bw = (t->width + 3) >> 2;
        const uint8_t *blk = mem + base + ((size_t)by * bw + bx) * bb;
        uintptr_t key = (uintptr_t)blk;
        size_t slot = ((key / bb) ^ (key >> 16)) & 4095;
        uint64_t raw[2] = {0, 0};
        memcpy(raw, blk, bb);
        if (cache[slot].key != key || cache[slot].fmt != fmt
            || cache[slot].raw[0] != raw[0] || cache[slot].raw[1] != raw[1]) {
            uint32_t i;
            for (i = 0; i < 16; i++)
                d3d8_dxt_decode_texel(mem + base, fmt, bx * 4 + (i & 3),
                                      by * 4 + (i >> 2), t->width,
                                      &cache[slot].px[i]);
            cache[slot].key = key;
            cache[slot].fmt = fmt;
            cache[slot].raw[0] = raw[0];
            cache[slot].raw[1] = raw[1];
        }
        *argb = cache[slot].px[(v & 3) * 4 + (u & 3)];
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + base;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + base + (size_t)v * t->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!t->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + t->palette))[p[u]];
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
    return sample_tex(&g_pb.texs[0], 0, u, v, argb);
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = g_pb.texs[0].width, h = g_pb.texs[0].height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X.bmp",
             prefix, seq, g_pb.texs[0].offset, g_pb.texs[0].color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, g_pb.texs[0].color);
    fflush(stderr);
}

/* RECOMP_PROBE_TEXDUMP=<prefix>: level 0 of a probed draw's stage texture,
 * colour on the left and alpha as grey on the right, through sample_tex like
 * the rasteriser reads it. Once per address and format. */
static void probe_dump_stage_tex(const Texture *t, int st)
{
    static struct { uint32_t offset, color; } seen[64];
    static int nseen;
    const char *prefix = getenv("RECOMP_PROBE_TEXDUMP");
    uint32_t w = t->width, h = t->height, x, y, ow, row_bytes, pad, filesz, i;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !t->valid || !w || !h || w > 2048 || h > 2048 || nseen >= 64)
        return;
    for (i = 0; i < (uint32_t)nseen; i++)
        if (seen[i].offset == t->offset && seen[i].color == t->color)
            return;
    seen[nseen].offset = t->offset;
    seen[nseen++].color = t->color;
    ow = w * 2;
    row_bytes = ow * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;
    snprintf(path, sizeof path, "%st%d_%08X_fmt%02X.bmp", prefix, st,
             t->offset, t->color);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &ow, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < h; y++) {
        for (x = 0; x < ow; x++) {
            uint32_t argb = 0;
            uint8_t px[3];
            if (!sample_tex(t, 0, x % w, h - 1 - y, &argb))
                argb = 0;
            if (x < w) {
                px[0] = (uint8_t)argb;
                px[1] = (uint8_t)(argb >> 8);
                px[2] = (uint8_t)(argb >> 16);
            } else {
                px[0] = px[1] = px[2] = (uint8_t)(argb >> 24);
            }
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[PROBE]   texdump %s\n", path);
}

/* The surface, resolved once per batch.
 *
 * pb_dma_resolve consults the contiguous arena's high-water mark and
 * pb_surface_hits_image walks the image range; both were being done per pixel
 * -- pb_dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
static uint8_t *s_surface;          /* host address of surface row 0 */

/* Swizzled surfaces (SET_SURFACE_FORMAT type 2): render targets a title
 * draws into and then samples as a swizzled texture. Their pixels are in
 * Morton order, not rows, with the size in the format's log2 fields, so
 * a pixel written row-major is read back from somewhere else. Conker draws
 * its floor lighting into 128x128 R5G6B5 ones every frame; written as rows,
 * the floor sampled them scrambled. */
static void surface_swizzle_setup(void)
{
    s_swz.on = ((g_pb.format >> 8) & 0xF) == 2;
    if (!s_swz.on)
        return;
    s_swz.w = 1u << ((g_pb.format >> 16) & 0xF);
    s_swz.h = 1u << ((g_pb.format >> 24) & 0xF);
    xbox_swizzle_masks(s_swz.w, s_swz.h, &s_swz.mask_x, &s_swz.mask_y);
}

/* Host address of pixel (x, y) of the surface at `base`, or NULL when a
 * swizzled surface does not have that pixel. */
static uint8_t *surface_pixel(uint8_t *base, uint32_t bpp, int x, int y)
{
    if (!s_swz.on)
        return base + (size_t)y * g_pb.pitch + (size_t)x * bpp;
    if ((uint32_t)x >= s_swz.w || (uint32_t)y >= s_swz.h)
        return NULL;
    return base + (size_t)(swizzle_deposit((uint32_t)x, s_swz.mask_x)
                         | swizzle_deposit((uint32_t)y, s_swz.mask_y)) * bpp;
}

static int surface_begin_batch(const uint8_t *mem)
{
    uint32_t base = pb_dma_resolve(g_pb.color_offset);

    if (pb_surface_hits_image(base, (g_pb.clip_y + g_pb.clip_h) * g_pb.pitch))
        return 0;
    s_surface = (uint8_t *)mem + base;
    surface_swizzle_setup();
    return 1;
}

static uint32_t pack_color(const float c[4]);

/* One GL blend factor, per channel (r g b a). */
static void blend_factor(uint32_t f, const float s[4], const float d[4],
                         float out[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float c = (float)((g_pb.blend_color >> (k == 3 ? 24 : 16 - 8 * k))
                          & 0xFF) / 255.0f;
        switch (f) {
        case 0x0000: out[k] = 0.0f;            break;   /* ZERO                */
        case 0x0001: out[k] = 1.0f;            break;   /* ONE                 */
        case 0x0300: out[k] = s[k];            break;   /* SRC_COLOR           */
        case 0x0301: out[k] = 1.0f - s[k];     break;
        case 0x0302: out[k] = s[3];            break;   /* SRC_ALPHA           */
        case 0x0303: out[k] = 1.0f - s[3];     break;
        case 0x0304: out[k] = d[3];            break;   /* DST_ALPHA           */
        case 0x0305: out[k] = 1.0f - d[3];     break;
        case 0x0306: out[k] = d[k];            break;   /* DST_COLOR           */
        case 0x0307: out[k] = 1.0f - d[k];     break;
        case 0x0308: out[k] = k == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]); break;
        case 0x8001: out[k] = c;               break;   /* CONSTANT_COLOR      */
        case 0x8002: out[k] = 1.0f - c;        break;
        case 0x8003: out[k] = (float)(g_pb.blend_color >> 24) / 255.0f; break;
        case 0x8004: out[k] = 1.0f - (float)(g_pb.blend_color >> 24) / 255.0f; break;
        default:     out[k] = f ? 1.0f : 0.0f; break;
        }
    }
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *px;

    (void)mem;
    if (x < (int)g_pb.clip_x || x >= (int)(g_pb.clip_x + g_pb.clip_w))
        return;
    if (y < (int)g_pb.clip_y || y >= (int)(g_pb.clip_y + g_pb.clip_h))
        return;
    px = surface_pixel(s_surface, bpp, x, y);
    if (!px)
        return;
    g_pb.pixels++;
    if ((argb & 0x00FFFFFFu) > (g_pb.pixel_max & 0x00FFFFFFu))
        g_pb.pixel_max = argb;

    /* Blending, with the GL factor set and equations the NV2A takes.
     *
     * Only SRC_ALPHA / ONE_MINUS_SRC_ALPHA used to be honoured and every other
     * pair was written opaque. Menus never noticed; a race does: Burnout 3
     * finishes its 3D frame with screen-space passes that modulate or add
     * over the scene, and written opaque they paint the whole view over. */
    if (g_pb.blend_enable
        && !(g_pb.blend_sfactor == 1 && g_pb.blend_dfactor == 0)) {
        uint32_t dst = 0xFF000000u;
        float sc[4], dc[4], sf[4], df[4], out[4];
        int k;
        if (bpp == 4) {
            dst = *(const uint32_t *)px;
        } else if (bpp == 2) {
            uint32_t t = *(const uint16_t *)px;
            dst = 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
                | ((t & 0x001Fu) << 3);
        }
        for (k = 0; k < 4; k++) {           /* r g b a, 0..1 */
            int sh = k == 3 ? 24 : 16 - 8 * k;
            sc[k] = (float)((argb >> sh) & 0xFF) / 255.0f;
            dc[k] = (float)((dst  >> sh) & 0xFF) / 255.0f;
        }
        blend_factor(g_pb.blend_sfactor, sc, dc, sf);
        blend_factor(g_pb.blend_dfactor, sc, dc, df);
        for (k = 0; k < 4; k++) {
            float a = sc[k] * sf[k], b = dc[k] * df[k];
            switch (g_pb.blend_equation) {
            case 0x800A: out[k] = a - b; break;               /* SUBTRACT     */
            case 0x800B: out[k] = b - a; break;               /* REV_SUBTRACT */
            case 0x8007: out[k] = fminf(sc[k], dc[k]); break; /* MIN          */
            case 0x8008: out[k] = fmaxf(sc[k], dc[k]); break; /* MAX          */
            default:     out[k] = a + b; break;               /* ADD          */
            }
        }
        argb = pack_color(out);
    }

    if (g_pb.color_mask != 0x01010101u) {
        uint32_t keep = 0, old;
        if (!(g_pb.color_mask & 0x01000000u)) keep |= 0xFF000000u;
        if (!(g_pb.color_mask & 0x00010000u)) keep |= 0x00FF0000u;
        if (!(g_pb.color_mask & 0x00000100u)) keep |= 0x0000FF00u;
        if (!(g_pb.color_mask & 0x00000001u)) keep |= 0x000000FFu;
        if (keep == 0xFFFFFFFFu)
            return;
        if (bpp == 4) {
            old = *(const uint32_t *)px;
        } else {
            uint32_t t = *(const uint16_t *)px;
            old = ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
        }
        argb = (argb & ~keep) | (old & keep);
    }
    if (bpp == 4) {
        *(uint32_t *)px = argb;
    } else if (bpp == 2) {
        *(uint16_t *)px = (uint16_t)(((argb >> 8) & 0xF800)
                                   | ((argb >> 5) & 0x07E0)
                                   | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = pb_surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && g_pb.texs[0].valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)g_pb.clip_x) minx = (int)g_pb.clip_x;
    if (miny < (int)g_pb.clip_y) miny = (int)g_pb.clip_y;
    if (maxx > (int)(g_pb.clip_x + g_pb.clip_w)) maxx = (int)(g_pb.clip_x + g_pb.clip_w);
    if (maxy > (int)(g_pb.clip_y + g_pb.clip_h)) maxy = (int)(g_pb.clip_y + g_pb.clip_h);
    if (minx >= maxx || miny >= maxy) {
        g_pb.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    put_pixel(mem, bpp, x, y, texel);
                    continue;
                }
            }
            put_pixel(mem, bpp, x, y, argb);
        }
    }
    g_pb.tris_drawn++;
    note_drawn();
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    /* Size 0 is how the NV2A says "no array": the format register keeps
     * whatever offset and stride the previous layout left, so testing those
     * alone took a disabled slot 3 over Conker's colour in slot 1, and its
     * glow quad (alpha 0.2) was drawn at full strength. */
    if (g_pb.attr[3].size && g_pb.attr[3].offset && g_pb.attr[3].stride)
        return &g_pb.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (g_pb.attr[a].type == 0 && g_pb.attr[a].size == 4
                && g_pb.attr[a].offset && g_pb.attr[a].stride)
            return &g_pb.attr[a];
    return &g_pb.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (g_pb.attr[9].offset && g_pb.attr[9].stride)
        return &g_pb.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (g_pb.attr[a].type == 2 && g_pb.attr[a].size == 2
                && g_pb.attr[a].offset && g_pb.attr[a].stride)
            return &g_pb.attr[a];
    return &g_pb.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
int pb_fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!pb_fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(g_pb.texs[0].color)) {
        out[0] *= (float)g_pb.texs[0].width;
        out[1] *= (float)g_pb.texs[0].height;
    }
    return 1;
}

/* The colour of a vertex whose batch has no colour array.
 *
 * On the NV2A an attribute with no array enabled takes the value last written
 * to it with SET_VERTEX_DATA*; it is a register, not a per-draw default. A
 * title uses that for a quad's strength or fade: Conker's frontend glow adds
 * a downsampled copy of the frame back onto itself with blend SRC_ALPHA/ONE
 * and the alpha set this way. Returning opaque white here added the copy at
 * full strength, and the intro movie washed out to pure white within a few
 * frames. White remains the answer when the title never wrote the register,
 * which is the visible-wrong-colour choice for bring-up. */
static int constant_color(float c[4])
{
    if (!(g_pb.imm_set & (1u << 3)))
        return 0;
    c[0] = g_pb.imm_attr[3][0];
    c[1] = g_pb.imm_attr[3][1];
    c[2] = g_pb.imm_attr[3][2];
    c[3] = g_pb.imm_attr[3][3];
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!pb_fetch_attr(color_attr(), index, c) && !constant_color(c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!g_pb.clip_w || !g_pb.clip_h || !g_pb.idx_count)
        return 0;
    if (!pb_fetch_attr(&g_pb.attr[0], g_pb.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < g_pb.idx_count; i++) {
        if (!pb_fetch_attr(&g_pb.attr[0], g_pb.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)g_pb.clip_x
     || lo_x > (float)(g_pb.clip_x + g_pb.clip_w)
     || hi_y < (float)g_pb.clip_y
     || lo_y > (float)(g_pb.clip_y + g_pb.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!pb_fetch_attr(&g_pb.attr[0], i0, p[0])
     || !pb_fetch_attr(&g_pb.attr[0], i1, p[1])
     || !pb_fetch_attr(&g_pb.attr[0], i2, p[2]))
        return;

    textured = pb_fetch_texcoord(i0, uv[0])
            && pb_fetch_texcoord(i1, uv[1])
            && pb_fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

/* ---- 3D: vertex programs and depth -------------------------------------
 *
 * A batch drawn while TRANSFORM_EXECUTION_MODE says "program" carries
 * model-space positions. Each vertex is run through the title's own vertex
 * program (nv2a_vsh_interp.c), whose oPos output on Xbox D3D is already
 * screen space -- the runtime appends the viewport transform and the divide
 * by w -- with the clip-space w left in oPos.w. So the rasteriser gets pixel
 * coordinates and a depth, and w for perspective-correct texturing.
 *
 * ponytail: no clipping. A triangle with any vertex behind the eye (w <= 0)
 * is dropped rather than clipped, which loses the slivers that cross the near
 * plane; road right under the camera is where that shows. Clip in
 * homogeneous space if it matters. */

/* Depth and stencil buffers, host-side, one per zeta surface the title uses:
 * float depth and a byte of stencil per pixel. The guest's own copy is only
 * written when a texture reads the zeta surface (zeta_readback): Conker builds
 * its glow from the stencil, sampling the Z24S8 buffer as LIN_R8G8B8A8 so the
 * stencil byte is alpha, and alpha-testing it. With nothing in guest memory
 * every pixel failed and the glow chain added black (run 154). */
#define NV_ZBUF_W 1024
#define NV_ZBUF_H 1024
static struct {
    uint32_t offset, zf;        /* zf: zeta format, 1 Z16, 2 Z24S8 */
    float *z;
    uint8_t *s;
    int dirty;                  /* written since the guest copy was */
} s_zbufs[4];
static int s_zbuf_next;

static float zclear_value(void)
{
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. */
    uint32_t zf = (g_pb.format >> 4) & 0xF;
    return zf == 1 ? (float)(g_pb.zstencil_clear & 0xFFFF)
                   : (float)(g_pb.zstencil_clear >> 8);
}

static int zbuf_slot(int create)
{
    int i;
    size_t k, n = (size_t)NV_ZBUF_W * NV_ZBUF_H;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].offset == g_pb.zeta_offset)
            return i;
    if (!create)
        return -1;
    i = s_zbuf_next++ & 3;
    if (!s_zbufs[i].z)
        s_zbufs[i].z = (float *)malloc(sizeof(float) * n);
    if (!s_zbufs[i].s)
        s_zbufs[i].s = (uint8_t *)malloc(n);
    if (!s_zbufs[i].z || !s_zbufs[i].s)
        return -1;
    s_zbufs[i].offset = g_pb.zeta_offset;
    s_zbufs[i].zf = (g_pb.format >> 4) & 0xF;
    s_zbufs[i].dirty = 1;
    for (k = 0; k < n; k++)
        s_zbufs[i].z[k] = 3.4e38f;
    memset(s_zbufs[i].s, 0, n);
    return i;
}

static float *zbuf_current(int create)
{
    int i = zbuf_slot(create);
    return i < 0 ? NULL : s_zbufs[i].z;
}

static uint8_t *sbuf_current(int create)
{
    int i = zbuf_slot(create);
    return i < 0 ? NULL : s_zbufs[i].s;
}

/* The batch about to draw may write depth or stencil: the guest copy is
 * stale from here on. */
static void zbuf_touch(void)
{
    int i = zbuf_slot(0);
    if (i >= 0) {
        s_zbufs[i].dirty = 1;
        s_zbufs[i].zf = (g_pb.format >> 4) & 0xF;
    }
}

static void sbuf_clear(void)
{
    uint8_t *s = sbuf_current(1);
    if (s)
        memset(s, (int)(g_pb.zstencil_clear & 0xFF),
               (size_t)NV_ZBUF_W * NV_ZBUF_H);
    zbuf_touch();
}

/* Write a zeta surface the title is about to sample into guest memory, in
 * the surface's own format, over the rectangle the texture reads. Linear
 * textures only (a depth texture made from a render target is); the depth is
 * already in Z24 or Z16 units, as the clear value is.
 * ponytail: a float zeta format (SET_CONTROL0) is written as fixed. */
static void zeta_readback(const Texture *t)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t w, h, x, y;
    int i;

    for (i = 0; i < 4; i++)
        if (s_zbufs[i].z && s_zbufs[i].dirty
            && (s_zbufs[i].offset & 0x0FFFFFFFu) == (t->offset & 0x0FFFFFFFu))
            break;
    if (i == 4 || tex_size_from_format(t->color) || !t->pitch)
        return;
    w = t->width < NV_ZBUF_W ? t->width : NV_ZBUF_W;
    h = t->height < NV_ZBUF_H ? t->height : NV_ZBUF_H;
    for (y = 0; y < h; y++) {
        const float *zr = s_zbufs[i].z + (size_t)y * NV_ZBUF_W;
        const uint8_t *sr = s_zbufs[i].s + (size_t)y * NV_ZBUF_W;
        uint8_t *row = mem + t->offset + (size_t)y * t->pitch;
        for (x = 0; x < w; x++) {
            float z = zr[x];
            if (s_zbufs[i].zf == 1) {
                uint16_t v = (uint16_t)(z <= 0.0f ? 0.0f
                                      : z >= 65535.0f ? 65535.0f : z);
                if ((x + 1) * 2 <= t->pitch)
                    memcpy(row + x * 2, &v, 2);
            } else {
                uint32_t v = (uint32_t)(z <= 0.0f ? 0.0f
                                      : z >= 16777215.0f ? 16777215.0f : z);
                v = v << 8 | sr[x];
                if ((x + 1) * 4 <= t->pitch)
                    memcpy(row + x * 4, &v, 4);
            }
        }
    }
    s_zbufs[i].dirty = 0;
}

static int stencil_pass(uint8_t stored)
{
    uint32_t m = g_pb.stencil_rmask & 0xFFu;
    uint32_t r = g_pb.stencil_ref & m, v = stored & m;
    switch (g_pb.stencil_func) {                 /* ref OP stored, as GL */
    case 0x200: return 0;
    case 0x201: return r <  v;
    case 0x202: return r == v;
    case 0x203: return r <= v;
    case 0x204: return r >  v;
    case 0x205: return r != v;
    case 0x206: return r >= v;
    default:    return 1;
    }
}

static void stencil_apply(uint8_t *sp, uint32_t op)
{
    uint32_t v = *sp, n, wm = g_pb.stencil_wmask & 0xFFu;
    switch (op) {                                 /* GL enums */
    case 0x0000: n = 0; break;                    /* ZERO      */
    case 0x1E01: n = g_pb.stencil_ref & 0xFFu; break; /* REPLACE */
    case 0x1E02: n = v < 255 ? v + 1 : 255; break;/* INCR (sat) */
    case 0x1E03: n = v ? v - 1 : 0; break;        /* DECR (sat) */
    case 0x150A: n = ~v & 0xFFu; break;           /* INVERT    */
    case 0x8507: n = (v + 1) & 0xFFu; break;      /* INCR_WRAP */
    case 0x8508: n = (v - 1) & 0xFFu; break;      /* DECR_WRAP */
    default:     return;                          /* KEEP      */
    }
    *sp = (uint8_t)((v & ~wm) | (n & wm));
}

static void zbuf_clear(void)
{
    float *z = zbuf_current(1), v = zclear_value();
    size_t k;
    zbuf_touch();
    if (z)
        for (k = 0; k < (size_t)NV_ZBUF_W * NV_ZBUF_H; k++)
            z[k] = v;
}

static int depth_pass(float z, float stored)
{
    switch (g_pb.depth_func) {                   /* GL enums, as the NV2A */
    case 0x200: return 0;
    case 0x201: return z <  stored;
    case 0x202: return z == stored;
    case 0x203: return z <= stored;
    case 0x204: return z >  stored;
    case 0x205: return z != stored;
    case 0x206: return z >= stored;
    default:    return 1;
    }
}

static uint32_t pack_color(const float c[4])
{
    int i;
    uint32_t b[4];
    for (i = 0; i < 4; i++) {
        float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        b[i] = (uint32_t)(f * 255.0f + 0.5f);
    }
    return (b[3] << 24) | (b[0] << 16) | (b[1] << 8) | b[2];
}

static uint32_t modulate(uint32_t t, const float c[4])
{
    float f[4];
    f[0] = (float)((t >> 16) & 0xFF) / 255.0f * c[0];
    f[1] = (float)((t >> 8) & 0xFF) / 255.0f * c[1];
    f[2] = (float)(t & 0xFF) / 255.0f * c[2];
    f[3] = (float)(t >> 24) / 255.0f * c[3];
    return pack_color(f);
}

/* Bytes one texel of a (non-DXT) format takes, for cube-face strides. */
static uint32_t tex_texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Bytes between cube faces: one face with all its mip levels, rounded up to
 * the NV2A's 128-byte face alignment (xemu texture.c). */
static uint32_t tex_face_stride(const Texture *t)
{
    uint32_t w = t->width, h = t->height, lv, total = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    uint32_t levels = t->levels ? t->levels : 1;

    if (!tex_size_from_format(t->color))
        return (t->pitch * t->height + 127u) & ~127u;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = (w >> lv) ? (w >> lv) : 1, lh = (h >> lv) ? (h >> lv) : 1;
        total += block ? ((lw + 3) / 4) * ((lh + 3) / 4) * block
                       : lw * lh * tex_texel_bytes(t->color);
    }
    return (total + 127u) & ~127u;
}

/* One texel at normalised (swizzled/DXT) or texel (linear) coordinates, as
 * floats. An unusable stage reads white, so a missing texture multiplies
 * through instead of blacking the pixel out. */
static void rc_texel(const Texture *t, uint32_t face, float u, float v,
                     float out[4])
{
    uint32_t texel, mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    if (tex_size_from_format(t->color)) {
        u *= (float)t->width;
        v *= (float)t->height;
    }
    /* TENT (bilinear) when the title asks for it, magnifying or minifying:
     * a 64x32 sky gradient stretched over the screen is blocks with nearest
     * texels and a gradient with four. MAG 2 is tent; MIN 2 is tent at LOD 0
     * and 4/6 the tent mip modes.
     * ponytail: level 0 only, no mip selection -- minified textures shimmer.
     * Add LOD from the screen-space derivative when that shows. */
    if (mag == 2 || min == 2 || min == 4 || min == 6) {
        float fu = u - 0.5f, fv = v - 0.5f, wu, wv, c[4][4];
        int32_t iu = (int32_t)floorf(fu), iv = (int32_t)floorf(fv), k, j;
        wu = fu - (float)iu;
        wv = fv - (float)iv;
        for (k = 0; k < 4; k++) {
            if (sample_tex(t, face, (uint32_t)(iu + (k & 1)),
                           (uint32_t)(iv + (k >> 1)), &texel))
                nv2a_rc_unpack(texel, c[k]);
            else
                c[k][0] = c[k][1] = c[k][2] = c[k][3] = 1.0f;
        }
        for (j = 0; j < 4; j++)
            out[j] = (c[0][j] * (1.0f - wu) + c[1][j] * wu) * (1.0f - wv)
                   + (c[2][j] * (1.0f - wu) + c[3][j] * wu) * wv;
        return;
    }
    if (sample_tex(t, face, (uint32_t)(int32_t)floorf(u),
                   (uint32_t)(int32_t)floorf(v), &texel))
        nv2a_rc_unpack(texel, out);
    else
        out[0] = out[1] = out[2] = out[3] = 1.0f;
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (xemu psh.c). Returns 0 when a clip-plane stage kills the pixel. */
static int rc_stage_fetch(int st, const float c[4], float out[4])
{
    const Texture *t = &g_pb.texs[st];
    uint32_t mode = (g_pb.rc.stage_program >> (st * 5)) & 0x1F, j;
    float q = c[3] != 0.0f ? c[3] : 1.0f;

    switch (mode) {
    case 0:                                       /* NONE */
        out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
        return 1;
    case 1: case 2:                               /* PROJECT2D / 3D */
        rc_texel(t, 0, c[0] / q, c[1] / q, out);
        return 1;
    case 3: {                                     /* CUBEMAP */
        float x = c[0], y = c[1], z = c[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
        uint32_t face;
        if (!t->cube) {
            rc_texel(t, 0, c[0], c[1], out);
            return 1;
        }
        if (ax >= ay && ax >= az) {
            face = x > 0 ? 0 : 1; ma = ax; sc = x > 0 ? -z : z; tc = -y;
        } else if (ay >= az) {
            face = y > 0 ? 2 : 3; ma = ay; sc = x; tc = y > 0 ? z : -z;
        } else {
            face = z > 0 ? 4 : 5; ma = az; sc = z > 0 ? x : -x; tc = -y;
        }
        if (ma == 0.0f)
            ma = 1.0f;
        rc_texel(t, face * tex_face_stride(t), (sc / ma + 1.0f) * 0.5f,
                 (tc / ma + 1.0f) * 0.5f, out);
        return 1;
    }
    case 4:                                       /* PASSTHRU */
        for (j = 0; j < 4; j++)
            out[j] = c[j] < 0.0f ? 0.0f : (c[j] > 1.0f ? 1.0f : c[j]);
        return 1;
    case 5:                                       /* CLIPPLANE */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        for (j = 0; j < 4; j++) {
            int ge = (g_pb.clip_plane_mode >> (st * 4 + j)) & 1;
            if (ge ? c[j] >= 0.0f : c[j] < 0.0f)
                return 0;
        }
        return 1;
    default:
        /* ponytail: bump-env and dot-product modes read zero; they are water
         * and bump effects -- add them when a title's look depends on one. */
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return 1;
    }
}

/* The fixed-function fog unit, per vertex, from the program's oFog.x
 * (xemu vsh.c). Fog disabled reads as factor 1: no fog. */
static float fog_factor(float d)
{
    float f, px = g_pb.fog_param[0], py = g_pb.fog_param[1];
    if (!g_pb.fog_enable)
        return 1.0f;
    switch (g_pb.fog_mode) {
    case 0x800: case 0x802:                       /* EXP, EXP_ABS */
        f = px + exp2f(d * py * 16.0f) - 1.5f;
        break;
    case 0x801: case 0x803:                       /* EXP2, EXP2_ABS */
        f = px + exp2f(-d * d * py * py * 32.0f) - 1.5f;
        break;
    default:                                      /* LINEAR, LINEAR_ABS */
        f = px + d * py - 1.0f;
        break;
    }
    if (g_pb.fog_mode == 0x802 || g_pb.fog_mode == 0x803
        || g_pb.fog_mode == 0x804)
        f = fabsf(f);
    if (f != f)
        f = 1.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static int alpha_test_pass(float a)
{
    int v = (int)(a * 255.0f + 0.5f), r = (int)(g_pb.alpha_ref & 0xFF);
    switch (g_pb.alpha_func) {
    case 0x200: return 0;                         /* NEVER    */
    case 0x201: return v <  r;                    /* LESS     */
    case 0x202: return v == r;                    /* EQUAL    */
    case 0x203: return v <= r;                    /* LEQUAL   */
    case 0x204: return v >  r;                    /* GREATER  */
    case 0x205: return v != r;                    /* NOTEQUAL */
    case 0x206: return v >= r;                    /* GEQUAL   */
    default:    return 1;                         /* ALWAYS   */
    }
}

/* Everything a triangle's pixels need, computed once per triangle, so the
 * pixel loop can run over any subset of rows on any thread. */
typedef struct {
    uint8_t *mem;
    uint32_t bpp;
    const Nv2aVshOutput *va, *vb, *vc;
    const float *a, *b, *c;
    float iw[3], uv[3][2], stc[3][4][4], vfog[3], fogc[4], inv_area;
    float *zb;
    uint8_t *sb;                        /* stencil, when the test is on */
    int stencil_early_z;                /* depth failures leave it unchanged */
    int minx, maxx, miny, maxy, use_rc, textured;
    int probe_x, probe_y;               /* RECOMP_PIXEL_PROBE pixel, or -1 */
} XfTri;

/* What the combiners saw and made at the probe pixel, for probe_log. */
static struct { int hit; float d0[4], d1[4], t[4][4], tc[4][4], out[4]; } s_probe_px;

typedef struct { uint64_t depth_fail, pixels; uint32_t zpass; } XfCount;

/* Rows y0, y0+step, ... < maxy of triangle T. */
static void xf_rows(const XfTri *T, int y0, int step, XfCount *cnt)
{
    const float *a = T->a, *b = T->b, *c = T->c;
    const Nv2aVshOutput *va = T->va, *vb = T->vb, *vc = T->vc;
    int x, y, k;

    for (y = y0; y < T->maxy; y += step) {
        for (x = T->minx; x < T->maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            float l0, l1, l2, z, col[4], pw;
            uint32_t argb;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* w1 is opposite a, w2 opposite b, w0 opposite c. */
            l0 = w1 * T->inv_area; l1 = w2 * T->inv_area; l2 = w0 * T->inv_area;
            z = l0 * a[2] + l1 * b[2] + l2 * c[2];
            {
                float *zp = T->zb ? &T->zb[(size_t)y * NV_ZBUF_W + x] : NULL;
                uint8_t *sp = T->sb ? &T->sb[(size_t)y * NV_ZBUF_W + x] : NULL;
                int shade = (g_pb.color_mask & 0x01010101u) || g_pb.alpha_test;
                /* Depth test, then shade, then alpha test, and only then the
                 * depth write: an alpha-tested texel that is cut away must
                 * not leave its depth behind (foliage, fences). With stencil
                 * on, the depth test waits until after the alpha test too:
                 * the stencil op depends on it, and a cut-away texel must
                 * not change the stencil either -- unless both ops a depth
                 * failure can lead to are KEEP (Conker's tagging passes),
                 * when it changes nothing and can be dropped early. */
                if ((!sp || T->stencil_early_z) && zp && g_pb.depth_test
                    && !depth_pass(z, *zp)) {
                    cnt->depth_fail++;
                    continue;
                }
                argb = 0;
                if (shade) {
                    for (k = 0; k < 4; k++)
                        col[k] = l0 * va->d0[k] + l1 * vb->d0[k] + l2 * vc->d0[k];
                    pw = l0 * T->iw[0] + l1 * T->iw[1] + l2 * T->iw[2];
                    if (T->use_rc) {
                        float d1[4], fog[4], t[4][4], out[4], tc[4];
                        int st, j, keep = 1;
                        for (k = 0; k < 4; k++)
                            d1[k] = l0 * va->d1[k] + l1 * vb->d1[k] + l2 * vc->d1[k];
                        fog[0] = T->fogc[0]; fog[1] = T->fogc[1]; fog[2] = T->fogc[2];
                        fog[3] = l0 * T->vfog[0] + l1 * T->vfog[1] + l2 * T->vfog[2];
                        for (st = 0; st < 4 && keep; st++) {
                            if (!((g_pb.rc.stage_program >> (st * 5)) & 0x1F)) {
                                t[st][0] = t[st][1] = t[st][2] = 0.0f;
                                t[st][3] = 1.0f;
                                continue;
                            }
                            for (j = 0; j < 4; j++)
                                tc[j] = (l0 * T->stc[0][st][j] + l1 * T->stc[1][st][j]
                                       + l2 * T->stc[2][st][j]) / pw;
                            if (x == T->probe_x && y == T->probe_y)
                                memcpy(s_probe_px.tc[st], tc, sizeof tc);
                            keep = rc_stage_fetch(st, tc, t[st]);
                        }
                        if (!keep)
                            continue;
                        nv2a_rc_eval(&g_pb.rc, col, d1, fog,
                                     (const float (*)[4])t, out);
                        if (x == T->probe_x && y == T->probe_y) {
                            s_probe_px.hit = 1;
                            memcpy(s_probe_px.d0, col, sizeof col);
                            memcpy(s_probe_px.d1, d1, sizeof d1);
                            memcpy(s_probe_px.t, t, sizeof t);
                            memcpy(s_probe_px.out, out, sizeof out);
                        }
                        if (g_pb.alpha_test && !alpha_test_pass(out[3]))
                            continue;
                        argb = pack_color(out);
                    } else {
                        argb = pack_color(col);
                        if (T->textured) {
                            uint32_t texel;
                            float tu, tv;
                            tu = (l0 * T->uv[0][0] + l1 * T->uv[1][0] + l2 * T->uv[2][0]) / pw;
                            tv = (l0 * T->uv[0][1] + l1 * T->uv[1][1] + l2 * T->uv[2][1]) / pw;
                            if (sample_texture((uint32_t)(int32_t)floorf(tu),
                                               (uint32_t)(int32_t)floorf(tv), &texel))
                                argb = modulate(texel, col);
                        }
                        if (g_pb.alpha_test
                            && !alpha_test_pass((float)(argb >> 24) / 255.0f))
                            continue;
                    }
                }
                if (sp) {
                    if (!stencil_pass(*sp)) {
                        stencil_apply(sp, g_pb.stencil_op_fail);
                        continue;
                    }
                    if (zp && g_pb.depth_test && !depth_pass(z, *zp)) {
                        stencil_apply(sp, g_pb.stencil_op_zfail);
                        cnt->depth_fail++;
                        continue;
                    }
                    stencil_apply(sp, g_pb.stencil_op_zpass);
                }
                if (zp && g_pb.depth_mask)
                    *zp = z;
                if (g_pb.zpass_enable)
                    cnt->zpass++;
                if (!(g_pb.color_mask & 0x01010101u))
                    continue;              /* colour writes off: depth only */
                cnt->pixels++;
            }
            put_pixel(T->mem, T->bpp, x, y, argb);
        }
    }
}

/* Triangle queue and worker threads.
 *
 * The rasteriser is a software GPU. Pixels split cleanly by row: every pixel
 * reads only its own depth and colour, so N threads that each own every Nth
 * row of the screen need no locking -- and because each thread draws the
 * queued triangles in order, overlapping triangles still blend and
 * depth-test in submission order.
 *
 * The pool used to be woken per triangle, for big triangles only (Burnout
 * 3's full-screen passes). A Conker frame is ~10,000 small triangles, so the
 * executor thread drew nearly all of them alone while the workers slept, and
 * waking them per triangle cost more than it saved. Triangles are now set up
 * on the executor thread and queued; the queue is drawn when the batch ends
 * (raster_xf_prims) or fills, with one wake-up for all of it. Draw state
 * (g_pb, s_surface, the combiners) cannot change inside a batch, so the
 * workers can read it directly.
 *
 * RECOMP_RASTER_THREADS=<n> sets the count (1 = off); the default leaves a
 * few cores for the title and the host. RECOMP_RASTER_MT_MIN=<pixels>: a
 * queue whose bounding boxes add up to less is drawn on the executor thread.
 * ponytail: g_pb.pixels/pixel_max in put_pixel are unsynchronised stats and
 * may undercount; nothing depends on them. */
#define NV_RASTER_MAX_THREADS 16
#define NV_RASTER_MT_MIN_PIXELS 1024
#define NV_TRIQ_MAX 2048

typedef struct {
    XfTri T;
    Nv2aVshOutput v[3];          /* the vertices T points at: clipped ones
                                  * live on raster_xf_clipped's stack */
} XfQueued;

static XfQueued s_triq[NV_TRIQ_MAX];
static int s_triq_n;
static long s_triq_px;

/* The rows of every queued triangle that thread k of n owns. */
static void triq_rows(int k, int n, XfCount *cnt)
{
    int i;
    for (i = 0; i < s_triq_n; i++) {
        const XfTri *T = &s_triq[i].T;
        int y0 = T->miny + ((k - T->miny % n) % n + n) % n;
        xf_rows(T, y0, n, cnt);
    }
}

/* The pool runs one job at a time on all its threads: job(k, n) for k in
 * 0..n-1, the caller taking k = n-1, and returns when every part is done. */
typedef void (*PoolJob)(int k, int n);

#if defined(_WIN32)
static struct {
    int n;                                  /* threads incl. the caller */
    HANDLE start[NV_RASTER_MAX_THREADS], done;
    volatile LONG pending;
    PoolJob job;
} s_pool;

static DWORD WINAPI raster_worker(LPVOID arg)
{
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(s_pool.start[k], INFINITE);
        s_pool.job(k, s_pool.n);
        if (InterlockedDecrement(&s_pool.pending) == 0)
            SetEvent(s_pool.done);
    }
    return 0;
}

static int raster_pool_size(void)
{
    static int init;
    if (!init) {
        const char *e = getenv("RECOMP_RASTER_THREADS");
        SYSTEM_INFO si;
        int n, k;
        init = 1;
        GetSystemInfo(&si);
        n = e ? atoi(e) : (int)si.dwNumberOfProcessors - 4;
        if (n < 1) n = 1;
        if (n > NV_RASTER_MAX_THREADS) n = NV_RASTER_MAX_THREADS;
        s_pool.n = n;
        if (n > 1) {
            s_pool.done = CreateEventA(NULL, FALSE, FALSE, NULL);
            for (k = 0; k < n - 1; k++) {
                s_pool.start[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
                CloseHandle(CreateThread(NULL, 0, raster_worker,
                                         (LPVOID)(intptr_t)k, 0, NULL));
            }
        }
    }
    return s_pool.n;
}

static void pool_run(PoolJob job)
{
    int n = s_pool.n, k;

    s_pool.job = job;
    s_pool.pending = n - 1;
    for (k = 0; k < n - 1; k++)
        SetEvent(s_pool.start[k]);
    job(n - 1, n);
    WaitForSingleObject(s_pool.done, INFINITE);
}
#else
static int raster_pool_size(void) { return 1; }
static void pool_run(PoolJob job) { job(0, 1); }
#endif

static XfCount s_triq_cnt[NV_RASTER_MAX_THREADS];

static void triq_job(int k, int n)
{
    memset(&s_triq_cnt[k], 0, sizeof s_triq_cnt[k]);
    triq_rows(k, n, &s_triq_cnt[k]);
}

static long raster_mt_min(void)
{
    static long v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_RASTER_MT_MIN");
        v = e ? atol(e) : NV_RASTER_MT_MIN_PIXELS;
        if (v < 0) v = 0;
    }
    return v;
}

/* Draw everything queued, then empty the queue. */
static void triq_flush(void)
{
    XfCount total = {0, 0, 0};
    int n, k;

    if (!s_triq_n)
        return;
    n = raster_pool_size();
    if (n <= 1 || s_triq_px < raster_mt_min()) {
        triq_rows(0, 1, &total);
    } else {
        pool_run(triq_job);
        for (k = 0; k < n; k++) {
            total.depth_fail += s_triq_cnt[k].depth_fail;
            total.pixels += s_triq_cnt[k].pixels;
            total.zpass += s_triq_cnt[k].zpass;
        }
    }
    g_pb.xf_depth_fail += total.depth_fail;
    g_pb.xf_pixels += total.pixels;
    g_pb.zpass_count += total.zpass;
    s_triq_n = 0;
    s_triq_px = 0;
}

/* Queue triangle T, with copies of its vertices. */
static void triq_push(const XfTri *T)
{
    XfQueued *q;

    if (s_triq_n == NV_TRIQ_MAX)
        triq_flush();
    q = &s_triq[s_triq_n++];
    q->T = *T;
    q->v[0] = *T->va; q->v[1] = *T->vb; q->v[2] = *T->vc;
    q->T.va = &q->v[0]; q->T.vb = &q->v[1]; q->T.vc = &q->v[2];
    q->T.a = q->v[0].pos; q->T.b = q->v[1].pos; q->T.c = q->v[2].pos;
    s_triq_px += (long)(T->maxx - T->minx) * (T->maxy - T->miny);
}

/* RECOMP_PIXEL_PROBE=x,y[,first_flip]: every program-path triangle that
 * covers screen pixel (x, y) is logged with the pixel before and after it and
 * what decided its colour. A wrong area on screen says nothing about which
 * of a thousand draws made it; this names the draw. Up to 400 lines, from
 * flip first_flip on. */
static struct { int on, x, y; uint32_t from_flip, hits; } s_probe = { -1 };
static int s_probe_clipped;             /* triangle came from near clipping */

static int probe_init(void)
{
    if (s_probe.on < 0) {
        const char *e = getenv("RECOMP_PIXEL_PROBE");
        s_probe.on = 0;
        if (e && sscanf(e, "%d,%d,%u", &s_probe.x, &s_probe.y,
                        &s_probe.from_flip) >= 2)
            s_probe.on = 1;
    }
    return s_probe.on;
}

static uint32_t probe_read(uint32_t bpp)
{
    const uint8_t *px = surface_pixel(s_surface, bpp, s_probe.x, s_probe.y);
    if (!px)
        return 0;
    if (bpp == 4)
        return *(const uint32_t *)px;
    {
        uint32_t t = *(const uint16_t *)px;
        return 0xFF000000u | ((t & 0xF800u) << 8) | ((t & 0x07E0u) << 5)
             | ((t & 0x001Fu) << 3);
    }
}

static int probe_covers(const float *a, const float *b, const float *c)
{
    float px = (float)s_probe.x + 0.5f, py = (float)s_probe.y + 0.5f;
    float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
    float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
    float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
    return (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
}

static void probe_log(const XfTri *T, uint32_t before, uint32_t after,
                      float zbuf)
{
    const Nv2aVshOutput *v[3];
    const float *a = T->a, *b = T->b, *c = T->c;
    float px = (float)s_probe.x + 0.5f, py = (float)s_probe.y + 0.5f, z;
    int k;
    v[0] = T->va; v[1] = T->vb; v[2] = T->vc;
    /* The triangle's depth at the pixel, as xf_rows computes it. */
    z = ((c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])) * T->inv_area * a[2]
      + ((a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])) * T->inv_area * b[2]
      + ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) * T->inv_area * c[2];
    fprintf(stderr, "[PROBE] flip %u draw %u: %08X -> %08X%s  tex0 %s fmt %02X"
            " at %08X  rc %d prog %05X  blend %d %X/%X  depth %d %X mask %d"
            " z %.9g vs %.9g  alpha %d\n", g_pb.flips, g_pb.draws, before,
            after, s_probe_clipped ? " (near-clipped)" : "",
            g_pb.texs[0].valid ? "on" : "off", g_pb.texs[0].color,
            g_pb.texs[0].offset, T->use_rc, g_pb.rc.stage_program,
            g_pb.blend_enable, g_pb.blend_sfactor, g_pb.blend_dfactor,
            g_pb.depth_test, g_pb.depth_func, g_pb.depth_mask, z, zbuf,
            g_pb.alpha_test);
    for (k = 0; k < 3; k++)
        fprintf(stderr, "[PROBE]   v%d pos %.1f %.1f %.4f w %.3f  d0 %.2f %.2f"
                " %.2f %.2f  t0 %.3f %.3f %.3f %.3f\n", k, v[k]->pos[0],
                v[k]->pos[1], v[k]->pos[2], v[k]->pos[3], v[k]->d0[0],
                v[k]->d0[1], v[k]->d0[2], v[k]->d0[3], v[k]->tex[0][0],
                v[k]->tex[0][1], v[k]->tex[0][2], v[k]->tex[0][3]);
    /* The combiner program and what it made of this pixel. */
    fprintf(stderr, "[PROBE]   cmask %08X  rc ctl %08X fin %08X %08X fc %08X %08X\n",
            g_pb.color_mask, g_pb.rc.control, g_pb.rc.final0,
            g_pb.rc.final1, g_pb.rc.final_c0, g_pb.rc.final_c1);
    for (k = 0; k < (int)(g_pb.rc.control & 0xFF) && k < 8; k++)
        fprintf(stderr, "[PROBE]   s%d icw %08X %08X ocw %08X %08X c %08X %08X\n",
                k, g_pb.rc.color_icw[k], g_pb.rc.alpha_icw[k],
                g_pb.rc.color_ocw[k], g_pb.rc.alpha_ocw[k],
                g_pb.rc.factor0[k], g_pb.rc.factor1[k]);
    fprintf(stderr, "[PROBE]   alpha func %X ref %02X\n", g_pb.alpha_func,
            g_pb.alpha_ref & 0xFF);
    for (k = 0; k < 4; k++)
        if ((g_pb.rc.stage_program >> (k * 5)) & 0x1F) {
            /* How much of level 0 has alpha >= 0.5, and its mean alpha: says
             * whether an all-transparent sample is the texture or where we
             * read it. */
            const Texture *tx = &g_pb.texs[k];
            uint32_t u, vv, texel, solid = 0, n = 0;
            uint64_t asum = 0;
            for (vv = 0; vv < tx->height && vv < 1024; vv++)
                for (u = 0; u < tx->width && u < 1024; u++)
                    if (sample_tex(tx, 0, u, vv, &texel)) {
                        n++;
                        asum += texel >> 24;
                        solid += (texel >> 24) >= 0x80;
                    }
            fprintf(stderr, "[PROBE]   t%d %08X %ux%u fmt %02X%s pitch %u levels %u"
                    " addr %X/%X filter %08X cube %d  alpha>=.5 %u/%u mean %.3f\n",
                    k, tx->offset, tx->width, tx->height, tx->color,
                    tx->valid ? "" : " (invalid)", tx->pitch, tx->levels,
                    tx->addr_u, tx->addr_v, tx->filter, tx->cube, solid, n,
                    n ? (double)asum / n / 255.0 : 0.0);
            probe_dump_stage_tex(tx, k);
        }
    if (s_probe_px.hit) {
        const float *q;
        q = s_probe_px.d0;
        fprintf(stderr, "[PROBE]   px d0 %.3f %.3f %.3f %.3f", q[0], q[1], q[2], q[3]);
        q = s_probe_px.d1;
        fprintf(stderr, "  d1 %.3f %.3f %.3f %.3f\n", q[0], q[1], q[2], q[3]);
        for (k = 0; k < 4; k++) {
            q = s_probe_px.t[k];
            fprintf(stderr, "[PROBE]   px t%d %.3f %.3f %.3f %.3f  at %.4f %.4f %.4f"
                    " %.4f\n", k, q[0], q[1], q[2], q[3], s_probe_px.tc[k][0],
                    s_probe_px.tc[k][1], s_probe_px.tc[k][2], s_probe_px.tc[k][3]);
        }
        q = s_probe_px.out;
        fprintf(stderr, "[PROBE]   px out %.3f %.3f %.3f %.3f\n", q[0], q[1], q[2], q[3]);
    } else {
        fprintf(stderr, "[PROBE]   px not shaded (depth, clip or flat path)\n");
    }
}

static void raster_xf_triangle(const Nv2aVshOutput *va, const Nv2aVshOutput *vb,
                               const Nv2aVshOutput *vc)
{
    uint32_t probe_before = 0;
    float probe_z = -1.0f;              /* depth buffer at the pixel, before */
    int probe = 0;
    XfTri T;
    XfCount cnt = {0, 0, 0};
    const Nv2aVshOutput *v[3];
    const float *a, *b, *c;
    float area, su = 1.0f, sv = 1.0f;
    int k;
    static int no_rc = -1;

    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    memset(&T, 0, sizeof T);
    T.probe_x = T.probe_y = -1;
    T.mem = (uint8_t *)xbox_GetMemoryOffset();
    T.bpp = pb_surface_bpp();
    T.textured = g_pb.texs[0].valid;
    T.use_rc = g_pb.rc_seen && !no_rc;
    T.va = va; T.vb = vb; T.vc = vc;

    v[0] = va; v[1] = vb; v[2] = vc;
    a = va->pos; b = vb->pos; c = vc->pos;
    T.a = a; T.b = b; T.c = c;
    if (T.bpp != 4 && T.bpp != 2)
        return;
    if (a[3] <= 0.0f || b[3] <= 0.0f || c[3] <= 0.0f) {
        g_pb.tris_behind++;
        return;
    }
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f || area != area) {
        g_pb.xf_degenerate++;
        return;
    }
    for (k = 0; k < 3; k++) {
        if (!g_pb.xf_seeded)
            g_pb.xf_min[k] = g_pb.xf_max[k] = a[k];
        if (a[k] < g_pb.xf_min[k]) g_pb.xf_min[k] = a[k];
        if (a[k] > g_pb.xf_max[k]) g_pb.xf_max[k] = a[k];
    }
    g_pb.xf_seeded = 1;
    if (!surface_begin_batch(T.mem))
        return;
    if (g_pb.depth_test || g_pb.depth_mask || g_pb.stencil_test)
        T.zb = zbuf_current(1);
    if (g_pb.stencil_test)
        T.sb = sbuf_current(1);
    T.stencil_early_z = g_pb.stencil_op_fail == 0x1E00
                     && g_pb.stencil_op_zfail == 0x1E00;
    if (tex_size_from_format(g_pb.texs[0].color)) {
        su = (float)g_pb.texs[0].width;
        sv = (float)g_pb.texs[0].height;
    }
    for (k = 0; k < 3; k++) {
        T.iw[k] = 1.0f / v[k]->pos[3];
        T.uv[k][0] = v[k]->tex[0][0] * su * T.iw[k];
        T.uv[k][1] = v[k]->tex[0][1] * sv * T.iw[k];
    }
    if (T.use_rc) {
        int st, j;
        for (k = 0; k < 3; k++) {
            for (st = 0; st < 4; st++)
                for (j = 0; j < 4; j++)
                    T.stc[k][st][j] = v[k]->tex[st][j] * T.iw[k];
            T.vfog[k] = fog_factor(v[k]->fog[0]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        nv2a_rc_unpack(g_pb.fog_color, T.fogc);
        { float r = T.fogc[2]; T.fogc[2] = T.fogc[0]; T.fogc[0] = r; }
    }

    T.minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    T.maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    T.miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    T.maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));
    if (T.minx < (int)g_pb.clip_x) T.minx = (int)g_pb.clip_x;
    if (T.miny < (int)g_pb.clip_y) T.miny = (int)g_pb.clip_y;
    if (T.maxx > (int)(g_pb.clip_x + g_pb.clip_w)) T.maxx = (int)(g_pb.clip_x + g_pb.clip_w);
    if (T.maxy > (int)(g_pb.clip_y + g_pb.clip_h)) T.maxy = (int)(g_pb.clip_y + g_pb.clip_h);
    if (T.maxx > NV_ZBUF_W) T.maxx = NV_ZBUF_W;
    if (T.maxy > NV_ZBUF_H) T.maxy = NV_ZBUF_H;
    if (T.minx >= T.maxx || T.miny >= T.maxy) {
        g_pb.tris_skipped_offscreen++;
        g_pb.xf_offscreen++;
        return;
    }
    g_pb.xf_drawn++;
    T.inv_area = 1.0f / area;

    if (probe_init() && s_probe.hits < 400 && g_pb.flips >= s_probe.from_flip
        && s_probe.x >= T.minx && s_probe.x < T.maxx
        && s_probe.y >= T.miny && s_probe.y < T.maxy && probe_covers(a, b, c)) {
        probe = 1;
        T.probe_x = s_probe.x;
        T.probe_y = s_probe.y;
    }
    if (probe) {
        /* The probe logs the pixel before and after this one triangle, so
         * everything queued ahead of it has to be on the surface first, and
         * it is drawn right here on this thread. */
        triq_flush();
        s_probe_px.hit = 0;
        probe_before = probe_read(T.bpp);
        probe_z = T.zb ? T.zb[(size_t)s_probe.y * NV_ZBUF_W + s_probe.x] : -1.0f;
        xf_rows(&T, T.miny, 1, &cnt);
        s_probe.hits += 4;
        probe_log(&T, probe_before, probe_read(T.bpp), probe_z);
        g_pb.xf_depth_fail += cnt.depth_fail;
        g_pb.xf_pixels += cnt.pixels;
        g_pb.zpass_count += cnt.zpass;
    } else {
        triq_push(&T);
    }
    g_pb.tris_drawn++;
    note_drawn();
}


/* Near-plane clipping.
 *
 * A triangle with a vertex behind the eye (w <= 0) used to be dropped whole.
 * That is invisible for distant geometry and ruinous close up: in a chase
 * view the road under the camera is exactly the set of triangles that reach
 * behind it, so the bottom of the frame showed a hole -- a quarter of every
 * race frame's triangles went this way.
 *
 * Clipping has to happen in clip space, but the D3D epilogue has already
 * divided x, y and z by w and applied the viewport (c[58] scale, c[59]
 * offset) while leaving w itself alone. So each vertex is taken back to clip
 * space first: clip = (screen - offset) / scale * w. Everything else the
 * vertex carries -- colours, fog, texture coordinates -- is linear in clip
 * space and is interpolated directly. The polygon is clipped against
 * w = NV_CLIP_W and the near plane, re-projected, and fanned.
 * ponytail: x/y/far fall to the rasteriser's bounds check, which is
 * correct, just slower for huge off-screen triangles. */
#define NV_CLIP_W 1e-3f

static void xf_to_clip(const Nv2aVshOutput *v, const float k[3],
                       const float o[3], float c[3])
{
    int i;
    for (i = 0; i < 3; i++)
        c[i] = k[i] != 0.0f ? (v->pos[i] - o[i]) / k[i] * v->pos[3] : 0.0f;
}

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

/* Which side of clip plane p a clip-space vertex is on (>= 0: kept).
 * Plane 0 is w = NV_CLIP_W, plane 1 the near plane z = 0.
 *
 * w alone was not enough. A vertex between the eye and the near plane has a
 * small positive w, so it passed, and projected to coordinates in the
 * millions or billions: the bounding box overflowed int and the triangle
 * counted as off-surface, or its area overflowed and it counted as
 * degenerate -- dropped either way, without a trace. The NV2A never draws
 * that part (depth below 0), so it is clipped away like the part behind the
 * eye. */
static float xf_plane_dist(const Nv2aVshOutput *v, int p)
{
    return p == 0 ? v->pos[3] - NV_CLIP_W : v->pos[2];
}

static void raster_xf_clipped(const Nv2aVshOutput *a, const Nv2aVshOutput *b,
                              const Nv2aVshOutput *c)
{
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput poly[2][8];
    float k[3], o[3], cc[3];
    int n = 3, cur = 0, i, j, p;

    {
        const float *sc = nv2a_vsh_constant(58), *of = nv2a_vsh_constant(59);
        for (i = 0; i < 3; i++) { k[i] = sc[i]; o[i] = of[i]; }
    }
    in[0] = a; in[1] = b; in[2] = c;
    /* In front of the eye and past the near plane: as it is. For w > 0 the
     * clip-space z has the sign of (screen z - offset) / scale. */
    for (i = 0; i < 3; i++)
        if (in[i]->pos[3] <= NV_CLIP_W
            || (k[2] != 0.0f && (in[i]->pos[2] - o[2]) / k[2] < 0.0f))
            break;
    if (i == 3) {
        raster_xf_triangle(a, b, c);
        return;
    }
    /* The same triangle arrives in another vertex order in another pass (a
     * strip for the base layer, a list for the lighting over it), and a
     * different order clips into a differently fanned polygon whose depth
     * differs in the last bits -- so the later pass's EQUAL/LEQUAL test
     * failed and its pixels vanished. Put the corners in one order first;
     * the rasteriser takes either winding. */
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2 - i; j++)
            if (memcmp(in[j]->pos, in[j + 1]->pos, sizeof in[j]->pos) > 0) {
                const Nv2aVshOutput *t = in[j];
                in[j] = in[j + 1];
                in[j + 1] = t;
            }
    for (i = 0; i < 3; i++) {
        poly[0][i] = *in[i];
        xf_to_clip(in[i], k, o, cc);
        for (j = 0; j < 3; j++)
            poly[0][i].pos[j] = cc[j];
    }
    /* Sutherland-Hodgman, one plane at a time; 3 -> at most 5 vertices. */
    for (p = 0; p < 2; p++) {
        const Nv2aVshOutput *src = poly[cur];
        Nv2aVshOutput *dst = poly[cur ^ 1];
        int m = 0;
        for (i = 0; i < n; i++) {
            const Nv2aVshOutput *P = &src[i], *Q = &src[(i + 1) % n];
            float dp = xf_plane_dist(P, p), dq = xf_plane_dist(Q, p);
            if (dp >= 0.0f)
                dst[m++] = *P;
            if ((dp >= 0.0f) != (dq >= 0.0f))
                xf_lerp(P, Q, dp / (dp - dq), &dst[m++]);
        }
        n = m;
        cur ^= 1;
        if (n < 3) {
            g_pb.tris_behind++;              /* nothing left in front */
            return;
        }
    }
    /* Back to screen space: divide by the (now positive) w, viewport. */
    for (i = 0; i < n; i++)
        for (j = 0; j < 3; j++)
            poly[cur][i].pos[j] = poly[cur][i].pos[j] / poly[cur][i].pos[3]
                                * k[j] + o[j];
    s_probe_clipped = 1;
    for (i = 1; i + 1 < n; i++)
        raster_xf_triangle(&poly[cur][0], &poly[cur][i], &poly[cur][i + 1]);
    s_probe_clipped = 0;
}

static Nv2aVshOutput s_xf[NV_MAX_INDICES];

static int transform_vertex(uint32_t index, Nv2aVshOutput *out)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t a;

    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        pb_fetch_attr(&g_pb.attr[a], index, in[a]);   /* absent: 0,0,0,1 */
    return nv2a_vsh_run((const float (*)[4])in, out);
}

static void raster_xf_prims(uint32_t n);

/* Post-transform vertex cache, one batch deep, and transforms on the pool.
 *
 * An indexed triangle list names each vertex about six times (once per
 * triangle around it), and every one of those ran the vertex program again:
 * same index, same attributes, same program and constants, same result. The
 * NV2A keeps a small cache of transformed vertices for exactly this. Here a
 * vertex index maps to the first s_xf slot that holds it in this batch; only
 * those first slots run the program, and repeats are copied from them
 * afterwards. The interpreter was the largest single cost of the GPU thread
 * (run_program, 30% of it).
 *
 * The program runs are independent, so a big batch spreads them over the
 * raster pool. The first one always runs here, alone: it decodes the
 * program and sets the interpreter's one-time switches before any worker
 * reads them.
 *
 * Direct-mapped on the low 16 bits; the index is kept to tell 32-bit indices
 * that share them apart. A new batch bumps the stamp instead of clearing.
 * Off while the program may write constants (CXT_WRITE_EN): then one vertex
 * can change what the next computes, and only running them all, in order,
 * on one thread is right. Also one thread under RECOMP_VSH_DUMP/_STEP, whose
 * logging is not thread-safe. */
#define NV_VCACHE_SIZE 65536u
#define NV_VSH_MT_MIN  64           /* program runs worth waking the pool */
static uint32_t s_vc_stamp[NV_VCACHE_SIZE], s_vc_index[NV_VCACHE_SIZE];
static uint32_t s_vc_slot[NV_VCACHE_SIZE], s_vc_now;
static uint32_t s_xf_from[NV_MAX_INDICES];  /* slot to copy, or itself */
static uint32_t s_xf_run[NV_MAX_INDICES], s_xf_nrun;   /* slots to run */
static volatile int s_xf_failed;

static void xf_job(int k, int n)
{
    /* Contiguous shares: neighbouring vertices share cache lines. Slot 0 of
     * s_xf_run already ran. */
    uint32_t m = s_xf_nrun - 1, j;
    uint32_t lo = 1 + (uint32_t)((uint64_t)m * k / n);
    uint32_t hi = 1 + (uint32_t)((uint64_t)m * (k + 1) / n);

    for (j = lo; j < hi; j++) {
        uint32_t i = s_xf_run[j];
        if (!transform_vertex(g_pb.idx[i], &s_xf[i]))
            s_xf_failed = 1;
    }
}

/* Every vertex of the batch into s_xf. 0: the program did not run. */
static int transform_batch(uint32_t n)
{
    static int vsh_debug = -1;
    int cache = !nv2a_vsh_cxt_write();
    uint32_t i;

    static long mt_min = -1;
    if (vsh_debug < 0)
        vsh_debug = getenv("RECOMP_VSH_DUMP") || getenv("RECOMP_VSH_STEP");
    if (mt_min < 0) {                       /* RECOMP_VSH_MT_MIN=<runs> */
        const char *e = getenv("RECOMP_VSH_MT_MIN");
        mt_min = e ? atol(e) : NV_VSH_MT_MIN;
        if (mt_min < 2) mt_min = 2;
    }
    if (!cache) {
        for (i = 0; i < n; i++)
            if (!transform_vertex(g_pb.idx[i], &s_xf[i]))
                return 0;
        return 1;
    }
    if (++s_vc_now == 0) {                  /* stamp wrapped: forget all */
        memset(s_vc_stamp, 0, sizeof s_vc_stamp);
        s_vc_now = 1;
    }
    s_xf_nrun = 0;
    for (i = 0; i < n; i++) {
        uint32_t index = g_pb.idx[i], h = index & (NV_VCACHE_SIZE - 1);
        if (s_vc_stamp[h] == s_vc_now && s_vc_index[h] == index) {
            s_xf_from[i] = s_vc_slot[h];
            continue;
        }
        s_vc_stamp[h] = s_vc_now;
        s_vc_index[h] = index;
        s_vc_slot[h] = i;
        s_xf_from[i] = i;
        s_xf_run[s_xf_nrun++] = i;
    }
    if (!transform_vertex(g_pb.idx[s_xf_run[0]], &s_xf[s_xf_run[0]]))
        return 0;
    s_xf_failed = 0;
    if (s_xf_nrun >= (uint32_t)mt_min && raster_pool_size() > 1 && !vsh_debug)
        pool_run(xf_job);
    else
        xf_job(0, 1);
    if (s_xf_failed)
        return 0;
    for (i = 0; i < n; i++)
        if (s_xf_from[i] != i) {
            s_xf[i] = s_xf[s_xf_from[i]];
            g_pb.verts_cached++;
        }
    return 1;
}

static void raster_batch_program(void)
{
    uint32_t i, n = g_pb.idx_count;

    if (!transform_batch(n)) {
        if (probe_init() && g_pb.flips == s_probe.from_flip)
            fprintf(stderr, "[PROBE-DRAW] draw %u prim %u verts %u: vertex"
                    " program did not run (no END), batch dropped\n",
                    g_pb.draws, g_pb.prim, n);
        return;                                        /* no program loaded */
    }
    g_pb.batches_program++;
    g_pb.verts_program += n;
    if (g_pb.blend_enable) {
        uint32_t pair = g_pb.blend_sfactor << 16 | (g_pb.blend_dfactor & 0xFFFF);
        int j;
        for (j = 0; j < g_pb.blend_npairs && g_pb.blend_pairs[j] != pair; j++)
            ;
        if (j == g_pb.blend_npairs && j < 16)
            g_pb.blend_pairs[g_pb.blend_npairs++] = pair;
    }
    {
        /* RECOMP_VSH_TRACE=<n>: print n program batches -- attribute setup,
         * raw inputs, key constants, and what came out. */
        static int left = -1;
        if (left < 0) {
            const char *t = getenv("RECOMP_VSH_TRACE");
            left = t ? atoi(t) : 0;
        }
        /* Spread out: one every 20000 program batches, so a run that spends
         * its first half in menus still traces the 3D scene. */
        if (left > 0 && n >= 3 && g_pb.batches_program % 20000 == 0) {
            float in[4];
            left--;
            fprintf(stderr, "[VTRACE] prim %u n %u idx %u %u %u\n", g_pb.prim,
                    n, g_pb.idx[0], g_pb.idx[1], g_pb.idx[2]);
            for (i = 0; i < NV_VERTEX_ATTRS; i++) {
                const VertexAttr *at = &g_pb.attr[i];
                if (!at->size)
                    continue;
                pb_fetch_attr(at, g_pb.idx[0], in);
                fprintf(stderr, "[VTRACE]   v%u off %08X type %u size %u"
                        " stride %u = %g %g %g %g\n", i, at->offset, at->type,
                        at->size, at->stride, in[0], in[1], in[2], in[3]);
            }
            {
                static const uint32_t cs[] = {58, 59, 96, 97, 112, 113, 114, 115};
                for (i = 0; i < 8; i++) {
                    const float *c = nv2a_vsh_constant(cs[i]);
                    fprintf(stderr, "[VTRACE]   c[%u] %g %g %g %g%c", cs[i],
                            c[0], c[1], c[2], c[3], 10);
                }
            }
            for (i = 0; i < 3; i++)
                fprintf(stderr, "[VTRACE]   out%u pos %g %g %g %g\n", i,
                        s_xf[i].pos[0], s_xf[i].pos[1], s_xf[i].pos[2],
                        s_xf[i].pos[3]);
        }
    }
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        const VertexAttr *at = &g_pb.attr[i];
        uint32_t f = at->type | (at->size << 4) | (i << 8), j;
        if (!at->size || !at->stride)
            continue;
        for (j = 0; j < (uint32_t)g_pb.xf_nfmt && g_pb.xf_fmt[j] != f; j++)
            ;
        if (j == (uint32_t)g_pb.xf_nfmt && g_pb.xf_nfmt < 16)
            g_pb.xf_fmt[g_pb.xf_nfmt++] = f;
    }
    if (g_pb.texs[0].valid)
        note_texture_use();

    /* The probe's first frame also gets one line per batch: where its
     * triangles went, and how many vertices came out of the program as
     * NaN or infinity -- a triangle dropped before the rasteriser never
     * reaches the pixel probe. */
    if (probe_init() && g_pb.flips == s_probe.from_flip) {
        uint32_t drawn = g_pb.xf_drawn, degen = g_pb.xf_degenerate;
        uint32_t off = g_pb.xf_offscreen, behind = g_pb.tris_behind;
        uint32_t bad = 0, k;
        for (i = 0; i < n; i++)
            for (k = 0; k < 4; k++)
                if (!isfinite(s_xf[i].pos[k])) {
                    bad++;
                    break;
                }
        raster_xf_prims(n);
        fprintf(stderr, "[PROBE-DRAW] draw %u prim %u verts %u (%u NaN/inf):"
                " %u drawn, %u degenerate/NaN, %u off, %u behind  tex0 %s"
                " fmt %02X  blend %d %X/%X  depth %d mask %d\n", g_pb.draws,
                g_pb.prim, n, bad, g_pb.xf_drawn - drawn,
                g_pb.xf_degenerate - degen, g_pb.xf_offscreen - off,
                g_pb.tris_behind - behind, g_pb.texs[0].valid ? "on" : "off",
                g_pb.texs[0].color, g_pb.blend_enable, g_pb.blend_sfactor,
                g_pb.blend_dfactor, g_pb.depth_test, g_pb.depth_mask);
        return;
    }
    raster_xf_prims(n);
}

/* Fixed-function screen-space vertices for the combiner path.
 *
 * A batch drawn with the transform engine in fixed-function mode carries
 * pixel coordinates already, and raster_triangle used to fill it with one
 * colour or the raw stage-0 texel: no vertex colour, no combiners, no stages
 * 1-3. That is right for a menu quad and wrong for a post-process pass.
 * Conker's frontend glow downsamples the frame through four taps (t0..t3,
 * one texcoord set each, averaged by the combiners), adds it back with the
 * vertex colour's alpha (0.2) as strength, and blurs it with weighted taps;
 * drawn as raw texels at alpha 1 every pass added the whole frame, and the
 * intro movie went white within a few frames.
 *
 * So once the title has programmed the combiners, such a batch is built
 * into the same per-vertex form a vertex program produces -- position with
 * w = 1, colour in d0, one texcoord per stage -- and rasterised by the path
 * that already evaluates the combiners per pixel. Which attribute feeds a
 * stage follows the slot convention (texcoord n in slot 9+n) and, absent
 * that, the float attributes after the position in slot order: the layouts
 * Conker and Half-Life 2 use put colour and texcoords in low slots. */
static int fixed_texcoord_slot(uint32_t stage, uint32_t colour_slot)
{
    uint32_t a, seen = 0;

    if (9 + stage < NV_VERTEX_ATTRS && g_pb.attr[9 + stage].size
            && g_pb.attr[9 + stage].stride)
        return (int)(9 + stage);
    for (a = 1; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &g_pb.attr[a];
        if (a == colour_slot || !at->size || !at->stride || at->type != 2)
            continue;
        if (seen++ == stage)
            return (int)a;
    }
    return -1;
}

static void fixed_vertex(uint32_t index, Nv2aVshOutput *out)
{
    const VertexAttr *ca = color_attr();
    uint32_t colour_slot = (uint32_t)(ca - g_pb.attr), st;
    float c[4];

    memset(out, 0, sizeof *out);
    pb_fetch_attr(&g_pb.attr[0], index, out->pos);
    out->pos[3] = 1.0f;
    if (!pb_fetch_attr(ca, index, c) && !constant_color(c))
        c[0] = c[1] = c[2] = c[3] = 1.0f;
    memcpy(out->d0, c, sizeof c);
    for (st = 0; st < 4; st++) {
        int slot = fixed_texcoord_slot(st, colour_slot);
        if (slot >= 0)
            pb_fetch_attr(&g_pb.attr[slot], index, out->tex[st]);
    }
}

static void raster_xf_prims(uint32_t n)
{
    uint32_t i;

    switch (g_pb.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++)
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_xf_clipped(&s_xf[0], &s_xf[i], &s_xf[i+1]);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+2]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+2], &s_xf[i+3]);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_xf_clipped(&s_xf[i], &s_xf[i+1], &s_xf[i+3]);
            raster_xf_clipped(&s_xf[i], &s_xf[i+3], &s_xf[i+2]);
        }
        break;
    default:
        break;
    }
    triq_flush();       /* draw state may change after the batch */
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = g_pb.tris_drawn;

    if (g_pb.idx_count < 3)
        return;
    /* The queue of the batch before has been drawn by now, so a zeta
     * surface sampled here is complete; and from here on this batch's own
     * depth and stencil writes make the guest copy stale. */
    for (i = 0; i < 4; i++)
        if (g_pb.texs[i].valid)
            zeta_readback(&g_pb.texs[i]);
    if (g_pb.depth_mask || g_pb.stencil_test)
        zbuf_touch();
    static int no_vsh = -1;
    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    if ((g_pb.xf_mode & 3) == 2 && !no_vsh) {
        raster_batch_program();
        return;
    }
    if (probe_init() && g_pb.flips == s_probe.from_flip)
        fprintf(stderr, "[PROBE-DRAW] draw %u prim %u verts %u: fixed-function"
                " (mode %u), %s\n", g_pb.draws, g_pb.prim, g_pb.idx_count,
                g_pb.xf_mode, batch_is_screen_space() ? "screen-space"
                : "not screen-space, dropped");
    if (!batch_is_screen_space()) {
        g_pb.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            g_pb.batches_no_uv++;
        else if (!g_pb.texs[0].valid)
            g_pb.batches_no_tex++;
        else {
            g_pb.batches_textured++;
            note_texture_use();
        }
    }

    /* Combiners programmed: per-pixel colour, all four stages (see
     * fixed_vertex). RECOMP_NO_COMBINERS keeps the old flat path. */
    static int no_rc = -1;
    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    if (g_pb.rc_seen && !no_rc) {
        for (i = 0; i < g_pb.idx_count; i++)
            fixed_vertex(g_pb.idx[i], &s_xf[i]);
        raster_xf_prims(g_pb.idx_count);
    } else switch (g_pb.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < g_pb.idx_count; i += 3)
            raster_indexed(g_pb.idx[i], g_pb.idx[i+1], g_pb.idx[i+2],
                           vertex_color(g_pb.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < g_pb.idx_count; i++)
            raster_indexed(g_pb.idx[i], g_pb.idx[i+1], g_pb.idx[i+2],
                           vertex_color(g_pb.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < g_pb.idx_count; i++)
            raster_indexed(g_pb.idx[0], g_pb.idx[i], g_pb.idx[i+1],
                           vertex_color(g_pb.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Independent quads, four vertices each. A batch of eight is two
         * quads, not one six-triangle fan around the first vertex; with
         * exactly four the two agreed, which is why sharing the fan arm
         * looked right. */
        for (i = 0; i + 3 < g_pb.idx_count; i += 4) {
            raster_indexed(g_pb.idx[i], g_pb.idx[i+1], g_pb.idx[i+2],
                           vertex_color(g_pb.idx[i]));
            raster_indexed(g_pb.idx[i], g_pb.idx[i+2], g_pb.idx[i+3],
                           vertex_color(g_pb.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Each vertex pair past the first closes another quad against the
         * pair before it. */
        for (i = 0; i + 3 < g_pb.idx_count; i += 2) {
            raster_indexed(g_pb.idx[i], g_pb.idx[i+1], g_pb.idx[i+3],
                           vertex_color(g_pb.idx[i]));
            raster_indexed(g_pb.idx[i], g_pb.idx[i+3], g_pb.idx[i+2],
                           vertex_color(g_pb.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (g_pb.tris_drawn && (g_pb.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", g_pb.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (g_pb.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW
        && !s_flip_dumping) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* ── The back end ─────────────────────────────────────────────────────── */

static void sw_clear(uint32_t param)
{
    clear_surface(param);
    if (param & 0x01)                         /* Z */
        zbuf_clear();
    if (param & 0x02)                         /* stencil */
        sbuf_clear();
}

static void sw_draw(void)
{
    uint32_t t0 = g_pb.tris_drawn;
    uint64_t p0 = g_pb.pixels;
    raster_batch();
    if (g_pb_ftrace == 2) {
        fprintf(stderr, "[FTRACE] %s prim %u n %u surf %08X %ux%u+%u+%u"
                " pitch %u | tex %s %08X %ux%u fmt %02X | blend %u %X/%X"
                " eq %X | z test %u func %X mask %u | tris %u px %llu\n",
                (g_pb.xf_mode & 3) == 2 ? "PRG" : "FIX", g_pb.prim,
                g_pb.idx_count, g_pb.color_offset, g_pb.clip_w,
                g_pb.clip_h, g_pb.clip_x, g_pb.clip_y, g_pb.pitch,
                g_pb.texs[0].valid ? "on" : "off", g_pb.texs[0].offset,
                g_pb.texs[0].width, g_pb.texs[0].height, g_pb.texs[0].color,
                g_pb.blend_enable, g_pb.blend_sfactor,
                g_pb.blend_dfactor, g_pb.blend_equation,
                g_pb.depth_test, g_pb.depth_func, g_pb.depth_mask,
                g_pb.tris_drawn - t0,
                (unsigned long long)(g_pb.pixels - p0));
        if (g_pb.stencil_test || g_pb.alpha_test)
            fprintf(stderr, "[FTRACE]     stencil %u func %X ref %02X"
                    " mask r %02X w %02X op %X/%X/%X | alpha test %u"
                    " func %X ref %02X\n", g_pb.stencil_test,
                    g_pb.stencil_func, g_pb.stencil_ref & 0xFF,
                    g_pb.stencil_rmask & 0xFF, g_pb.stencil_wmask & 0xFF,
                    g_pb.stencil_op_fail, g_pb.stencil_op_zfail,
                    g_pb.stencil_op_zpass, g_pb.alpha_test,
                    g_pb.alpha_func, g_pb.alpha_ref & 0xFF);
        {
            /* What the batch did to the picture, not just how many
             * pixels it touched: the target's mean colour over the clip
             * after the batch, then every attribute with an array and
             * its value at vertex 0, and the colour constant (slot 3's
             * SET_VERTEX_DATA value) an array-less colour takes. A
             * post-process chain that ends white names its culprit as
             * the first batch whose mean jumps, and these say where
             * that batch's strength came from. */
            float d[4];
            uint32_t mr, mg, mb, mx, at;
            surface_mean(&mr, &mg, &mb, &mx);
            fprintf(stderr, "[FTRACE]     after: mean rgb %u %u %u max %u"
                    " | colour const %s %.3f %.3f %.3f %.3f%c",
                    mr, mg, mb, mx,
                    (g_pb.imm_set & (1u << 3)) ? "set" : "unset",
                    g_pb.imm_attr[3][0], g_pb.imm_attr[3][1],
                    g_pb.imm_attr[3][2], g_pb.imm_attr[3][3], 10);
            for (at = 0; at < NV_VERTEX_ATTRS; at++) {
                const VertexAttr *va = &g_pb.attr[at];
                if (!va->size || !va->stride)
                    continue;
                pb_fetch_attr(va, g_pb.idx[0], d);
                fprintf(stderr, "[FTRACE]       attr %u type %u size %u"
                        " stride %u: v0 %g %g %g %g%c", at, va->type,
                        va->size, va->stride, d[0], d[1], d[2], d[3], 10);
            }
        }
        if ((g_pb.xf_mode & 3) == 2)
            fprintf(stderr, "[FTRACE]     v0 %g %g %g %g  v1 %g %g  v2 %g %g%c",
                    s_xf[0].pos[0], s_xf[0].pos[1], s_xf[0].pos[2],
                    s_xf[0].pos[3], s_xf[1].pos[0], s_xf[1].pos[1],
                    s_xf[2].pos[0], s_xf[2].pos[1], 10);
        if (g_pb.rc_seen) {
            int st;
            fprintf(stderr, "[FTRACE]     rc stages %u ctl %08X prog %05X fin %08X %08X",
                    g_pb.rc.control & 0xFF, g_pb.rc.control,
                    g_pb.rc.stage_program, g_pb.rc.final0, g_pb.rc.final1);
            for (st = 0; st < 4; st++)
                if ((g_pb.rc.stage_program >> (st * 5)) & 0x1F)
                    fprintf(stderr, " | t%d %08X %ux%u fmt %02X%s", st,
                            g_pb.texs[st].offset, g_pb.texs[st].width,
                            g_pb.texs[st].height, g_pb.texs[st].color,
                            g_pb.texs[st].valid ? "" : " (invalid)");
            fputc(10, stderr);
            for (st = 0; st < (int)(g_pb.rc.control & 0xFF) && st < 8; st++)
                fprintf(stderr, "[FTRACE]       s%d icw %08X %08X ocw %08X %08X c %08X %08X%c",
                        st, g_pb.rc.color_icw[st], g_pb.rc.alpha_icw[st],
                        g_pb.rc.color_ocw[st], g_pb.rc.alpha_ocw[st],
                        g_pb.rc.factor0[st], g_pb.rc.factor1[st], 10);
        }
        if ((g_pb.xf_mode & 3) == 2) {
            uint32_t r;
            for (r = 112; r < 116; r++) {
                const float *c = nv2a_vsh_constant(r);
                fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r,
                        c[0], c[1], c[2], c[3], 10);
            }
        }
    }
}

/* Hand the window a copy of the frame just finished.
 *
 * The buffer the title has finished is the one the last batch drew into,
 * which is what drawn_offset holds and why it exists: by the flip,
 * color_offset has already moved to the next buffer. Copying here, rather
 * than letting the window read guest memory on its own clock, is what stops
 * it showing a surface the rasteriser is still writing. */
static void sw_present(void)
{
    if (g_pb.pitch) {
        extern void xbox_FramebufferWindowPresent(uint32_t, uint32_t);
        uint32_t done = g_pb.drawn_offset ? g_pb.drawn_offset
                                           : g_pb.color_offset;
        uint32_t pitch = g_pb.drawn_offset ? g_pb.drawn_pitch
                                            : g_pb.pitch;
        if (done) {
            xbox_FramebufferWindowSet(pb_dma_resolve(done), pitch);
            xbox_FramebufferWindowPresent(pb_dma_resolve(done), pitch);
        }
    }
    {
        /* RECOMP_FB_DUMP_FLIPS: dump every presented frame, not one per
         * report -- a consecutive sequence, which is what a headless
         * recording (frames -> ffmpeg) needs. "1" dumps from boot; any
         * other value names a file, and frames are dumped while it
         * exists, so a capture can start mid-game without writing
         * gigabytes of menus first. */
        static const char *spec = (const char *)-1;
        if (spec == (const char *)-1)
            spec = getenv("RECOMP_FB_DUMP_FLIPS");
        if (spec) {
            int on = strcmp(spec, "1") == 0;
            if (!on) {
                FILE *f = fopen(spec, "rb");
                if (f) { fclose(f); on = 1; }
            }
            s_flip_dumping = on;
            if (on)
                dump_surface_bmp();
        }
    }
    g_pb.drawn_stale = 1;
}

static void sw_report(void)
{
    int i, j;

    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    if (!s_flip_dumping)
        dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", g_pb.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)g_pb.pixels, g_pb.drawn_offset,
            pb_dma_resolve(g_pb.drawn_offset), g_pb.color_offset,
            pb_dma_resolve(g_pb.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface; %u indices"
                    " dropped past %u per batch\n",
            g_pb.tris_drawn, g_pb.batches_untransformed,
            g_pb.tris_skipped_offscreen, g_pb.idx_dropped,
            (unsigned)NV_MAX_INDICES);
    fprintf(stderr, "[GPU] vertex programs: %u batches, %u vertices (%u from"
                    " the vertex cache); %u triangles dropped behind the eye\n",
            g_pb.batches_program, g_pb.verts_program, g_pb.verts_cached,
            g_pb.tris_behind);
    fprintf(stderr, "[GPU]   of the rest: %u degenerate/NaN, %u off-surface,"
                    " %u drawn; pixels %llu written, %llu depth-failed;"
                    " x %.0f..%.0f y %.0f..%.0f z %g..%g\n",
            g_pb.xf_degenerate, g_pb.xf_offscreen, g_pb.xf_drawn,
            (unsigned long long)g_pb.xf_pixels,
            (unsigned long long)g_pb.xf_depth_fail,
            g_pb.xf_min[0], g_pb.xf_max[0], g_pb.xf_min[1], g_pb.xf_max[1],
            g_pb.xf_min[2], g_pb.xf_max[2]);
    {
        fprintf(stderr, "[GPU]   blend pairs (src/dst):");
        for (j = 0; j < g_pb.blend_npairs; j++)
            fprintf(stderr, " %X/%X", g_pb.blend_pairs[j] >> 16,
                    g_pb.blend_pairs[j] & 0xFFFF);
        fputc(10, stderr);
        fprintf(stderr, "[GPU]   program attribute formats (slot:type/size):");
        for (j = 0; j < g_pb.xf_nfmt; j++)
            fprintf(stderr, " v%u:%u/%u", g_pb.xf_fmt[j] >> 8,
                    g_pb.xf_fmt[j] & 15, (g_pb.xf_fmt[j] >> 4) & 15);
        fprintf(stderr, "\n");
    }

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            g_pb.batches_textured, g_pb.batches_no_uv, g_pb.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);
}

int pb_probe_frame(void)
{
    return probe_init() && g_pb.flips == s_probe.from_flip;
}

const Nv2aPbBackend nv2a_pb_backend_sw = {
    "software", sw_clear, sw_draw, sw_present, sw_report
};
