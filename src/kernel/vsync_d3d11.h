/**
 * VSync for the D3D11 back end (vsync_d3d11.c): how a frame is presented,
 * and the monitor's refreshes driving the emulated vblank clock.
 * nv2a_draw_d3d11.c makes its swap chain with vsync_swap_flags and
 * presents every frame, held ones too, through vsync_present.
 */
#ifndef VSYNC_D3D11_H
#define VSYNC_D3D11_H

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <dxgi1_2.h>

/* The flags the swap chain is made with (and resized with): "may tear"
 * when the system supports it, so VSync can change live. Call once, before
 * the first vsync_present. */
UINT vsync_swap_flags(IDXGIFactory2 *factory);

/* Present the back buffer as the VSync mode says. */
HRESULT vsync_present(IDXGISwapChain1 *swap);

#endif

#endif
