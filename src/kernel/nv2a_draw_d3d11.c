/**
 * The D3D11 back end of the pushbuffer executor (RECOMP_GPU=d3d11).
 *
 * The software back end (nv2a_draw_sw.c) draws every pixel on the CPU, which
 * caps an in-game frame at a few per second. This one hands the same work to
 * the real GPU: the front end (nv2a_pb_exec.c) still decodes the methods and
 * keeps the state, and this turns clears, batches and flips into D3D11 calls.
 *
 * Built up in steps, each checked against the software path at the same flip
 * (docs: GPU plan in the game repo's architecture notes). This is step 1:
 * the device, a swap chain on the framebuffer window, and colour clears into
 * one GPU texture per guest colour surface. Batches are counted, not drawn,
 * so what appears is the clear colour of the frame's main surface.
 *
 * Guest surfaces live on the GPU only. Nothing is written back to guest
 * memory, so a title that reads its own pixels back sees whatever was there
 * before; write-back comes with the surface cache proper.
 *
 * RECOMP_FB_DUMP + RECOMP_FB_DUMP_FLIPS dump presented frames exactly as the
 * software path does (24-bit BMPs, same numbering), read back from the GPU, so
 * scripts/compare_frames.py can put the two back ends side by side.
 *
 * RECOMP_GPU=both opens this as a shadow: the software back end keeps the
 * window and its dumps, and this one draws into its own textures only. At
 * every flip the software path dumps, this dumps the same rectangle of the
 * same surface as <RECOMP_FB_DUMP>gpu_NNNNN.bmp with the software dump's
 * number, so any frame of a run -- in-game too -- can be compared.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_pb_state.h"

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

extern void xbox_FramebufferWindowStart(void);
extern void *xbox_FramebufferWindowHandle(void);
extern void xbox_FramebufferWindowGpuOwned(int on);

/* The swap chain is the window's client area, which fb_present.c makes
 * 640x480; a bigger or smaller frame is scaled by DXGI. */
#define SC_W 640
#define SC_H 480

static ID3D11Device         *s_dev;
static ID3D11DeviceContext  *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;    /* ClearView: clears a rectangle */
static IDXGIFactory2        *s_factory;
static IDXGISwapChain1      *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static int                   s_swap_failed;
static int                   s_shadow;  /* RECOMP_GPU=both: no window */

/* One GPU texture per guest colour surface, found by its resolved address
 * and pitch. ponytail: always B8G8R8A8 whatever the guest format, and a
 * surface that turns out bigger than its texture is recreated empty -- both
 * fine while the only thing drawn is a clear. */
typedef struct {
    uint32_t addr, pitch;
    uint32_t w, h;
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
} GpuSurface;

#define GPU_SURFACES 32
static GpuSurface s_surf[GPU_SURFACES];
static int        s_surf_next;

/* The surface to present: the biggest one cleared since the last flip, as
 * note_drawn picks the drawn one in the software path (a title also clears
 * small render targets, often after the main one). */
static int      s_shown = -1, s_shown_stale = 1;
static uint32_t s_shown_x, s_shown_y, s_shown_w, s_shown_h;

static struct {
    uint32_t clears, clears_skipped, batches, presents, surfaces;
    uint32_t shadow_dumps, shadow_no_surface;
} s_stat;

static void fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "[GPU] d3d11: %s failed (hr 0x%08lX)%c", what,
            (unsigned long)hr, 10);
}

/* The swap chain, once the window exists. The window opens on its own thread
 * a moment after xbox_FramebufferWindowStart, so this is retried at every
 * present until it is there (or never, without RECOMP_FB_WINDOW). */
