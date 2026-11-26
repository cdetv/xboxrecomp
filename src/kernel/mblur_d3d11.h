/**
 * Camera motion blur drawn by the host (D3D11 back end; mblur_d3d11.c): a
 * host effect (NV_HOST_FX_MBLUR, nv2a_pb_state.h) a game asks for after its
 * 3D picture and before its HUD, with how the camera moved since the last
 * frame, so each pixel is smeared along the path its point took on screen.
 */
#ifndef MBLUR_D3D11_H
#define MBLUR_D3D11_H

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

/* Camera space is hostfx_view.h's: x right, y up, z the distance in front
 * (a right-handed camera that looks down -z sends its matrices with z
 * flipped). A point p of this frame's camera space was at R p + t in the
 * last frame's, R the rotation q. */
typedef struct {
    float q[4];             /* rotation, quaternion x y z w (unit) */
    float t[3];             /* translation after it (world units) */
    float zn, zf;           /* the camera's near and far planes */
    float sx, sy;           /* its projection's x / y scale (m00, m11) */
    float amount;           /* share of the frame-to-frame motion smeared */
    float max_blur;         /* the longest smear, guest pixels */
    float centre;           /* radius kept sharp, share of the half diagonal
                               (0 = none); it fades in over as much again */
    unsigned flags;         /* NV_HOST_MBLUR_* */
} MblurParams;

/* Blur the rectangle x0,y0 w x h (host pixels) of colour surface tex (its
 * view rtv) along each pixel's motion, from depth (an R24 view of its depth
 * buffer, same host scale); guest_h the rectangle's height in guest pixels
 * (max_blur is in those). Writes RGB only. Sets every piece of pipeline
 * state it uses and leaves no target or view bound. 1 when drawn; 0 when it
 * could not be (said once). */
int mblur_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
               ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
               ID3D11ShaderResourceView *depth,
               UINT x0, UINT y0, UINT w, UINT h, float guest_h,
               const MblurParams *p);

#endif

#endif
