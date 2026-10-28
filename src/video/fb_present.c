/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* The window itself, for a GPU back end that presents into it with a swap
 * chain (nv2a_draw_d3d11.c). Once one has, GDI stops drawing: two presenters
 * on one window fight over every frame. */
static HWND          s_hwnd;
static volatile LONG s_gpu_owned;

void *xbox_FramebufferWindowHandle(void)
{
    return s_fb_running ? (void *)s_hwnd : NULL;
}

/* The largest w:h rectangle that fits a cw x ch client area, centred:
 * letterbox or pillarbox, never stretched. */
void xbox_FitAspect(int cw, int ch, int w, int h, int *x, int *y, int *fw, int *fh)
{
    if (cw <= 0 || ch <= 0 || w <= 0 || h <= 0) {
        *x = *y = 0; *fw = cw > 0 ? cw : 0; *fh = ch > 0 ? ch : 0;
        return;
    }
    if ((long long)cw * h > (long long)ch * w) {      /* wider: bars left/right */
        *fh = ch;
        *fw = (int)((long long)ch * w / h);
    } else {                                          /* taller: bars top/bottom */
        *fw = cw;
        *fh = (int)((long long)cw * h / w);
    }
    *x = (cw - *fw) / 2;
    *y = (ch - *fh) / 2;
}

/* A GPU back end's swap chain goes on a child window that the frame window
 * keeps at the image's aspect ratio, centred on black. The swap chain's own
 * DXGI_SCALING_STRETCH then scales evenly: a window dragged to any shape
 * shows the picture bigger or smaller, never squashed. (Aspect-keeping
 * scaling is not available to swap chains on an HWND.) The child is
 * disabled, so clicks and keys stay with the frame window. */
#define FB_WM_MAKE_GPU_VIEW (WM_APP + 1)
static HWND s_gpu_view;
static int  s_aspect_w = 4, s_aspect_h = 3;

/* Per-monitor DPI aware, so a W x H picture is W x H screen pixels. Without
 * it Windows draws the window at 96 DPI and scales it up on a monitor set to
 * 125 % or more: a 4K picture would be shrunk and blown up again, blurred.
 * Only with RECOMP_RENDER_RES, so the default window keeps its size. Looked
 * up by name: Windows 10 1703 and later. */
static void fb_dpi_aware(void)
{
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleA("user32.dll");
    SetCtx set = u ? (SetCtx)(void *)GetProcAddress(u,
                         "SetProcessDpiAwarenessContext") : NULL;
    if (set)
        set((HANDLE)(INT_PTR)-4);       /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
}

/* RECOMP_RENDER_RES=<W>x<H>, or "native" for the primary monitor's size: the
 * D3D11 back end draws the title's 640x480 frame at W x H, each axis scaled
 * on its own (1920x1080 is 3 x 2.25), and the window shows it at W:H. 1 and
 * the size when set; 0 when unset or out of range (the frame stays 640x480).
 * The upper limits keep a scaled 1024 x 1024 depth buffer inside D3D11's
 * 16384-texel textures. Every thread computes the same answer, so a race on
 * the first call only does the work twice.
 *
 * xbox_SetRenderRes changes it while the title runs (a settings menu): the
 * D3D11 back end follows at its next present (nv2a_draw_d3d11.c,
 * apply_render_res), and anything asking here sees the new size at once.
 * Width and height live in one 64-bit word, so no reader sees half of a
 * change; 0 = unset. */
static volatile LONG64 s_render_res = -1;   /* -1: environment not read yet */

static int render_res_ok(unsigned a, unsigned b)
{
    return a >= 320 && b >= 240 && a <= 10240 && b <= 7680;
}

int xbox_RenderRes(unsigned *w, unsigned *h)
{
    LONG64 v = s_render_res;

    if (v < 0) {
        const char *e = getenv("RECOMP_RENDER_RES");
        unsigned a = 0, b = 0;
        if (e && *e) {
            fb_dpi_aware();
            if (_stricmp(e, "native") == 0) {
                a = (unsigned)GetSystemMetrics(SM_CXSCREEN);
                b = (unsigned)GetSystemMetrics(SM_CYSCREEN);
            } else if (sscanf(e, "%ux%u", &a, &b) != 2) {
                a = b = 0;
            }
            if (!render_res_ok(a, b)) {
                fprintf(stderr, "  [FBWIN] RECOMP_RENDER_RES=%s ignored: want"
                        " WxH from 320x240 to 10240x7680, or native\n", e);
                a = b = 0;
            }
        }
        v = (LONG64)a << 32 | b;
        /* A change made before the first read wins over the environment. */
        InterlockedCompareExchange64(&s_render_res, v, -1);
        v = s_render_res;
    }
    if (v) {
        *w = (unsigned)(v >> 32);
        *h = (unsigned)(v & 0xFFFFFFFF);
    }
    return v != 0;
}