static int ensure_swap_chain(void)
{
    DXGI_SWAP_CHAIN_DESC1 d;
    ID3D11Texture2D *back = NULL;
    HWND hwnd;
    HRESULT hr;

    if (s_swap)
        return 1;
    if (s_swap_failed)
        return 0;
    hwnd = (HWND)xbox_FramebufferWindowHandle();
    if (!hwnd)
        return 0;

    memset(&d, 0, sizeof d);
    d.Width = SC_W;
    d.Height = SC_H;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(s_factory, (IUnknown *)s_dev,
                                              hwnd, &d, NULL, NULL, &s_swap);
    if (FAILED(hr)) {
        fail("CreateSwapChainForHwnd", hr);
        s_swap_failed = 1;
        return 0;
    }
    IDXGIFactory2_MakeWindowAssociation(s_factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    hr = IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D,
                                   (void **)&back);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back,
                                                 NULL, &s_back_rtv);
    if (back)
        ID3D11Texture2D_Release(back);
    if (FAILED(hr)) {
        fail("back buffer view", hr);
        s_swap_failed = 1;
        return 0;
    }
    /* From here on the window is ours; GDI would draw over every frame. */
    xbox_FramebufferWindowGpuOwned(1);
    fprintf(stderr, "[GPU] d3d11: swap chain on the framebuffer window"
            " (%ux%u)%c", SC_W, SC_H, 10);
    return 1;
}

/* The GPU texture for the current colour surface, made or grown to cover the
 * current clip: its index, or -1 when there is no surface to speak of. */
