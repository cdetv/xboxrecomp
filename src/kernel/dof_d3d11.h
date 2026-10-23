/**
 * Depth of field drawn by the host (D3D11 back end; dof_d3d11.c): a host
 * effect (NV_HOST_FX_DOF, nv2a_pb_state.h) that a game asks for in place of
 * its own, at the same point of the frame.
 */
#ifndef DOF_D3D11_H
#define DOF_D3D11_H

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

typedef struct {
    float focus;            /* depth (0..1) in focus */
    float range;            /* depth distance from it to the most blur */
    float zn, zf;           /* the camera's near and far planes */
    int   style;            /* 0 = like the title's, 1 = cinematic */
    float max_blur;         /* the most blur, in guest pixels (radius) */
} DofParams;

/* Blur the rectangle x0,y0 w x h (host pixels) of colour surface tex (its
 * view rtv) by the depth in depth (an R24 view of its depth buffer, same
 * host scale), in place. guest_h: the rectangle's height in guest pixels
 * (blur sizes are in guest pixels). Sets every piece of pipeline state it
 * uses and leaves no target or view bound. 1 when drawn; 0 when it could
 * not be (said once). */
int dof_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
             ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
             ID3D11ShaderResourceView *depth, UINT x0, UINT y0, UINT w,
             UINT h, float guest_h, const DofParams *p);

#endif

#endif
