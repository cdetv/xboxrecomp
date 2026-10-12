# window_aspect

The D3D11 back end's swap chain (640x480, DXGI_SCALING_STRETCH) used to sit on
the frame window itself, so dragging the window to another shape stretched the
picture. It now sits on a child view that the window keeps at the image's
aspect ratio, centred on black (`fb_present.c`, `xbox_FramebufferWindowGpuView`).
This checks the fitting: `xbox_FitAspect` gives the largest 4:3 rectangle that
fits, with bars on the long sides.

    cmake -S tests/window_aspect -B build/window-aspect -G Ninja
    cmake --build build/window-aspect
    ctest --test-dir build/window-aspect