static int surface_current(void)
{
    uint32_t addr, w, h;
    int i;

    if (!g_pb.color_offset || !g_pb.pitch || !g_pb.clip_w || !g_pb.clip_h)
        return -1;
    addr = pb_dma_resolve(g_pb.color_offset);
    w = g_pb.clip_x + g_pb.clip_w;
    h = g_pb.clip_y + g_pb.clip_h;
    if (((g_pb.format >> 8) & 0xF) == 2) {        /* swizzled: size in format */
        uint32_t sw = 1u << ((g_pb.format >> 16) & 0xF);
        uint32_t sh = 1u << ((g_pb.format >> 24) & 0xF);
        if (w > sw) w = sw;
        if (h > sh) h = sh;
    }
    if (w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return -1;

    for (i = 0; i < GPU_SURFACES; i++)
        if (s_surf[i].tex && s_surf[i].addr == addr
            && s_surf[i].pitch == g_pb.pitch)
            break;
    if (i < GPU_SURFACES && s_surf[i].w >= w && s_surf[i].h >= h)
        return i;
    if (i == GPU_SURFACES) {
        /* Round robin over a table that, in practice, never fills. */
        i = s_surf_next++ % GPU_SURFACES;
        s_stat.surfaces++;
    } else {
        /* Grow: keep the larger of old and new in each direction. */
        if (s_surf[i].w > w) w = s_surf[i].w;
        if (s_surf[i].h > h) h = s_surf[i].h;
    }
    if (s_surf[i].rtv) ID3D11RenderTargetView_Release(s_surf[i].rtv);
    if (s_surf[i].tex) ID3D11Texture2D_Release(s_surf[i].tex);
    if (s_shown == i)
        s_shown = -1;
    memset(&s_surf[i], 0, sizeof s_surf[i]);
    {
        D3D11_TEXTURE2D_DESC d;
        HRESULT hr;
        memset(&d, 0, sizeof d);
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &s_surf[i].tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateRenderTargetView(s_dev,
                    (ID3D11Resource *)s_surf[i].tex, NULL, &s_surf[i].rtv);
        if (FAILED(hr)) {
            static int said;
            if (!said++)
                fail("surface texture", hr);
            if (s_surf[i].tex) ID3D11Texture2D_Release(s_surf[i].tex);
            memset(&s_surf[i], 0, sizeof s_surf[i]);
            return -1;
        }
    }
    s_surf[i].addr = addr;
    s_surf[i].pitch = g_pb.pitch;
    s_surf[i].w = w;
    s_surf[i].h = h;
    return i;
}

/* The clear value is in the surface's own pixel format (see clear_surface in
 * nv2a_draw_sw.c): a 16-bit surface takes its low half as R5G6B5. */
static void clear_rgba(float c[4])
{
    uint32_t v = g_pb.clear_color;

    switch (pb_surface_bpp()) {
    case 2:
        c[0] = (float)((v >> 11) & 0x1F) / 31.0f;
        c[1] = (float)((v >>  5) & 0x3F) / 63.0f;
        c[2] = (float)( v        & 0x1F) / 31.0f;
        c[3] = 1.0f;
        break;
    case 1:
        c[0] = c[1] = c[2] = (float)(v & 0xFF) / 255.0f;
        c[3] = 1.0f;
        break;
    default:
        c[0] = (float)((v >> 16) & 0xFF) / 255.0f;
        c[1] = (float)((v >>  8) & 0xFF) / 255.0f;
        c[2] = (float)( v        & 0xFF) / 255.0f;
        c[3] = (float)( v >> 24        ) / 255.0f;
        break;
    }
}

/* ponytail: colour only, and all four channels whatever the mask bits say.
 * Depth and stencil wait for depth buffers on the GPU. */
static void d3d_clear(uint32_t param)
{
    D3D11_RECT r;
    float c[4];
    int i;

    /* The window opens on the first clear, as in the software path. */
    xbox_FramebufferWindowStart();
    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;
    i = surface_current();
    if (i < 0) {
        s_stat.clears_skipped++;
        return;
    }
    clear_rgba(c);
    {
        /* RECOMP_GPU_CLEAR_TEST: every colour clear is red, then green,
         * then blue, a second each. A title that clears to black shows
         * nothing either way; this proves the clear reaches the window and
         * the dumps, in the right channel order. By the clock, not the flip
         * count: with nothing drawn a title's flips come in bursts. */
        static int test = -1;
        if (test < 0)
            test = getenv("RECOMP_GPU_CLEAR_TEST") != NULL;
        if (test) {
            uint32_t phase = (uint32_t)((GetTickCount64() / 1000) % 3);
            c[0] = phase == 0 ? 1.0f : 0.0f;
            c[1] = phase == 1 ? 1.0f : 0.0f;
            c[2] = phase == 2 ? 1.0f : 0.0f;
            c[3] = 1.0f;
        }
    }
    r.left = (LONG)g_pb.clip_x;
    r.top = (LONG)g_pb.clip_y;
    r.right = (LONG)(g_pb.clip_x + g_pb.clip_w);
    r.bottom = (LONG)(g_pb.clip_y + g_pb.clip_h);
    ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)s_surf[i].rtv, c,
                                   &r, 1);
    s_stat.clears++;

    if (s_shown_stale || s_shown < 0
        || g_pb.clip_w * g_pb.clip_h >= s_shown_w * s_shown_h) {
        s_shown = i;
        s_shown_stale = 0;
        s_shown_x = g_pb.clip_x; s_shown_y = g_pb.clip_y;
        s_shown_w = g_pb.clip_w; s_shown_h = g_pb.clip_h;
    }
}

static void d3d_draw(void)
{
    s_stat.batches++;
}

/* RECOMP_FB_DUMP_FLIPS, as in the software path: "1" dumps every presented
 * frame from boot, anything else names a flag file that switches it on. */