/* w x h from now on, or 0 x 0 for none (the title's own 640x480). 1 when
 * taken, 0 when out of range (nothing changes). A process that started
 * without RECOMP_RENDER_RES is not DPI aware (fb_dpi_aware must come before
 * the window), so there a bigger picture is scaled up by Windows on a
 * monitor set above 100 %. */
int xbox_SetRenderRes(unsigned w, unsigned h)
{
    if ((w || h) && !render_res_ok(w, h))
        return 0;
    InterlockedExchange64(&s_render_res, (LONG64)w << 32 | h);
    return 1;
}

/* The shape the picture is shown at, when something other than the frame's
 * own 4:3 sets it: the render resolution's (any shape: 16:10, 21:9...), or
 * 16:9 for RECOMP_WIDESCREEN=1 (the title draws a 16:9 view squeezed into
 * its 4:3 frame, anamorphic, as for a widescreen TV). A title-side hook
 * widens the title's own view to match (Conker: sub_000645C0_enter). */
int xbox_DisplayShape(unsigned *w, unsigned *h)
{
    const char *e;
    if (xbox_RenderRes(w, h))
        return 1;
    e = getenv("RECOMP_WIDESCREEN");
    if (e && atoi(e)) {
        *w = 16;
        *h = 9;
        return 1;
    }
    *w = 4;
    *h = 3;
    return 0;
}

static void fb_layout_gpu_view(HWND parent)
{
    RECT c;
    int x, y, w, h;
    if (!s_gpu_view || !GetClientRect(parent, &c))
        return;
    if (c.right <= 0 || c.bottom <= 0)                 /* minimised */
        return;
    xbox_FitAspect(c.right, c.bottom, s_aspect_w, s_aspect_h, &x, &y, &w, &h);
    MoveWindow(s_gpu_view, x, y, w, h, TRUE);
}

void *xbox_FramebufferWindowGpuView(unsigned w, unsigned h)
{
    HWND parent = s_fb_running ? s_hwnd : NULL;
    if (!parent)
        return NULL;
    if (!s_gpu_view)   /* made on the window's own thread, which owns it */
        SendMessageA(parent, FB_WM_MAKE_GPU_VIEW, (WPARAM)w, (LPARAM)h);
    return (void *)s_gpu_view;
}

void xbox_FramebufferWindowGpuOwned(int on)
{
    InterlockedExchange(&s_gpu_owned, on ? 1 : 0);
}

/* The picture's shape changed (xbox_SetRenderRes, applied by the GPU back
 * end): the view takes xbox_DisplayShape's new shape, or w:h when that has
 * none. Posted: the view belongs to the window's own thread. */
#define FB_WM_SET_SHAPE (WM_APP + 3)

