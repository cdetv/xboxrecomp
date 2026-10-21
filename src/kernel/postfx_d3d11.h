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
 * draws the frame through postfx_draw instead of copying it. */
int postfx_active(void);

/* The rectangle sx0,sy0 sw x sh (host pixels) of texture src, filtered,
 * over the rectangle dx0,dy0 dw x dh of target dst. Same size: pixel for
 * pixel; otherwise sampled bilinearly. Sets every piece of pipeline state it
 * uses and leaves no target or view bound. 1 when drawn; 0 when the chain
 * could not be made (said once), and the caller copies as before. */
int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh);

#endif

#endif
