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

#endif

#endif