static int flip_dump_on(void)
{
    static const char *spec = (const char *)-1;
    FILE *f;

    if (spec == (const char *)-1)
        spec = getenv("RECOMP_FB_DUMP_FLIPS");
    if (!spec || !getenv("RECOMP_FB_DUMP"))
        return 0;
    if (strcmp(spec, "1") == 0)
        return 1;
    f = fopen(spec, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Rectangle x,y,w,h of surface i (or of nothing, i < 0), read back from the
 * GPU and written as a 24-bit BMP. What lies outside the surface's texture
 * comes out black. 1 when the file was written. */
static int dump_rect(const char *path, int i, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
    static const uint8_t black[4] = {0, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *staging = NULL;
    uint32_t cw = 0, ch = 0, row_bytes, pad, filesz, r, c;
    uint8_t hdr[54];
    FILE *f;

    if (!w || !h)
        return 0;
    if (i >= 0 && x < s_surf[i].w && y < s_surf[i].h) {
        cw = s_surf[i].w - x < w ? s_surf[i].w - x : w;
        ch = s_surf[i].h - y < h ? s_surf[i].h - y : h;
    }
    memset(&m, 0, sizeof m);
    if (cw && ch) {
        D3D11_TEXTURE2D_DESC d;
        D3D11_BOX box;
        memset(&d, 0, sizeof d);
        d.Width = cw;
        d.Height = ch;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &staging)))
            return 0;
        box.left = x; box.top = y; box.front = 0;
        box.right = x + cw; box.bottom = y + ch; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                (ID3D11Resource *)staging, 0, 0, 0, 0,
                (ID3D11Resource *)s_surf[i].tex, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging,
                                           0, D3D11_MAP_READ, 0, &m))) {
            ID3D11Texture2D_Release(staging);
            return 0;
        }
    }

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;
    f = fopen(path, "wb");
    if (f) {
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
        for (r = h; r-- > 0; ) {             /* BMP rows run bottom-up */
            const uint8_t *row = r < ch
                ? (const uint8_t *)m.pData + (size_t)r * m.RowPitch : NULL;
            for (c = 0; c < w; c++)                  /* B, G, R */
                fwrite(row && c < cw ? row + c * 4 : black, 1, 3, f);
            if (pad)
                fwrite(black, 1, pad, f);
        }
        fclose(f);
    }
    if (staging) {
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
        ID3D11Texture2D_Release(staging);
    }
    return f != NULL;
}

/* The presented rectangle, as <prefix>NNNNN.bmp. */
static void dump_shown(void)
{
    static int seq;
    char path[512];

    if (s_shown < 0 || !s_shown_w || !s_shown_h)
        return;
    snprintf(path, sizeof path, "%s%05d.bmp", getenv("RECOMP_FB_DUMP"), seq++);
    if (dump_rect(path, s_shown, s_shown_x, s_shown_y, s_shown_w, s_shown_h)
        && seq == 1)
        fprintf(stderr, "  [GPU] d3d11 frame dump: %s (%ux%u)%c", path,
                s_shown_w, s_shown_h, 10);
}

/* Shadow mode: the software path has just dumped a rectangle of a surface;
 * dump this back end's copy of it beside it, as <prefix>gpu_NNNNN.bmp. A
 * surface this back end never made comes out black, and is counted. */
static void dump_beside_sw(const Nv2aPbDump *sw)
{
    char path[512];
    int i;

    for (i = 0; i < GPU_SURFACES; i++)
        if (s_surf[i].tex && s_surf[i].addr == sw->addr
            && s_surf[i].pitch == sw->pitch)
            break;
    if (i == GPU_SURFACES) {
        i = -1;
        s_stat.shadow_no_surface++;
    }
    snprintf(path, sizeof path, "%sgpu_%05d.bmp", getenv("RECOMP_FB_DUMP"),
             sw->seq);
    if (dump_rect(path, i, sw->x, sw->y, sw->w, sw->h)
        && s_stat.shadow_dumps++ == 0)
        fprintf(stderr, "  [GPU] d3d11 shadow dump: %s (%ux%u)%c", path,
                sw->w, sw->h, 10);
}

