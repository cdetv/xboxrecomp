/**
 * Ambient occlusion drawn by the host (D3D11 back end; ao_d3d11.c): a host
 * effect (NV_HOST_FX_AO, nv2a_pb_state.h) a game asks for after its opaque
 * scenery, so creases and corners darken from the depth buffer.
 */
#ifndef AO_D3D11_H
#define AO_D3D11_H

/* Tuning for testing (a game's console): the most screen span of the radius
 * (share of the picture's height, 0.02-1), the brightness knee below which
 * it weakens (0-1, 0 = never), the darkest it may make a pixel (0-1); a
 * value out of range keeps the current one. */
void xbox_SetAoTuning(float span, float luma, float floor_);
void xbox_GetAoTuning(float *span, float *luma, float *floor_);

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

typedef struct {
    float zn, zf;           /* the camera's near and far planes */
    float sx, sy;           /* its projection's x / y scale (m00, m11) */
    float radius;           /* world units */
    float strength;         /* 0..1 */
    unsigned flags;         /* NV_HOST_AO_* */
    float fade_end;         /* distance where it is gone (starts at 0.3 x) */
} AoParams;

/* Darken the rectangle x0,y0 w x h (host pixels) of colour surface tex (its
 * view rtv) by the occlusion worked out from depth (an R24 view of its depth
 * buffer, same host scale) and stencil (its G8 view), in place. Sets every
 * piece of pipeline state it uses and leaves no target or view bound.
 * 1 when drawn; 0 when it could not be (said once). */
int ao_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
            ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
            ID3D11ShaderResourceView *depth, ID3D11ShaderResourceView *stencil,
            UINT x0, UINT y0, UINT w, UINT h, const AoParams *p);

#endif

#endif