void xbox_FramebufferWindowShapeChanged(unsigned w, unsigned h)
{
    if (s_fb_running && s_hwnd)
        PostMessageA(s_hwnd, FB_WM_SET_SHAPE, (WPARAM)w, (LPARAM)h);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Borderless full screen: F11 or Alt+Enter, or RECOMP_FULLSCREEN=1 at start.
 *
 * The window loses its frame and covers the monitor it is on; the picture
 * keeps its aspect ratio through WM_SIZE like any other window size. Not
 * DXGI's exclusive full screen (the swap chain says DXGI_MWA_NO_ALT_ENTER):
 * no display-mode change, so alt-tabbing is instant and the swap chain is
 * never lost. Leaving it puts back the old style and placement. */
static int             s_fullscreen;
static LONG            s_saved_style;
static WINDOWPLACEMENT s_saved_place;

/* Full screen from another thread (a settings overlay): posted, because the
 * style change belongs to the window's own thread. */
#define FB_WM_SET_FULLSCREEN (WM_APP + 2)

void xbox_FramebufferWindowSetFullscreen(int on)
{
    if (s_fb_running && s_hwnd)
        PostMessageA(s_hwnd, FB_WM_SET_FULLSCREEN, on ? 1 : 0, 0);
}

int xbox_FramebufferWindowIsFullscreen(void)
{
    return s_fullscreen;
}

/* Where the GPU view sits in the frame window's client area: a host overlay
 * drawn into the swap chain needs it to turn mouse positions (reported to
 * the frame window) into back-buffer pixels. 0 while there is no view. */
int xbox_FramebufferWindowGpuViewRect(int *x, int *y, int *w, int *h)
{
    RECT r;
    if (!s_gpu_view || !s_hwnd || !GetWindowRect(s_gpu_view, &r))
        return 0;
    MapWindowPoints(NULL, s_hwnd, (POINT *)&r, 2);
    *x = r.left;
    *y = r.top;
    *w = r.right - r.left;
    *h = r.bottom - r.top;
    return *w > 0 && *h > 0;
}

/* A host-side hook that sees every window message first (a settings
 * overlay). It returns nonzero when it handled the message, with the
 * window procedure's result in *result; the window then does nothing else
 * with it. Runs on the window thread. */
typedef int (*xbox_WindowMessageHook)(void *hwnd, unsigned msg, uintptr_t w,
                                      intptr_t l, intptr_t *result);
static volatile xbox_WindowMessageHook s_msg_hook;

void xbox_SetWindowMessageHook(xbox_WindowMessageHook hook)
{
    s_msg_hook = hook;
}

static void fb_set_fullscreen(HWND h, int on)
{
    if (on == s_fullscreen)
        return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        s_saved_place.length = sizeof(s_saved_place);
        if (!GetWindowPlacement(h, &s_saved_place) ||
            !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        s_saved_style = GetWindowLongA(h, GWL_STYLE);
        SetWindowLongA(h, GWL_STYLE,
                       (s_saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(h, GWL_STYLE, s_saved_style);
        SetWindowPlacement(h, &s_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    s_fullscreen = on;
    fprintf(stderr, "  [FBWIN] %s full screen\n", on ? "entered" : "left");
    fflush(stderr);
}

/* Keyboard and mouse as player 1's pad (src/input/kbm_pad.c, once the game
 * has bound them): every key, mouse button and wheel notch goes there as
 * well as to s_key_down, and mouse movement comes from raw input, which
 * has no pointer acceleration and keeps counting at the screen's edge.
 *
 * While they are in use the mouse is held in the window: the pointer is
 * hidden and kept inside the client area, so it cannot wander onto another
 * monitor or click another window. Held only while the window has the
 * focus and nothing on the host side wants the pointer (an overlay menu
 * open, the game held). In a window, it takes a click in the picture to
 * hold the mouse (the click goes no further), so a window that opens under
 * the pointer -- a test run started from somewhere else -- does not trap it;
 * in full screen, getting the focus is enough. Alt+Tab lets go. */
#define FB_WM_KBM (WM_APP + 4)

extern int  xbox_KbmConfigured(void);
extern int  xbox_KbmWantsMouse(void);
extern void xbox_KbmSetWake(void (*wake)(void));
extern void xbox_KbmKey(unsigned input, int down);
extern void xbox_KbmWheel(int notches);
extern void xbox_KbmMouseMove(long dx, long dy);
extern void xbox_KbmReleaseAll(void);

static int s_mouse_held;      /* pointer clipped and hidden */
static int s_mouse_clicked;   /* windowed: a click asked for it */
static int s_wheel_rest;      /* part of a notch (high-resolution wheels) */

static void kbm_wake(void)
{
    if (s_hwnd)
        PostMessageA(s_hwnd, FB_WM_KBM, 0, 0);
}

static void kbm_clip(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    MapWindowPoints(h, NULL, (POINT *)&r, 2);
    ClipCursor(&r);
}

static void kbm_follow(HWND h)
{
    int want = xbox_KbmWantsMouse() && GetForegroundWindow() == h
               && (s_fullscreen || s_mouse_clicked);
    if (want == s_mouse_held)
        return;
    s_mouse_held = want;
    if (want) {
        kbm_clip(h);
        SetCursor(NULL);
    } else {
        ClipCursor(NULL);
        /* Keys stay as they are; only the mouse is let go. */
        xbox_KbmKey(VK_LBUTTON, 0);
        xbox_KbmKey(VK_RBUTTON, 0);
        xbox_KbmKey(VK_MBUTTON, 0);
        xbox_KbmKey(VK_XBUTTON1, 0);
        xbox_KbmKey(VK_XBUTTON2, 0);
    }
    fprintf(stderr, "  [FBWIN] mouse %s\n", want ? "held in the window"
                                                 : "let go");
    fflush(stderr);
}

static void kbm_register(HWND h)
{
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;     /* generic desktop */
    rid.usUsage     = 0x02;     /* mouse */
    rid.dwFlags     = 0;        /* the usual WM_MOUSE* messages too */
    rid.hwndTarget  = h;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        fprintf(stderr, "  [FBWIN] raw mouse input not available (%lu)\n",
                (unsigned long)GetLastError());
    xbox_KbmSetWake(kbm_wake);
}

/* A mouse button message: to the pad while the mouse is held; a click that
 * asks to hold it is used up. Returns nonzero when handled. */
static int kbm_button(HWND h, UINT m, WPARAM w)
{
    unsigned vk;
    int down;

    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: vk = VK_MBUTTON; break;
    default:
        vk = HIWORD(w) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
        break;
    }
    down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN
        || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN;
    if (!xbox_KbmConfigured())
        return 0;
    if (down && !s_mouse_held) {
        if (m == WM_LBUTTONDOWN && xbox_KbmWantsMouse()) {
            s_mouse_clicked = 1;
            kbm_follow(h);
        }
        return 1;
    }
    xbox_KbmKey(vk, down);
    return 1;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    {
        xbox_WindowMessageHook hook = s_msg_hook;
        intptr_t result = 0;
        if (hook && m != FB_WM_MAKE_GPU_VIEW && hook(h, m, w, l, &result))
            return (LRESULT)result;
    }
    switch (m) {
    case WM_CLOSE:
        /* The X button or Alt+F4: quit, as the title's own quit does
         * (HalReturnToFirmware). Only closing the window left the game
         * running with nothing on screen. File writes go straight through
         * WriteFile, so nothing written so far is lost. */
        fprintf(stderr, "[EXIT] window closed\n");
        fflush(stderr);
        ExitProcess(0);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case FB_WM_MAKE_GPU_VIEW:
        if (!s_gpu_view) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc   = DefWindowProcA;
            wc.hInstance     = GetModuleHandleA(NULL);
            wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = "XboxRecompGpuView";
            RegisterClassA(&wc);
            if (w && l) {
                s_aspect_w = (int)w;
                s_aspect_h = (int)l;
            }
            {
                /* RECOMP_RENDER_RES / RECOMP_WIDESCREEN: their shape,
                 * whatever the surface size. */
                unsigned sw, sh;
                if (xbox_DisplayShape(&sw, &sh)) {
                    s_aspect_w = (int)sw;
                    s_aspect_h = (int)sh;
                }
            }
            s_gpu_view = CreateWindowExA(0, "XboxRecompGpuView", "",
                                         WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                         0, 0, 1, 1, h, NULL,
                                         GetModuleHandleA(NULL), NULL);
            fb_layout_gpu_view(h);
        }
        return 0;

    case WM_SIZE:
        fb_layout_gpu_view(h);
        /* Full screen on or off comes through here too. */
        kbm_follow(h);
        if (s_mouse_held)
            kbm_clip(h);
        return 0;

    case WM_MOVE:
        if (s_mouse_held)
            kbm_clip(h);
        break;

    case FB_WM_KBM:
        kbm_follow(h);
        return 0;

    case WM_ACTIVATE:
    case WM_SETFOCUS:
        kbm_follow(h);
        break;

    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (s_mouse_held
            && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size,
                               sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE
            && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            xbox_KbmMouseMove(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
        break;                    /* DefWindowProc cleans up after it */
    }

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        if (kbm_button(h, m, w))
            return (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) ? TRUE : 0;
        break;

    case WM_MOUSEWHEEL:
        if (s_mouse_held) {
            s_wheel_rest += GET_WHEEL_DELTA_WPARAM(w);
            if (s_wheel_rest >= WHEEL_DELTA || s_wheel_rest <= -WHEEL_DELTA) {
                xbox_KbmWheel(s_wheel_rest / WHEEL_DELTA);
                s_wheel_rest %= WHEEL_DELTA;
            }
            return 0;
        }
        break;

    /* Alt or F10 alone would put the window into its (absent) menu, and the
     * next keys would go to that instead of the game: not while Alt is a
     * key the game may use. Alt+Space and Alt+F4 still work. */
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0 && xbox_KbmConfigured())
            return 0;
        break;

    case FB_WM_SET_SHAPE: {
        unsigned sw, sh;
        if (xbox_DisplayShape(&sw, &sh)) {
            s_aspect_w = (int)sw;
            s_aspect_h = (int)sh;
        } else if (w && l) {
            s_aspect_w = (int)w;
            s_aspect_h = (int)l;
        }
        fb_layout_gpu_view(h);
        return 0;
    }

    case FB_WM_SET_FULLSCREEN:
        fb_set_fullscreen(h, w != 0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* The full-screen keys are the window's, not the game's: Enter is
         * the pad's Start, so Alt+Enter must not also pause. Bit 30 of l
         * is set on auto-repeat, which would toggle back and forth. */
        if (w == VK_F11 || (w == VK_RETURN && m == WM_SYSKEYDOWN &&
                            (HIWORD(l) & KF_ALTDOWN))) {
            if (!(l & (1L << 30)))
                fb_set_fullscreen(h, !s_fullscreen);
            return 0;
        }
        if (w == VK_F9) {                 /* trace this frame (nv2a_pb_exec.c) */
            extern volatile long g_pb_ftrace_key;
            if (!(l & (1L << 30)))
                g_pb_ftrace_key = 1;
            return 0;
        }
        if ((unsigned)w < 256) {
            s_key_down[w] = 1;
            xbox_KbmKey((unsigned)w, 1);
        }
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256) {
            s_key_down[w] = 0;
            xbox_KbmKey((unsigned)w, 0);
        }
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt+Enter's character: Windows would beep for it (no menu). */
    case WM_SYSCHAR:
        if (w == '\r')
            return 0;
        break;

    /* No mouse pointer over the picture in full screen. The GPU view is a
     * disabled child, so the mouse over it is reported here as client. */
    case WM_SETCURSOR:
        if ((s_fullscreen || s_mouse_held) && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        xbox_KbmReleaseAll();
        s_mouse_clicked = 0;
        kbm_follow(h);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);  /* the bars */
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
    {
        /* RECOMP_RENDER_RES: a window of the picture's shape, as big as
         * fits in 80 % of the screen's work area (no bigger than the
         * picture itself). Also makes the process DPI aware before the
         * window exists (xbox_RenderRes). */
        unsigned pw, ph;
        RECT wa;
        if (xbox_RenderRes(&pw, &ph)
            && SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            int x, y, fw, fh;
            int cw = (int)((wa.right - wa.left) * 4 / 5);
            int ch = (int)((wa.bottom - wa.top) * 4 / 5);
            if (cw > (int)pw) cw = (int)pw;
            if (ch > (int)ph) ch = (int)ph;
            xbox_FitAspect(cw, ch, (int)pw, (int)ph, &x, &y, &fw, &fh);
            if (fw > 0 && fh > 0) {
                r.right = fw;
                r.bottom = fh;
            }
        }
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    s_hwnd = hwnd;
    kbm_register(hwnd);
    if (getenv("RECOMP_FULLSCREEN") && atoi(getenv("RECOMP_FULLSCREEN")))
        fb_set_fullscreen(hwnd, 1);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_gpu_owned) {
            /* The GPU back end presents; this thread only pumps messages
             * and keeps the title bar. */
        } else if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        Sleep(16);
    }

    s_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void *xbox_FramebufferWindowHandle(void) { return NULL; }
void *xbox_FramebufferWindowGpuView(unsigned w, unsigned h) { (void)w; (void)h; return NULL; }
void xbox_FramebufferWindowGpuOwned(int on) { (void)on; }
void xbox_FitAspect(int cw, int ch, int w, int h, int *x, int *y, int *fw, int *fh)
{ (void)w; (void)h; *x = *y = 0; *fw = cw; *fh = ch; }
int xbox_RenderRes(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int xbox_SetRenderRes(unsigned w, unsigned h) { (void)w; (void)h; return 0; }
void xbox_FramebufferWindowShapeChanged(unsigned w, unsigned h) { (void)w; (void)h; }
int xbox_DisplayShape(unsigned *w, unsigned *h) { *w = 4; *h = 3; return 0; }
void xbox_FramebufferWindowSetFullscreen(int on) { (void)on; }
int xbox_FramebufferWindowIsFullscreen(void) { return 0; }
int xbox_FramebufferWindowGpuViewRect(int *x, int *y, int *w, int *h)
{ (void)x; (void)y; (void)w; (void)h; return 0; }
void xbox_SetWindowMessageHook(void *hook) { (void)hook; }
#endif