static void d3d_present(void)
{
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    {
        /* RECOMP_GPU_PACING: presents in each wall-clock second, ten to a
         * line -- how evenly the title paces its frames once drawing costs
         * nothing. */
        static int on = -1;
        static ULONGLONG second;
        static uint32_t n, line[10], nline;
        ULONGLONG now;
        if (on < 0)
            on = getenv("RECOMP_GPU_PACING") != NULL;
        if (on) {
            now = GetTickCount64() / 1000;
            if (second && now != second) {
                line[nline++] = n;
                n = 0;
                if (nline == 10) {
                    fprintf(stderr, "[PACE] presents/s: %u %u %u %u %u %u %u"
                            " %u %u %u (flip %u)%c", line[0], line[1], line[2],
                            line[3], line[4], line[5], line[6], line[7],
                            line[8], line[9], g_pb.flips, 10);
                    nline = 0;
                }
            }
            second = now;
            n++;
        }
    }

    if (s_shadow) {
        /* The software back end presented first and has said what it
         * dumped; the window is its, not ours. */
        const Nv2aPbDump *sw = pb_sw_flip_dump();
        if (sw)
            dump_beside_sw(sw);
    } else if (flip_dump_on()) {
        dump_shown();
    }
    if (!s_shadow && ensure_swap_chain()) {
        ID3D11Texture2D *back = NULL;
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
        if (s_shown >= 0
            && SUCCEEDED(IDXGISwapChain1_GetBuffer(s_swap, 0,
                    &IID_ID3D11Texture2D, (void **)&back))) {
            D3D11_BOX box;
            box.left = s_shown_x;
            box.top = s_shown_y;
            box.front = 0;
            box.right = s_shown_x + (s_shown_w < SC_W ? s_shown_w : SC_W);
            box.bottom = s_shown_y + (s_shown_h < SC_H ? s_shown_h : SC_H);
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                    (ID3D11Resource *)back, 0, 0, 0, 0,
                    (ID3D11Resource *)s_surf[s_shown].tex, 0, &box);
            ID3D11Texture2D_Release(back);
        }
        /* No vsync: the title paces itself on the vblank the kernel
         * emulates, as it does with the software path. */
        IDXGISwapChain1_Present(s_swap, 0, 0);
        s_stat.presents++;
    }
    s_shown_stale = 1;
}

static void d3d_report(void)
{
    fprintf(stderr, "[GPU] d3d11: %u colour clears (%u without a surface),"
            " %u batches not drawn yet, %u presents, %u surfaces made%c",
            s_stat.clears, s_stat.clears_skipped, s_stat.batches,
            s_stat.presents, s_stat.surfaces, 10);
    if (s_shadow)
        fprintf(stderr, "[GPU] d3d11 shadow: %u frames dumped beside the"
                " software ones (%u from a surface the GPU never made)%c",
                s_stat.shadow_dumps, s_stat.shadow_no_surface, 10);
}

static const Nv2aPbBackend s_backend = {
    "d3d11", d3d_clear, d3d_draw, d3d_present, d3d_report
};

const Nv2aPbBackend *nv2a_pb_backend_d3d11_open(int shadow)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL got;
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           (UINT)(sizeof levels / sizeof levels[0]),
                           D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (FAILED(hr)) {
        fail("D3D11CreateDevice", hr);
        return NULL;
    }
    hr = ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1,
                                            (void **)&s_ctx1);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice,
                                         (void **)&dxgi_dev);
    if (SUCCEEDED(hr))
        hr = IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    if (SUCCEEDED(hr))
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2,
                                    (void **)&s_factory);
    if (adapter) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)))
            fprintf(stderr, "[GPU] d3d11: %ls, feature level %X.%X%c",
                    ad.Description, (unsigned)got >> 12,
                    ((unsigned)got >> 8) & 0xF, 10);
        IDXGIAdapter_Release(adapter);
    }
    if (dxgi_dev)
        IDXGIDevice_Release(dxgi_dev);
    if (FAILED(hr)) {
        fail("device setup", hr);
        if (s_ctx1) { ID3D11DeviceContext1_Release(s_ctx1); s_ctx1 = NULL; }
        ID3D11DeviceContext_Release(s_ctx); s_ctx = NULL;
        ID3D11Device_Release(s_dev); s_dev = NULL;
        return NULL;
    }
    s_shadow = shadow;
    return &s_backend;
}

#else

const Nv2aPbBackend *nv2a_pb_backend_d3d11_open(int shadow)
{
    (void)shadow;
    return NULL;
}

#endif
