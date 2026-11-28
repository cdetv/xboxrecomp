/**
 * The filter chain between the shown frame and the back buffer (D3D11 back
 * end; postfx_d3d11.c). nv2a_draw_d3d11.c calls it from shown_to_back, for
 * every present, held ones too; the host overlay draws after it, unfiltered.
 */
#ifndef POSTFX_D3D11_H
#define POSTFX_D3D11_H

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

/* Nonzero when any filter is on (or RECOMP_POSTFX_FORCE=1): the caller then
 * draws the frame through postfx_draw instead of copying it. A frame of
 * another size than the target goes through postfx_draw either way. */
int postfx_active(void);

/* The rectangle sx0,sy0 sw x sh (host pixels) of texture src, filtered,
 * over the rectangle dx0,dy0 dw x dh of target dst. Same size: pixel for
 * pixel; bigger: shrunk by area; smaller: stretched by the upscaler
 * (bilinear or FSR 1). Anti-aliasing runs at the source's size,
 * sharpening at the target's. Sets every piece of pipeline state it
 * uses and leaves no target or view bound. 1 when drawn; 0 when the chain
 * could not be made (said once), and the caller copies or stretches as
 * before. */
int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh);

/* A movie frame for the texture stage that samples it (nv2a_draw_d3d11.c
 * movie_stage): yuv holds its raw 4:2:2 words as an R8G8B8A8_UINT texture,
 * one texel per pixel pair (YUY2, or UYVY with POSTFX_MOVIE_UYVY), the
 * frame w x h; POSTFX_MOVIE_ROW_PAIRS: its chroma comes in row pairs (a
 * 4:2:0 source), blended down the rows too. Turned into colour with
 * interpolated chroma into dst at w x h when ow x oh is that size; else
 * stretched to ow x oh by FSR 1's EASU, then sharpened by its RCAS at
 * sharpen percent (as RECOMP_SHARPEN behind FSR; 0 = none). ow x oh is
 * never smaller than w x h. Sets every piece of pipeline state it uses and
 * leaves no target or view bound: the caller binds its own again. 1 when
 * drawn; 0 when a shader or texture could not be made. */
#define POSTFX_MOVIE_UYVY      1u
#define POSTFX_MOVIE_ROW_PAIRS 2u
int postfx_movie(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                 ID3D11ShaderResourceView *yuv, unsigned flags, UINT w,
                 UINT h, ID3D11RenderTargetView *dst, UINT ow, UINT oh,
                 float sharpen);

/* The HDR step (hdr_d3d11.c): the SDR picture and the overlay over it onto
 * an HDR back buffer. */
typedef struct {
    int   out;          /* 0 SDR (the mask view alone), 1 PQ, 2 scRGB */
    float paper;        /* nits: SDR white */
    float peak;         /* nits: the most the highlights reach */
    float stretch;      /* 0..1: how far they are pushed towards the peak */
    int   mask_view;    /* the 2D layer tinted red (debug) */
    ID3D11ShaderResourceView *sdr;      /* the chain's output, dst's size */
    ID3D11ShaderResourceView *ui;       /* the overlay, premultiplied; or NULL */
    ID3D11ShaderResourceView *frame;    /* the shown frame and its copy when */
    ID3D11ShaderResourceView *scene;    /* its 3D was done; NULL = all 2D */
    float map[4];       /* frame uv = dst pixel * map.xy + map.zw */
    float rect[4];      /* where the frame lies on dst: x0 y0 x1 y1 */
} PostfxHdr;

/* One full-screen draw over dst (w x h, the size of h->sdr and h->ui): the
 * SDR picture made linear (gamma 2.2) with SDR white at the paper white;
 * where the frame still matches its scene-end copy (the 3D picture), the
 * brightest part (from 80 % of white, by the brightest channel, hue kept)
 * stretched towards the peak; the 2D layer and frames with no copy at the
 * paper white; the overlay blended over it as over the SDR picture; then
 * BT.709 to BT.2020 and PQ (or scRGB). Sets every piece of pipeline state
 * it uses and leaves no target or view bound. 1 when drawn. */
int postfx_hdr(ID3D11Device *dev, ID3D11DeviceContext *ctx,
               const PostfxHdr *h, ID3D11RenderTargetView *dst, float w,
               float hgt);

#endif

#endif
