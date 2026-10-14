/**
 * The D3D11 back end of the pushbuffer executor (RECOMP_GPU=d3d11).
 *
 * The software back end (nv2a_draw_sw.c) draws every pixel on the CPU, which
 * caps an in-game frame at a few per second. This one hands the same work to
 * the real GPU: the front end (nv2a_pb_exec.c) still decodes the methods and
 * keeps the state, and this turns clears, batches and flips into D3D11 calls.
 *
 * Built up in steps, each checked against the software path at the same flip
 * (docs: GPU plan in the game repo's architecture notes). Step 1: the device,
 * a swap chain on the framebuffer window, and colour clears into one GPU
 * texture per guest colour surface. Step 2: batches -- vertices still
 * transformed on the CPU by the software path's interpreter, pixels on the
 * GPU with four texture stages, the register combiners (one general shader
 * that reads them from a constant buffer), alpha test, fog, blend and colour
 * mask. Step 3: a texture that is a GPU surface is sampled from the GPU
 * (render to texture); depth and stencil buffers on the GPU, read back as
 * textures there too; near-plane clipping on the CPU. Not yet: mip levels.
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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_pb_state.h"

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowStart(void);
extern void *xbox_FramebufferWindowHandle(void);
extern void *xbox_FramebufferWindowGpuView(unsigned w, unsigned h);
extern void xbox_FramebufferWindowGpuOwned(int on);

extern int xbox_RenderRes(unsigned *w, unsigned *h);

/* The title's frame. The swap chain is this size times the render scale;
 * DXGI scales it to the window's aspect-keeping view. */
#define SC_W 640
#define SC_H 480

/* RECOMP_RENDER_RES (fb_present.c): the frame is drawn at s_sc_w x s_sc_h
 * host pixels instead of 640x480, each axis on its own (s_rsx, s_rsy: host
 * pixels per guest pixel; 1920x1080 is 3 x 2.25). Only "scaled" surfaces,
 * the frame and other screen-sized ones (surface_scaled_now), and their
 * depth buffers are drawn bigger. Everything the guest sees stays in its
 * own pixels: vertices arrive in guest pixels and the viewport maps them
 * onto the bigger texture, scissors and clears are scaled here, textures
 * are sampled through guest-sized coordinates, and visibility counts are
 * divided back down (d3d_poll). Off: every scale is exactly 1 and every
 * size the same as before. ponytail: assumes a 640x480 title frame. */
static int      s_scale_on;
static float    s_rsx = 1.0f, s_rsy = 1.0f;
static uint32_t s_sc_w = SC_W, s_sc_h = SC_H;

/* A guest-pixel coordinate on a surface of scale s, in host pixels. Edges
 * round to the nearest, so rectangles that touch in the guest still touch. */
static uint32_t to_host(uint32_t v, float s)
{
    return s == 1.0f ? v : (uint32_t)floorf((float)v * s + 0.5f);
}

static ID3D11Device         *s_dev;
static ID3D11DeviceContext  *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;    /* ClearView: clears a rectangle */
static IDXGIFactory2        *s_factory;
static IDXGISwapChain1      *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static int                   s_swap_failed;
static int                   s_shadow;  /* RECOMP_GPU=both: no window */

/* One GPU texture per guest colour surface, found by its resolved address
 * and pitch. ponytail: always B8G8R8A8 whatever the guest format; a 16-bit
 * surface keeps its alpha at 1 by never writing it (see blend_state), which
 * is what reading it back as R5G6B5 gives the software path.
 * Never smaller than ZETA_W x ZETA_H: D3D11 binds a depth buffer only with
 * colour targets of its own size, and one depth buffer serves every colour
 * surface drawn with it, as one host buffer does in the software path. */
typedef struct {
    uint32_t addr, pitch;
    uint32_t w, h;                      /* in guest pixels */
    uint32_t tw, th;                    /* the texture, in host pixels */
    uint32_t scaled;                    /* drawn at the render scale */
    uint32_t bpp, swz;                  /* guest pixel size, swizzled */
    uint32_t used;                      /* s_surf_tick at the last use */
    uint32_t drawn_flip;                /* g_pb.flips when last drawn on */
    ID3D11Texture2D        *tex;
    ID3D11RenderTargetView *rtv;
} GpuSurface;

/* Conker's 3D menu room renders into 39 small targets plus the frame: with
 * 32 slots it threw out and remade ~13 surfaces a frame, and textures read
 * from a thrown-out one came from guest memory. Each slot is at least
 * ZETA_W x ZETA_H x 4 bytes (4 MB) once used, so 128 is 512 MB at most. */
#define GPU_SURFACES 128
/* The software path's depth buffer size (NV_ZBUF_W/H): it draws nothing
 * with depth past it, and the GPU's depth buffers are the same. */
#define ZETA_W 1024
#define ZETA_H 1024
static GpuSurface s_surf[GPU_SURFACES];
static uint32_t   s_surf_tick;

static float surf_sx(int i) { return s_surf[i].scaled ? s_rsx : 1.0f; }
static float surf_sy(int i) { return s_surf[i].scaled ? s_rsy : 1.0f; }

/* Whether a surface made now for the current colour target is drawn at the
 * render scale: a linear one at least as wide as the frame (640 pixels), so
 * the frame and any screen-sized copy of it. Swizzled and narrower targets
 * -- shadow maps, glow buffers, Conker's 32x32 menu-room targets -- keep
 * their own size: every surface is padded to its depth buffer's size, and
 * scaling dozens of them would cost gigabytes at 4K for no visible gain. */
static int surface_scaled_now(void)
{
    uint32_t bpp = pb_surface_bpp();
    return s_scale_on && ((g_pb.format >> 8) & 0xF) != 2
           && bpp && g_pb.pitch / bpp >= SC_W;
}

/* The surface to present: the biggest one cleared since the last flip, as
 * note_drawn picks the drawn one in the software path (a title also clears
 * small render targets, often after the main one). */
static int      s_shown = -1, s_shown_stale = 1;
static uint32_t s_shown_x, s_shown_y, s_shown_w, s_shown_h;

static struct {
    uint32_t clears, clears_skipped, batches, presents, surfaces;
    uint32_t shadow_dumps, shadow_no_surface;
    /* Batches: drawn, and the reasons the rest were not. */
    uint32_t drawn, tris, skip_program, skip_not_screen, skip_surface;
    uint32_t verts_need_clip, tris_clipped, tris_behind, combined, tris_culled;
    uint32_t textured, tex_uploads, tex_reused, tex_unreadable, tex_made;
    uint32_t rtt_binds, rtt_unhandled;
    uint32_t zeta_clears, zeta_made, zeta_draws, zeta_too_big;
    uint32_t zeta_reads, zeta_unhandled;
    uint32_t stale_reads, rtt_old, rtt_twin;
    uint32_t evictions, evicted_reads;
} s_stat;

/* Diagnostic: addresses of surfaces thrown out of the table, so a texture
 * read later from one is counted. Its GPU copy is gone and guest memory,
 * which this back end never writes, is read instead. First 1024 kept. */
#define EVICTED_MAX 1024
static uint32_t s_evicted[EVICTED_MAX];
static int      s_nevicted;

static int was_evicted(uint32_t addr)
{
    int k;
    for (k = 0; k < s_nevicted; k++)
        if (s_evicted[k] == addr)
            return 1;
    return 0;
}

static void note_evicted(uint32_t addr)
{
    addr &= 0x0FFFFFFFu;
    s_stat.evictions++;
    if (s_nevicted < EVICTED_MAX && !was_evicted(addr))
        s_evicted[s_nevicted++] = addr;
}

/* RECOMP_GPU_CALL_TIMES=1: wall time spent in each group of D3D11 calls,
 * reported with the [GPU] lines. A sampling profile shows the executor
 * sleeping and reading the clock inside the driver, called from d3d_draw
 * (run 205); this says which call the driver is making it wait in. */
enum { CT_TEXTURES, CT_VB, CT_IB, CT_CB, CT_DRAW, CT_PRESENT, CT_N };
static const char *const CT_NAME[CT_N] = {
    "textures+samplers", "vertex Map", "index Map", "constants Map",
    "state+DrawIndexed", "Present"
};
static int s_ct_on = -1;
static uint64_t s_ct_ticks[CT_N], s_ct_count[CT_N];

static uint64_t ct_now(void)
{
    LARGE_INTEGER t;
    if (s_ct_on < 0)
        s_ct_on = getenv("RECOMP_GPU_CALL_TIMES") != NULL;
    if (!s_ct_on)
        return 0;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* What the last ring Map did (ring_put), named when that Map is slow. */
static char s_ct_ring_note[96];

/* Any one call group of 50 ms or more is also reported on its own: a level
 * load holds D3D's GPU DPC for seconds while the executor waits in a driver
 * call (2026-10-11), and the per-report totals hide which call and why. */
static void ct_add(int which, uint64_t t0)
{
    if (s_ct_on > 0) {
        uint64_t d = ct_now() - t0;
        LARGE_INTEGER f;
        s_ct_ticks[which] += d;
        s_ct_count[which]++;
        QueryPerformanceFrequency(&f);
        if (d * 20 >= (uint64_t)f.QuadPart)
            fprintf(stderr, "[GPU] d3d11: slow %s: %.0f ms%s%s%c", CT_NAME[which],
                    (double)d * 1000.0 / (double)f.QuadPart,
                    (which == CT_VB || which == CT_IB) ? " " : "",
                    (which == CT_VB || which == CT_IB) ? s_ct_ring_note : "", 10);
    }
}

static void ct_report(void)
{
    static uint64_t last_ticks[CT_N], last_count[CT_N];
    LARGE_INTEGER f;
    int i;

    if (s_ct_on <= 0)
        return;
    QueryPerformanceFrequency(&f);
    fprintf(stderr, "[GPU] d3d11 call times since last report:");
    for (i = 0; i < CT_N; i++) {
        uint64_t n = s_ct_count[i] - last_count[i];
        double ms = (double)(s_ct_ticks[i] - last_ticks[i]) * 1000.0
                  / (double)f.QuadPart;
        fprintf(stderr, " %s %.0f ms (%llu x %.1f us);", CT_NAME[i], ms,
                (unsigned long long)n, n ? ms * 1000.0 / (double)n : 0.0);
        last_ticks[i] = s_ct_ticks[i];
        last_count[i] = s_ct_count[i];
    }
    fprintf(stderr, "%c", 10);
}

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
    HWND hwnd, view;
    HRESULT hr;

    if (s_swap)
        return 1;
    if (s_swap_failed)
        return 0;
    hwnd = (HWND)xbox_FramebufferWindowHandle();
    if (!hwnd)
        return 0;
    /* The swap chain goes on the window's aspect-keeping child view, so a
     * resized window scales the picture without stretching it. */
    view = (HWND)xbox_FramebufferWindowGpuView(s_sc_w, s_sc_h);
    if (!view)
        view = hwnd;

    memset(&d, 0, sizeof d);
    d.Width = s_sc_w;
    d.Height = s_sc_h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(s_factory, (IUnknown *)s_dev,
                                              view, &d, NULL, NULL, &s_swap);
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
            " (%ux%u)%c", s_sc_w, s_sc_h, 10);
    return 1;
}

/* The GPU texture for the current colour surface, made or grown to cover the
 * current clip: its index, or -1 when there is no surface to speak of. */
static int surface_current(void)
{
    uint32_t addr, w, h, scaled;
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
    if (w < ZETA_W) w = ZETA_W;
    if (h < ZETA_H) h = ZETA_H;
    if (w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return -1;

    s_surf_tick++;
    for (i = 0; i < GPU_SURFACES; i++)
        if (s_surf[i].tex && s_surf[i].addr == addr
            && s_surf[i].pitch == g_pb.pitch)
            break;
    if (i < GPU_SURFACES && s_surf[i].w >= w && s_surf[i].h >= h) {
        s_surf[i].bpp = pb_surface_bpp();
        s_surf[i].swz = ((g_pb.format >> 8) & 0xF) == 2;
        s_surf[i].used = s_surf_tick;
        s_surf[i].drawn_flip = g_pb.flips;
        return i;
    }
    /* A grown surface keeps the scale it was made with. */
    scaled = i < GPU_SURFACES ? s_surf[i].scaled
                              : (uint32_t)surface_scaled_now();
    if (i == GPU_SURFACES) {
        /* An empty slot, or the one unused longest. Conker's menu room
         * draws into new render-target addresses every frame; a round
         * robin threw the frame itself out mid-frame (run 169: 2510
         * surfaces made, every few frames only the glow left on black). */
        int k;
        for (i = 0, k = 1; k < GPU_SURFACES && s_surf[i].tex; k++)
            if (!s_surf[k].tex || s_surf[k].used < s_surf[i].used)
                i = k;
        if (s_surf[i].tex)
            note_evicted(s_surf[i].addr);
        if (s_stat.surfaces++ < 48)
            fprintf(stderr, "[GPU] d3d11: surface %u: 0x%08X pitch %u"
                    " clip %ux%u+%u+%u fmt 0x%X (slot %d%s%s)%c",
                    s_stat.surfaces, addr, g_pb.pitch, g_pb.clip_w,
                    g_pb.clip_h, g_pb.clip_x, g_pb.clip_y, g_pb.format, i,
                    s_surf[i].tex ? ", evicting" : "",
                    scaled ? ", scaled" : "", 10);
    } else {
        /* Grow: keep the larger of old and new in each direction. */
        if (s_surf[i].w > w) w = s_surf[i].w;
        if (s_surf[i].h > h) h = s_surf[i].h;
    }
    {
        /* A grown surface keeps what was drawn on it: a title can widen its
         * clip partway through a frame, and starting over empty lost the
         * part already drawn. */
        GpuSurface old = s_surf[i];
        D3D11_TEXTURE2D_DESC d;
        HRESULT hr;
        int shown = s_shown == i;
        memset(&s_surf[i], 0, sizeof s_surf[i]);
        memset(&d, 0, sizeof d);
        d.Width = to_host(w, scaled ? s_rsx : 1.0f);
        d.Height = to_host(h, scaled ? s_rsy : 1.0f);
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
        if (SUCCEEDED(hr) && old.tex && old.addr == addr
            && old.pitch == g_pb.pitch)
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                    (ID3D11Resource *)s_surf[i].tex, 0, 0, 0, 0,
                    (ID3D11Resource *)old.tex, 0, NULL);
        if (old.rtv) ID3D11RenderTargetView_Release(old.rtv);
        if (old.tex) ID3D11Texture2D_Release(old.tex);
        if (FAILED(hr)) {
            static int said;
            if (!said++)
                fail("surface texture", hr);
            if (s_surf[i].tex) ID3D11Texture2D_Release(s_surf[i].tex);
            memset(&s_surf[i], 0, sizeof s_surf[i]);
            if (shown)
                s_shown = -1;
            return -1;
        }
        /* Replaced by a different surface: it is no longer the one shown. */
        if (shown && old.addr != addr)
            s_shown = -1;
        s_surf[i].tw = d.Width;
        s_surf[i].th = d.Height;
    }
    s_surf[i].addr = addr;
    s_surf[i].pitch = g_pb.pitch;
    s_surf[i].w = w;
    s_surf[i].h = h;
    s_surf[i].scaled = scaled;
    s_surf[i].bpp = pb_surface_bpp();
    s_surf[i].swz = ((g_pb.format >> 8) & 0xF) == 2;
    s_surf[i].used = s_surf_tick;
    s_surf[i].drawn_flip = g_pb.flips;
    return i;
}

/* Surface i was cleared or drawn over the current clip: it is the one to
 * present if it is the first since the flip or the biggest so far, as
 * note_drawn decides in the software path. */
static void note_shown(int i)
{
    if (s_shown_stale || s_shown < 0
        || g_pb.clip_w * g_pb.clip_h >= s_shown_w * s_shown_h) {
        s_shown = i;
        s_shown_stale = 0;
        s_shown_x = g_pb.clip_x; s_shown_y = g_pb.clip_y;
        s_shown_w = g_pb.clip_w; s_shown_h = g_pb.clip_h;
    }
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

/* ── Depth and stencil ─────────────────────────────────────────────────────
 *
 * One D24S8 texture per zeta surface, found by SURFACE_ZETA_OFFSET as the
 * software path finds its host buffers (zbuf_slot), ZETA_W x ZETA_H like
 * them. Depth arrives in the zeta format's units, as the clear value does
 * (0..0xFFFFFF for Z24, 0..0xFFFF for Z16), and is stored as 0..1.
 * Typeless, so the zeta copy pass (texture_from_zeta) can read it back. */

typedef struct {
    uint32_t offset;                    /* SURFACE_ZETA_OFFSET, as written */
    uint32_t zf;                        /* zeta format: 1 Z16, 2 Z24S8 */
    uint32_t scaled;                    /* for scaled colour surfaces */
    uint32_t tw, th;                    /* the texture, in host pixels */
    uint32_t used;                      /* s_zeta_tick at the last draw */
    ID3D11Texture2D          *tex;
    ID3D11DepthStencilView   *dsv;
    ID3D11ShaderResourceView *srv_z, *srv_s;
} GpuZeta;

/* With RECOMP_RENDER_RES a zeta surface can have two buffers, one per
 * colour-surface scale: D3D11 binds a depth buffer only with targets of its
 * own size. A clear clears both; a title drawing the same zeta under a
 * scaled and an unscaled target would see two depth buffers where it has
 * one, which Conker does not do (its depth-tested passes all go to the
 * frame). */
#define GPU_ZETAS 8
static GpuZeta  s_zeta[GPU_ZETAS];
static int      s_zeta_next;
static uint32_t s_zeta_tick;

/* SURFACE_FORMAT bits 4-7, the zeta format: 1 is Z16, 2 is Z24S8. */
static uint32_t zeta_format(void)
{
    return (g_pb.format >> 4) & 0xF;
}

/* Guest depth units to the 0..1 the depth buffer holds. */
static float zeta_scale(void)
{
    return zeta_format() == 1 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
}

/* The depth buffer of the current zeta surface for colour surfaces of the
 * given scale, made (depth 1, stencil 0, as zbuf_slot fills its new buffers:
 * farther than anything) when there is none and create is set: its index,
 * or -1. */
static int zeta_current(int create, uint32_t scaled)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    GpuZeta *z;
    HRESULT hr;
    int i;

    for (i = 0; i < GPU_ZETAS; i++)
        if (s_zeta[i].tex && s_zeta[i].offset == g_pb.zeta_offset
            && s_zeta[i].scaled == scaled) {
            s_zeta[i].zf = zeta_format();
            return i;
        }
    if (!create)
        return -1;
    i = s_zeta_next++ % GPU_ZETAS;
    z = &s_zeta[i];
    if (z->srv_s) ID3D11ShaderResourceView_Release(z->srv_s);
    if (z->srv_z) ID3D11ShaderResourceView_Release(z->srv_z);
    if (z->dsv) ID3D11DepthStencilView_Release(z->dsv);
    if (z->tex) ID3D11Texture2D_Release(z->tex);
    memset(z, 0, sizeof *z);

    memset(&d, 0, sizeof d);
    d.Width = to_host(ZETA_W, scaled ? s_rsx : 1.0f);
    d.Height = to_host(ZETA_H, scaled ? s_rsy : 1.0f);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R24G8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &z->tex);
    if (SUCCEEDED(hr)) {
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(s_dev,
                (ID3D11Resource *)z->tex, &dv, &z->dsv);
    }
    memset(&sv, 0, sizeof sv);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (SUCCEEDED(hr)) {
        sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        hr = ID3D11Device_CreateShaderResourceView(s_dev,
                (ID3D11Resource *)z->tex, &sv, &z->srv_z);
    }
    if (SUCCEEDED(hr)) {
        sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        hr = ID3D11Device_CreateShaderResourceView(s_dev,
                (ID3D11Resource *)z->tex, &sv, &z->srv_s);
    }
    if (FAILED(hr)) {
        static int said;
        if (!said++)
            fail("depth buffer", hr);
        if (z->srv_z) ID3D11ShaderResourceView_Release(z->srv_z);
        if (z->dsv) ID3D11DepthStencilView_Release(z->dsv);
        if (z->tex) ID3D11Texture2D_Release(z->tex);
        memset(z, 0, sizeof *z);
        return -1;
    }
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    z->offset = g_pb.zeta_offset;
    z->zf = zeta_format();
    z->scaled = scaled;
    z->tw = d.Width;
    z->th = d.Height;
    s_stat.zeta_made++;
    return i;
}

/* Depth (bit 0) and stencil (bit 1) clears: the whole buffer whatever the
 * clip, as zbuf_clear and sbuf_clear do, to the value in ZSTENCIL_CLEAR.
 * Every buffer of this zeta surface, scaled or not; when there is none yet,
 * one for the current colour target's scale. */
static void zeta_clear(uint32_t param)
{
    UINT flags = 0;
    float z;
    int i, n = 0;

    if (param & 0x01) flags |= D3D11_CLEAR_DEPTH;
    if (param & 0x02) flags |= D3D11_CLEAR_STENCIL;
    z = zeta_format() == 1 ? (float)(g_pb.zstencil_clear & 0xFFFF)
                           : (float)(g_pb.zstencil_clear >> 8);
    z *= zeta_scale();
    if (zeta_current(0, 0) < 0 && zeta_current(0, 1) < 0)
        zeta_current(1, (uint32_t)surface_scaled_now());
    for (i = 0; i < GPU_ZETAS; i++)
        if (s_zeta[i].tex && s_zeta[i].offset == g_pb.zeta_offset) {
            s_zeta[i].zf = zeta_format();
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_zeta[i].dsv,
                    flags, z > 1.0f ? 1.0f : z,
                    (UINT8)(g_pb.zstencil_clear & 0xFF));
            n++;
        }
    if (n)
        s_stat.zeta_clears++;
}

/* ponytail: all four colour channels whatever the mask bits say. */
static void d3d_clear(uint32_t param)
{
    D3D11_RECT r;
    float c[4];
    int i;

    /* The window opens on the first clear, as in the software path. */
    xbox_FramebufferWindowStart();
    if (param & 0x03)
        zeta_clear(param);
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
    r.left = (LONG)to_host(g_pb.clip_x, surf_sx(i));
    r.top = (LONG)to_host(g_pb.clip_y, surf_sy(i));
    r.right = (LONG)to_host(g_pb.clip_x + g_pb.clip_w, surf_sx(i));
    r.bottom = (LONG)to_host(g_pb.clip_y + g_pb.clip_h, surf_sy(i));
    ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)s_surf[i].rtv, c,
                                   &r, 1);
    s_stat.clears++;
    note_shown(i);
}

/* ── Batches ───────────────────────────────────────────────────────────────
 *
 * Vertices come out of the CPU, pixels out of the GPU. A batch that runs a
 * vertex program -- every batch Conker draws, 2D ones included, since Xbox
 * D3D puts even pre-transformed vertices through a pass-through program --
 * is transformed by the software path's interpreter (pb_transform_batch);
 * a fixed-function batch whose positions are already pixels is read through
 * the same helpers raster_batch uses (pb_fixed_vertex with the combiners,
 * the flat-path fetches without). Either way each vertex ends up as an
 * Nv2aVshOutput, which is also the GPU vertex: the batch goes up as it is,
 * with an index list that cuts its primitive into triangles the way
 * raster_batch cuts it. The shaders are in nv2a_draw_d3d11_hlsl.h. A
 * triangle reaching behind the eye or the near plane is clipped on the CPU
 * (clip_triangle, as raster_xf_clipped does). */

#include "nv2a_draw_d3d11_hlsl.h"

#define NV_CLIP_W 1e-3f                 /* as in nv2a_draw_sw.c */
enum { BV_OK = 1, BV_CLIP = 2 };        /* s_bv_ok; 0: not usable */

static Nv2aVshOutput s_bv[NV_MAX_INDICES];          /* fixed-function batches */
static uint8_t       s_bv_ok[NV_MAX_INDICES];       /* vertex usable */
/* The triangle list: at most one triangle per vertex, and a clipped one
 * fans into at most three. */
static uint32_t      s_ib[9 * NV_MAX_INDICES];

static ID3D11VertexShader *s_vs;
static ID3D11PixelShader  *s_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_cb;
static ID3D11RasterizerState *s_rs;

/* The shader's constant buffer; see the cbuffer in nv2a_draw_d3d11_hlsl.h. */
enum { PATH_FLAT = 0, PATH_MODULATE = 1, PATH_COMBINERS = 2 };
typedef struct {
    float    rt[4];
    float    tscale[4][4];
    uint32_t mode[4];                   /* path, bound stages, alpha func, ref */
    uint32_t stage[4];                  /* stage program, clip planes, control, fog */
    uint32_t icw[8][4];
    float    cf0[8][4], cf1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4], fogc[4], fogp[4];
    float    zsc[4];                    /* depth units to 0..1 */
    float    bmat[4][4];                /* per stage: BUMP_ENV_MAT 00 01 10 11 */
    float    bsc[4][4];                 /* per stage: bump scale, offset, input stage */
} GpuConsts;

/* The zeta copy pass (texture_from_zeta): its shaders and constants. */
static ID3D11VertexShader *s_zvs;
static ID3D11PixelShader  *s_zps;
static ID3D11Buffer       *s_zcb;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a_draw_d3d11",
                            NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] d3d11: shader %s failed: %s%c", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", 10);
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

/* The fixed pipeline objects. 0 when they cannot be made; batches are then
 * counted and skipped as before. */
static int draw_setup(void)
{
#define EL(name, idx, off) { name, idx, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, \
                             off, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    static const D3D11_INPUT_ELEMENT_DESC el[] = {   /* Nv2aVshOutput */
        EL("POSITION", 0, 0), EL("COLOR", 0, 16), EL("COLOR", 1, 32),
        EL("TEXCOORD", 4, 48), EL("TEXCOORD", 0, 64), EL("TEXCOORD", 1, 80),
        EL("TEXCOORD", 2, 96), EL("TEXCOORD", 3, 112),
    };
#undef EL
    ID3DBlob *vs = compile("vs", "vs_4_0"), *ps = compile("ps", "ps_4_0");
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr = vs && ps ? S_OK : E_FAIL;

    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(s_dev,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(s_dev,
                ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps),
                NULL, &s_ps);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(s_dev, el,
                (UINT)(sizeof el / sizeof el[0]),
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                &s_layout);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(GpuConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    }
    if (SUCCEEDED(hr)) {
        /* The zeta copy pass. Without it a zeta surface read as a texture
         * comes from guest memory, as before. */
        ID3DBlob *zvs = compile("zvs", "vs_4_0"), *zps = compile("zps", "ps_4_0");
        HRESULT zh = zvs && zps ? S_OK : E_FAIL;
        if (SUCCEEDED(zh))
            zh = ID3D11Device_CreateVertexShader(s_dev,
                    ID3D10Blob_GetBufferPointer(zvs),
                    ID3D10Blob_GetBufferSize(zvs), NULL, &s_zvs);
        if (SUCCEEDED(zh))
            zh = ID3D11Device_CreatePixelShader(s_dev,
                    ID3D10Blob_GetBufferPointer(zps),
                    ID3D10Blob_GetBufferSize(zps), NULL, &s_zps);
        if (SUCCEEDED(zh)) {
            bd.ByteWidth = 16;
            zh = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_zcb);
        }
        if (zvs) ID3D10Blob_Release(zvs);
        if (zps) ID3D10Blob_Release(zps);
        if (FAILED(zh)) {
            fail("zeta copy pass", zh);
            s_zcb = NULL;
        }
    }
    if (SUCCEEDED(hr)) {
        /* No culling here: build_triangles drops culled faces on the CPU
         * (pb_cull_tri), as the software path does. */
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = FALSE;
        hr = ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    }
    if (FAILED(hr)) {
        fail("draw pipeline", hr);
        return 0;
    }
    return 1;
}

/* Copy `bytes` into dynamic buffer b, discarding what it held. */
static int buf_fill(ID3D11Buffer *b, const void *src, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, src, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    return 1;
}

/* A dynamic buffer filled as a ring: each batch is appended after the last
 * with MAP_WRITE_NO_OVERWRITE and drawn from its own offset; only a wrap
 * discards. A discard per batch made the driver hand out a fresh copy of the
 * whole buffer every draw (60-130 us each on AMD, ~60% of the executor). */
typedef struct {
    ID3D11Buffer *b;
    uint32_t      bytes, pos, wraps;
} Ring;

#define RING_MIN_BYTES (16u << 20)

/* RECOMP_GPU_CALL_TIMES: before a wrap's discard, how long the GPU takes to
 * finish everything queued so far (an event query, flushed). Tells a GPU
 * that is really busy from a driver that holds the discard back. -1 when
 * the query could not be made. */
static ID3D11Query *s_ct_idle_q;
static double ct_gpu_idle_ms = -1.0;

static double ct_wait_gpu_idle(void)
{
    D3D11_QUERY_DESC qd;
    LARGE_INTEGER f, t0, t1;
    BOOL done = FALSE;

    if (!s_ct_idle_q) {
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_ct_idle_q)))
            return -1.0;
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_ct_idle_q);
    while (ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)s_ct_idle_q,
                                       &done, sizeof done, 0) != S_OK)
        SwitchToThread();
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static Ring s_vb, s_ib_ring;            /* the batches' vertices, indices */

/* Append pieces a and c back to back; *at gets their byte offset. */
static int ring_put(Ring *r, UINT bind, const void *a, size_t na,
                    const void *c, size_t nc, uint32_t *at)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    const char *what = "append";
    uint32_t need = (uint32_t)(na + nc);
    uint32_t pos = (r->pos + 15u) & ~15u;

    if (!r->b || need > r->bytes / 2) {         /* (re)made big enough */
        D3D11_BUFFER_DESC bd;
        uint32_t want = r->bytes ? r->bytes : RING_MIN_BYTES;
        while (want < need * 2)
            want *= 2;
        if (r->b)
            ID3D11Buffer_Release(r->b);
        r->b = NULL;
        r->bytes = 0;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = want;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = bind;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &r->b)))
            return 0;
        r->bytes = want;
        how = D3D11_MAP_WRITE_DISCARD;
        what = "new buffer";
        pos = 0;
    } else if (pos + need > r->bytes) {
        how = D3D11_MAP_WRITE_DISCARD;
        pos = 0;
        r->wraps++;
        what = "wrap discard";
        if (s_ct_on > 0)
            ct_gpu_idle_ms = ct_wait_gpu_idle();
    }
    if (s_ct_on > 0)
        snprintf(s_ct_ring_note, sizeof s_ct_ring_note,
                 "(%s at %u of %u MB, %u bytes, wraps %u; GPU idle after %.0f ms)",
                 what,
                 pos, r->bytes >> 20, need, r->wraps, ct_gpu_idle_ms);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->b, 0,
                                       how, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + pos, a, na);
    if (nc)
        memcpy((uint8_t *)m.pData + pos + na, c, nc);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->b, 0);
    *at = pos;
    r->pos = pos + need;
    return 1;
}

/* ── Blend ── GL factors and equations (what the NV2A takes) to D3D11's. */

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA
                              : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA
                              : D3D11_BLEND_INV_DEST_COLOR;
    /* SRC_ALPHA_SATURATE is 1 for alpha, as in blend_factor (sw). */
    case 0x0308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

/* Blend states by what they were made from; a title uses a handful. */
#define GPU_BLENDS 32
static struct {
    uint32_t key[4];
    ID3D11BlendState *bs;
} s_blend[GPU_BLENDS];
static int s_blend_next;

static ID3D11BlendState *blend_state(uint32_t bpp)
{
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *rt = &d.RenderTarget[0];
    uint32_t on = g_pb.blend_enable
                  && !(g_pb.blend_sfactor == 1 && g_pb.blend_dfactor == 0);
    uint32_t mask = 0, key[4];
    int i;

    if (g_pb.color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (g_pb.color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (g_pb.color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if ((g_pb.color_mask & 0x01000000u) && bpp == 4)
        mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    key[0] = on | mask << 1;
    key[1] = on ? g_pb.blend_sfactor : 0;
    key[2] = on ? g_pb.blend_dfactor : 0;
    key[3] = on ? g_pb.blend_equation : 0;
    for (i = 0; i < GPU_BLENDS; i++)
        if (s_blend[i].bs && !memcmp(s_blend[i].key, key, sizeof key))
            return s_blend[i].bs;

    memset(&d, 0, sizeof d);
    rt->BlendEnable = on ? TRUE : FALSE;
    rt->SrcBlend = blend_factor(g_pb.blend_sfactor, 0);
    rt->DestBlend = blend_factor(g_pb.blend_dfactor, 0);
    rt->BlendOp = blend_op(g_pb.blend_equation);
    rt->SrcBlendAlpha = blend_factor(g_pb.blend_sfactor, 1);
    rt->DestBlendAlpha = blend_factor(g_pb.blend_dfactor, 1);
    rt->BlendOpAlpha = rt->BlendOp;
    rt->RenderTargetWriteMask = (UINT8)mask;
    i = s_blend_next++ % GPU_BLENDS;
    if (s_blend[i].bs)
        ID3D11BlendState_Release(s_blend[i].bs);
    s_blend[i].bs = NULL;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &s_blend[i].bs)))
        return NULL;
    memcpy(s_blend[i].key, key, sizeof key);
    return s_blend[i].bs;
}

/* The constant blend colour; CONSTANT_ALPHA factors take its alpha in all
 * four channels.
 * ponytail: one batch mixing CONSTANT_COLOR and CONSTANT_ALPHA gets alpha. */
static void blend_constant(float f[4])
{
    uint32_t c = g_pb.blend_color;
    int k;

    for (k = 0; k < 4; k++)
        f[k] = (float)((c >> (k == 3 ? 24 : 16 - 8 * k)) & 0xFF) / 255.0f;
    if (g_pb.blend_sfactor == 0x8003 || g_pb.blend_sfactor == 0x8004
        || g_pb.blend_dfactor == 0x8003 || g_pb.blend_dfactor == 0x8004)
        f[0] = f[1] = f[2] = f[3];
}

/* ── Depth-stencil states ── GL functions and ops (what the NV2A takes) to
 * D3D11's, as depth_pass, stencil_pass and stencil_apply read them: the
 * comparisons are "new OP stored" and "ref OP stored" in both. */

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    switch (f) {
    case 0x200: return D3D11_COMPARISON_NEVER;
    case 0x201: return D3D11_COMPARISON_LESS;
    case 0x202: return D3D11_COMPARISON_EQUAL;
    case 0x203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x204: return D3D11_COMPARISON_GREATER;
    case 0x205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:    return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;          /* INCR_WRAP */
    case 0x8508: return D3D11_STENCIL_OP_DECR;          /* DECR_WRAP */
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* Depth-stencil states by what they were made from. Depth writes with the
 * test off are a test that always passes: D3D11 writes no depth with depth
 * off, the software path does (xf_rows writes whenever DEPTH_MASK is set). */
#define GPU_DSS 32
static struct {
    uint32_t key[8];
    ID3D11DepthStencilState *ds;
} s_dss[GPU_DSS];
static int s_dss_next;

static ID3D11DepthStencilState *ds_state(void)
{
    D3D11_DEPTH_STENCIL_DESC d;
    uint32_t key[8], zon = g_pb.depth_test || g_pb.depth_mask;
    uint32_t son = g_pb.stencil_test != 0;
    int i;

    memset(key, 0, sizeof key);
    key[0] = zon | son << 1 | (g_pb.depth_mask ? 4u : 0u);
    key[1] = g_pb.depth_test ? g_pb.depth_func : 0x207;
    if (son) {
        key[2] = g_pb.stencil_func;
        key[3] = g_pb.stencil_rmask & 0xFF;
        key[4] = g_pb.stencil_wmask & 0xFF;
        key[5] = g_pb.stencil_op_fail;
        key[6] = g_pb.stencil_op_zfail;
        key[7] = g_pb.stencil_op_zpass;
    }
    for (i = 0; i < GPU_DSS; i++)
        if (s_dss[i].ds && !memcmp(s_dss[i].key, key, sizeof key))
            return s_dss[i].ds;

    memset(&d, 0, sizeof d);
    d.DepthEnable = zon ? TRUE : FALSE;
    d.DepthWriteMask = g_pb.depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL
                                       : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = cmp_func(key[1]);
    d.StencilEnable = son ? TRUE : FALSE;
    d.StencilReadMask = (UINT8)key[3];
    d.StencilWriteMask = (UINT8)key[4];
    d.FrontFace.StencilFunc = cmp_func(son ? key[2] : 0x207);
    d.FrontFace.StencilFailOp = stencil_op(key[5]);
    d.FrontFace.StencilDepthFailOp = stencil_op(key[6]);
    d.FrontFace.StencilPassOp = stencil_op(key[7]);
    d.BackFace = d.FrontFace;           /* one-sided, as the software path */
    i = s_dss_next++ % GPU_DSS;
    if (s_dss[i].ds)
        ID3D11DepthStencilState_Release(s_dss[i].ds);
    s_dss[i].ds = NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s_dss[i].ds)))
        return NULL;
    memcpy(s_dss[i].key, key, sizeof key);
    return s_dss[i].ds;
}

/* ── Textures ──────────────────────────────────────────────────────────────
 *
 * A stage's level 0, decoded on the CPU through the software sampler
 * (pb_tex_texel: swizzled, DXT, palette, YUV all read one way) into a
 * B8G8R8A8 texture, which is 0xAARRGGBB in memory. Cached by address,
 * format, size, pitch and palette, and re-decoded when the guest bytes
 * change: a movie keeps one address and rewrites it every frame, and a
 * render target sampled later is rewritten in place.
 * ponytail: no mip levels, so a minified texture shimmers; a cube map is
 * not bound (its stage reads white).
 * ponytail: the bytes are hashed at every bind, which is a full read of the
 * texture per batch; a dirty-page scheme would avoid it. */

/* Enough for a whole scene. With 128, Conker's first field evicted textures
 * it bound again the same frame and re-decoded ~30 a second while standing
 * still. Found through s_tex_head (by address); a full scan only picks the
 * entry to evict, on a miss. */
#define GPU_TEXTURES 1024
#define TEX_BUCKETS  1024
typedef struct {
    uint32_t addr, fmt, w, h, pitch, palette;
    uint64_t hash;
    uint32_t used;                      /* s_tex_tick at the last bind */
    uint32_t hashed_flip;               /* g_pb.flips when hash was taken */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTexture;
static GpuTexture s_tex[GPU_TEXTURES];
static uint16_t   s_tex_head[TEX_BUCKETS];  /* entry index + 1, 0 = none */
static uint16_t   s_tex_next[GPU_TEXTURES]; /* same, next in the bucket */
static uint32_t   s_tex_tick;

static uint32_t tex_bucket(uint32_t addr)
{
    return (addr * 2654435761u) >> 22;      /* top 10 bits: TEX_BUCKETS */
}

static void tex_unlink(int i)
{
    uint16_t *p = &s_tex_head[tex_bucket(s_tex[i].addr)];
    while (*p && *p != i + 1)
        p = &s_tex_next[*p - 1];
    if (*p)
        *p = s_tex_next[i];
    s_tex_next[i] = 0;
}
static uint32_t  *s_texels;             /* decode scratch */
static size_t     s_texels_cap;

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* ── Render to texture ─────────────────────────────────────────────────────
 *
 * A title draws into a surface and then samples it: Conker's bloom
 * downsamples the frame into small targets and blurs them back. On this back
 * end those pixels exist only on the GPU, so a stage whose texture starts
 * where a GPU surface does takes that surface instead of guest memory.
 *
 * The surface is copied into a texture of the stage's own size, one per
 * stage, at every bind: the current render target cannot be sampled while
 * it is drawn into, and a copy the texture's size keeps clamp and wrap at the
 * texture's edges rather than the (often larger) surface's. A copy is a
 * GPU-side blit, cheap next to hashing the guest bytes, which this path skips.
 *
 * The surface texture holds B8G8R8A8, i.e. A8R8G8B8 as the guest stores it;
 * other readings of the same pixels are shader flags (tscale[st].z: alpha
 * reads 1; .w: red and blue swap). A reading not handled here (e.g. a
 * surface read as R8G8B8A8) falls back to guest memory and is counted.
 * ponytail: matched by address alone (pitch too when linear), however old
 * the surface: memory a title frees and refills with a CPU-made texture
 * would still read the old surface. Not seen in Conker yet. */

#define RTT_ALPHA_ONE 1u
#define RTT_SWAP_RB   2u

/* Frame trace (RECOMP_FRAME_TRACE): where each stage's texture came from,
 * 'S' a GPU surface (slot s_tex_slot), 'Z' a zeta buffer, 'G' guest memory,
 * '-' nothing bound. Set by texture_stage. */
static char s_tex_src[4];
static int  s_tex_slot[4];
static int  s_rtt_slot;

static struct {
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} s_rtt[4];

/* How texture format fmt reads a surface of bpp bytes a pixel: RTT_* flags,
 * or -1 when that reading is not handled. */
static int rtt_reading(uint32_t fmt, uint32_t bpp)
{
    if (bpp == 4)
        switch (fmt) {
        case 0x06: case 0x12: return 0;                     /* A8R8G8B8 */
        case 0x07: case 0x1E: return RTT_ALPHA_ONE;         /* X8R8G8B8 */
        case 0x3F:            return RTT_SWAP_RB;           /* LIN_A8B8G8R8 */
        default:              return -1;
        }
    if (bpp == 2)
        switch (fmt) {
        /* The GPU keeps a 16-bit surface at 8 bits a channel; its alpha is
         * never written (blend_state), so it reads 1 anyway. */
        case 0x05: case 0x11: return RTT_ALPHA_ONE;         /* R5G6B5 */
        default:              return -1;
        }
    return -1;
}

/* Stage st's texture as a copy of the GPU surface it starts at, with its
 * reading in *flags; NULL when no GPU surface is there (or one is, read in a
 * way not handled: counted, and guest memory is used as before). */
static ID3D11ShaderResourceView *texture_from_surface(int st, uint32_t *flags)
{
    const Texture *t = &g_pb.texs[st];
    uint32_t swz = d3d8_format_is_swizzled(t->color) ? 1u : 0u, tw, th;
    D3D11_BOX box;
    int i, r;

    if (d3d8_format_dxt_block_bytes(t->color))
        return NULL;
    for (i = 0; i < GPU_SURFACES; i++)
        if (s_surf[i].tex
            && (s_surf[i].addr & 0x0FFFFFFFu) == (t->offset & 0x0FFFFFFFu)
            && s_surf[i].swz == swz
            && (swz || s_surf[i].pitch == t->pitch))
            break;
    if (i == GPU_SURFACES)
        return NULL;
    {
        /* Diagnostic: a surface not drawn on this flip or the last, or a
         * second surface at the same address (another pitch) that the
         * lookup above did not pick. Logged once per address and kind. */
        static struct { uint32_t addr, kind; } seen[128];
        static int nseen;
        uint32_t age = g_pb.flips - s_surf[i].drawn_flip, kind = 0;
        int k, twin = -1;
        for (k = i + 1; k < GPU_SURFACES; k++)
            if (s_surf[k].tex
                && (s_surf[k].addr & 0x0FFFFFFFu) == (t->offset & 0x0FFFFFFFu))
                twin = k;
        if (age > 1) {
            s_stat.rtt_old++;
            kind |= 1;
        }
        if (twin >= 0) {
            s_stat.rtt_twin++;
            kind |= 2;
        }
        for (k = 0; kind && k < nseen; k++)
            if (seen[k].addr == t->offset && seen[k].kind == kind)
                kind = 0;
        if (kind && nseen < 128) {
            seen[nseen].addr = t->offset;
            seen[nseen++].kind = kind;
            fprintf(stderr, "[GPU] d3d11: rtt stage %d texture 0x%08X fmt"
                    " 0x%02X %ux%u: surface slot %d 0x%08X pitch %u%s last"
                    " drawn %u flips ago", st, t->offset, t->color,
                    t->width, t->height, i, s_surf[i].addr, s_surf[i].pitch,
                    s_surf[i].swz ? " swizzled" : "", age);
            if (twin >= 0)
                fprintf(stderr, "; twin slot %d pitch %u drawn %u flips ago",
                        twin, s_surf[twin].pitch,
                        g_pb.flips - s_surf[twin].drawn_flip);
            fprintf(stderr, " (flip %u)%c", g_pb.flips, 10);
        }
    }
    r = rtt_reading(t->color, s_surf[i].bpp);
    if (r < 0) {
        if (s_stat.rtt_unhandled++ < 8)
            fprintf(stderr, "[GPU] d3d11: texture 0x%08X fmt 0x%02X %ux%u"
                    " is a GPU surface (%u bpp) read in a way not handled;"
                    " read from guest memory%c", t->offset, t->color,
                    t->width, t->height, s_surf[i].bpp, 10);
        return NULL;
    }

    /* The copy is the texture's rectangle at the surface's scale: a frame
     * drawn at 1920x1080 is read back at 1920x1080. Texture coordinates
     * stay in the guest's units (tscale in d3d_draw), so they still land on
     * the same place in it. */
    tw = to_host(t->width, surf_sx(i));
    th = to_host(t->height, surf_sy(i));
    if (!s_rtt[st].tex || s_rtt[st].w != tw || s_rtt[st].h != th) {
        D3D11_TEXTURE2D_DESC d;
        if (s_rtt[st].srv) ID3D11ShaderResourceView_Release(s_rtt[st].srv);
        if (s_rtt[st].tex) ID3D11Texture2D_Release(s_rtt[st].tex);
        memset(&s_rtt[st], 0, sizeof s_rtt[st]);
        memset(&d, 0, sizeof d);
        d.Width = tw;
        d.Height = th;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL,
                                                &s_rtt[st].tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev,
                    (ID3D11Resource *)s_rtt[st].tex, NULL, &s_rtt[st].srv))) {
            if (s_rtt[st].tex) ID3D11Texture2D_Release(s_rtt[st].tex);
            memset(&s_rtt[st], 0, sizeof s_rtt[st]);
            return NULL;
        }
        s_rtt[st].w = tw;
        s_rtt[st].h = th;
    }
    /* What the surface has of the texture's rectangle; past its edge the
     * copy keeps whatever it held. */
    box.left = 0; box.top = 0; box.front = 0;
    box.right = tw < s_surf[i].tw ? tw : s_surf[i].tw;
    box.bottom = th < s_surf[i].th ? th : s_surf[i].th;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
            (ID3D11Resource *)s_rtt[st].tex, 0, 0, 0, 0,
            (ID3D11Resource *)s_surf[i].tex, 0, &box);
    *flags = (uint32_t)r;
    s_surf[i].used = ++s_surf_tick;
    s_stat.rtt_binds++;
    s_rtt_slot = i;
    return s_rtt[st].srv;
}

/* A zeta surface read as a texture: Conker's glow bright pass samples its
 * Z24S8 buffer as LIN_R8G8B8A8 (stencil byte = alpha) and alpha-tests it,
 * and another pass reads it as LIN_A8B8G8R8 (alpha = depth's top byte).
 * The software path writes the buffer into guest memory first
 * (zeta_readback); here a pass draws the same dwords, decoded in the
 * texture's format, into a colour texture of the texture's size, one per
 * stage. Linear 32-bit readings of a Z24S8 buffer only, as zeta_readback
 * writes only linear textures; anything else is counted and read from guest
 * memory, which nothing writes. */
static struct {
    uint32_t w, h;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
} s_zcopy[4];

static ID3D11ShaderResourceView *texture_from_zeta(int st)
{
    const Texture *t = &g_pb.texs[st];
    ID3D11ShaderResourceView *srv[2];
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    uint32_t zk[4], tw, th;
    int i, k;

    /* Of a zeta surface's buffers (one per scale), the one drawn last. */
    for (i = -1, k = 0; k < GPU_ZETAS; k++)
        if (s_zeta[k].tex
            && (s_zeta[k].offset & 0x0FFFFFFFu) == (t->offset & 0x0FFFFFFFu)
            && (i < 0 || s_zeta[k].used > s_zeta[i].used))
            i = k;
    if (i < 0)
        return NULL;
    if (!s_zcb || s_zeta[i].zf == 1 || tex_size_from_format(t->color)
        || !(t->color == 0x12 || t->color == 0x1E || t->color == 0x3F
             || t->color == 0x40 || t->color == 0x41)) {
        if (s_stat.zeta_unhandled++ < 8)
            fprintf(stderr, "[GPU] d3d11: texture 0x%08X fmt 0x%02X %ux%u"
                    " is a zeta surface (format %u) read in a way not"
                    " handled; read from guest memory%c", t->offset,
                    t->color, t->width, t->height, s_zeta[i].zf, 10);
        return NULL;
    }

    /* At the buffer's scale, pixel for pixel, as texture_from_surface. */
    tw = to_host(t->width, s_zeta[i].scaled ? s_rsx : 1.0f);
    th = to_host(t->height, s_zeta[i].scaled ? s_rsy : 1.0f);
    if (!s_zcopy[st].tex || s_zcopy[st].w != tw || s_zcopy[st].h != th) {
        D3D11_TEXTURE2D_DESC d;
        if (s_zcopy[st].srv) ID3D11ShaderResourceView_Release(s_zcopy[st].srv);
        if (s_zcopy[st].rtv) ID3D11RenderTargetView_Release(s_zcopy[st].rtv);
        if (s_zcopy[st].tex) ID3D11Texture2D_Release(s_zcopy[st].tex);
        memset(&s_zcopy[st], 0, sizeof s_zcopy[st]);
        memset(&d, 0, sizeof d);
        d.Width = tw;
        d.Height = th;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL,
                                                &s_zcopy[st].tex))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev,
                    (ID3D11Resource *)s_zcopy[st].tex, NULL, &s_zcopy[st].rtv))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev,
                    (ID3D11Resource *)s_zcopy[st].tex, NULL,
                    &s_zcopy[st].srv))) {
            if (s_zcopy[st].rtv) ID3D11RenderTargetView_Release(s_zcopy[st].rtv);
            if (s_zcopy[st].tex) ID3D11Texture2D_Release(s_zcopy[st].tex);
            memset(&s_zcopy[st], 0, sizeof s_zcopy[st]);
            return NULL;
        }
        s_zcopy[st].w = tw;
        s_zcopy[st].h = th;
    }

    zk[0] = t->color;
    zk[1] = s_zeta[i].tw;
    zk[2] = s_zeta[i].th;
    zk[3] = 0;
    if (!buf_fill(s_zcb, zk, sizeof zk))
        return NULL;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)tw;
    vp.Height = (float)th;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = 0;
    sc.top = 0;
    sc.right = (LONG)tw;
    sc.bottom = (LONG)th;
    /* The target first: it unbinds the depth buffer from the batch before,
     * which D3D would otherwise refuse to let the shader read. */
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_zcopy[st].rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_zvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_zps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_zcb);
    srv[0] = s_zeta[i].srv_z;
    srv[1] = s_zeta[i].srv_s;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 2, srv);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    /* Unbound again, so the depth buffer can be a target once more and the
     * copy can be read: D3D drops a shader view of a bound target. */
    srv[0] = srv[1] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 2, srv);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    s_stat.zeta_reads++;
    return s_zcopy[st].srv;
}

/* A texture read from guest memory although its bytes overlap a GPU colour
 * surface or zeta buffer. This back end writes neither back to guest memory,
 * so such a read sees whatever was there before. In RECOMP_GPU=both the
 * software path has written those bytes first, which hides it. Counted
 * always; each address/format/size logged once (first 256), with the surface
 * it overlaps. Surfaces are taken as pitch x their (padded) height, zeta
 * buffers as 4 MB: generous on purpose, the log says which overlap is real. */
static void note_stale_read(int st, uint32_t bytes)
{
    static struct { uint32_t addr, fmt, wh; } seen[256];
    static int nseen;
    const Texture *t = &g_pb.texs[st];
    uint32_t a = t->offset & 0x0FFFFFFFu, b = a + bytes;
    uint32_t key = (t->width << 16) | t->height;
    int i, k;

    for (i = 0; i < GPU_SURFACES; i++) {
        uint32_t s = s_surf[i].addr & 0x0FFFFFFFu;
        if (s_surf[i].tex && a < s + s_surf[i].pitch * s_surf[i].h && s < b)
            break;
    }
    for (k = 0; i == GPU_SURFACES && k < GPU_ZETAS; k++) {
        uint32_t s = s_zeta[k].offset & 0x0FFFFFFFu;
        if (s_zeta[k].tex && a < s + (4u << 20) && s < b)
            break;
    }
    if (i == GPU_SURFACES && k == GPU_ZETAS)
        return;
    s_stat.stale_reads++;
    for (k = 0; k < nseen; k++)
        if (seen[k].addr == a && seen[k].fmt == t->color && seen[k].wh == key)
            return;
    if (nseen == 256)
        return;
    seen[nseen].addr = a;
    seen[nseen].fmt = t->color;
    seen[nseen++].wh = key;
    if (i < GPU_SURFACES)
        fprintf(stderr, "[GPU] d3d11: stale read: stage %d texture 0x%08X"
                " fmt 0x%02X %ux%u pitch %u (%u bytes) overlaps GPU surface"
                " 0x%08X pitch %u %ux%u%s at +0x%X (flip %u)%c", st, a,
                t->color, t->width, t->height, t->pitch, bytes,
                s_surf[i].addr, s_surf[i].pitch, s_surf[i].w, s_surf[i].h,
                s_surf[i].swz ? " swizzled" : "",
                a - (s_surf[i].addr & 0x0FFFFFFFu), g_pb.flips, 10);
    else
        for (k = 0; k < GPU_ZETAS; k++) {
            uint32_t s = s_zeta[k].offset & 0x0FFFFFFFu;
            if (s_zeta[k].tex && a < s + (4u << 20) && s < b) {
                fprintf(stderr, "[GPU] d3d11: stale read: stage %d texture"
                        " 0x%08X fmt 0x%02X %ux%u pitch %u (%u bytes)"
                        " overlaps zeta 0x%08X (format %u) at %+d (flip %u)%c",
                        st, a, t->color, t->width, t->height, t->pitch,
                        bytes, s_zeta[k].offset, s_zeta[k].zf,
                        (int)(a - s), g_pb.flips, 10);
                break;
            }
        }
}

/* A shader view of stage st's texture as it is now, or NULL when it cannot
 * be read; RTT_* flags in *flags. */
static ID3D11ShaderResourceView *texture_stage(int st, uint32_t *flags)
{
    const Texture *t = &g_pb.texs[st];
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes, x, y, palette;
    uint64_t hash;
    GpuTexture *e = NULL;
    ID3D11ShaderResourceView *rtt;
    int i, lru = 0;

    *flags = 0;
    s_tex_src[st] = '-';
    if (!t->valid || t->cube || !t->offset || !t->width || !t->height
        || t->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || t->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return NULL;
    rtt = texture_from_zeta(st);
    if (rtt) {
        s_tex_src[st] = 'Z';
        return rtt;
    }
    rtt = texture_from_surface(st, flags);
    if (rtt) {
        s_tex_src[st] = 'S';
        s_tex_slot[st] = s_rtt_slot;
        return rtt;
    }
    s_tex_src[st] = 'G';
    bytes = pb_tex_bytes(t);
    if (!bytes)
        return NULL;
    note_stale_read(st, bytes);
    if (s_nevicted && was_evicted(t->offset & 0x0FFFFFFFu)
        && s_stat.evicted_reads++ < 8)
        fprintf(stderr, "[GPU] d3d11: stage %d texture 0x%08X fmt 0x%02X"
                " %ux%u is a surface evicted from the table; read from"
                " guest memory (flip %u)%c", st, t->offset, t->color,
                t->width, t->height, g_pb.flips, 10);
    palette = t->color == 0x0B ? t->palette : 0;

    s_tex_tick++;
    for (i = s_tex_head[tex_bucket(t->offset)]; i; i = s_tex_next[i - 1]) {
        GpuTexture *c = &s_tex[i - 1];
        if (c->addr == t->offset && c->fmt == t->color
            && c->w == t->width && c->h == t->height && c->pitch == t->pitch
            && c->palette == palette) {
            e = c;
            break;
        }
    }
    /* Hashed once a frame, not once a bind. A busy scene binds the same few
     * hundred textures thousands of times a frame, and hashing every byte of
     * each one each time was a quarter of the executor's time (Conker's first
     * field). ponytail: a texture the title rewrites between two binds in
     * the same frame keeps the first bind's contents until the next flip. */
    if (e && e->hashed_flip == g_pb.flips) {
        e->used = s_tex_tick;
        s_stat.tex_reused++;
        return e->srv;
    }
    hash = hash_bytes(mem + t->offset, bytes, 0xCBF29CE484222325ull);
    if (palette)
        hash = hash_bytes(mem + palette, 256 * 4, hash);
    if (e && e->hash == hash) {
        e->hashed_flip = g_pb.flips;
        e->used = s_tex_tick;
        s_stat.tex_reused++;
        return e->srv;
    }

    /* New or changed: decode level 0. A format the sampler cannot read
     * comes out NULL, as the software sampler's fallback. */
    if ((size_t)t->width * t->height > s_texels_cap) {
        free(s_texels);
        s_texels_cap = (size_t)t->width * t->height;
        s_texels = (uint32_t *)malloc(s_texels_cap * 4);
        if (!s_texels) {
            s_texels_cap = 0;
            return NULL;
        }
    }
    if (!pb_tex_texel(t, 0, 0, &s_texels[0])) {
        s_stat.tex_unreadable++;
        return NULL;
    }
    for (y = 0; y < t->height; y++)
        for (x = 0; x < t->width; x++)
            if (!pb_tex_texel(t, x, y, &s_texels[(size_t)y * t->width + x]))
                s_texels[(size_t)y * t->width + x] = 0xFFFFFFFFu;

    if (!e) {
        D3D11_TEXTURE2D_DESC d;
        for (i = 0; i < GPU_TEXTURES; i++)
            if (!s_tex[i].tex
                || (s_tex[lru].tex && s_tex[i].used < s_tex[lru].used))
                lru = i;
        e = &s_tex[lru];
        if (e->tex)
            tex_unlink(lru);
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        memset(&d, 0, sizeof d);
        d.Width = t->width;
        d.Height = t->height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &e->tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev,
                    (ID3D11Resource *)e->tex, NULL, &e->srv))) {
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            memset(e, 0, sizeof *e);
            return NULL;
        }
        e->addr = t->offset;
        e->fmt = t->color;
        e->w = t->width;
        e->h = t->height;
        e->pitch = t->pitch;
        e->palette = palette;
        s_tex_next[lru] = s_tex_head[tex_bucket(e->addr)];
        s_tex_head[tex_bucket(e->addr)] = (uint16_t)(lru + 1);
        s_stat.tex_made++;
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, 0,
                                          NULL, s_texels, t->width * 4, 0);
    e->hash = hash;
    e->hashed_flip = g_pb.flips;
    e->used = s_tex_tick;
    s_stat.tex_uploads++;
    return e->srv;
}

/* Sampler states: point or bilinear, wrap or clamp per axis. */
static ID3D11SamplerState *s_samp[8];

/* Border mode (address 4 with FORMAT's border-from-colour bit), as the
 * software sampler does it: outside the image the texture reads the border
 * colour. Conker projects its room light maps with a white border, so a
 * character outside the map is fully lit; clamped, it took the map's edge
 * texels and came out darker. Few distinct border colours exist, so these
 * are cached by key and colour in a short list. */
static struct { int k; uint32_t color; ID3D11SamplerState *s; } s_samp_border[16];

static ID3D11SamplerState *sampler_border(int lin, int bu, int bv,
                                          int wu, int wv, uint32_t argb)
{
    int k = lin | bu << 1 | bv << 2 | wu << 3 | wv << 4, i;
    D3D11_SAMPLER_DESC d;

    for (i = 0; i < 16 && s_samp_border[i].s; i++)
        if (s_samp_border[i].k == k && s_samp_border[i].color == argb)
            return s_samp_border[i].s;
    if (i == 16)
        return NULL;                    /* full: caller falls back to clamp */
    memset(&d, 0, sizeof d);
    d.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                   : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = bu ? D3D11_TEXTURE_ADDRESS_BORDER
               : wu ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = bv ? D3D11_TEXTURE_ADDRESS_BORDER
               : wv ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.BorderColor[0] = ((argb >> 16) & 0xFF) / 255.0f;
    d.BorderColor[1] = ((argb >>  8) & 0xFF) / 255.0f;
    d.BorderColor[2] = ( argb        & 0xFF) / 255.0f;
    d.BorderColor[3] = ((argb >> 24) & 0xFF) / 255.0f;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s_samp_border[i].s)))
        return NULL;
    s_samp_border[i].k = k;
    s_samp_border[i].color = argb;
    return s_samp_border[i].s;
}

static ID3D11SamplerState *sampler_stage(int st, int combiners)
{
    const Texture *t = &g_pb.texs[st];
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    /* Bilinear only where the software path filters: in the combiner path,
     * when the title asks for a tent (rc_texel); the other paths always
     * take the nearest texel. */
    int lin = combiners && (mag == 2 || min == 2 || min == 4 || min == 6);
    int k = lin | (t->addr_u == 1) << 1 | (t->addr_v == 1) << 2;

    if (t->border_from_color && (t->addr_u == 4 || t->addr_v == 4)) {
        ID3D11SamplerState *b = sampler_border(lin, t->addr_u == 4,
                                               t->addr_v == 4, t->addr_u == 1,
                                               t->addr_v == 1, t->border_color);
        if (b)
            return b;
    }
    if (!s_samp[k]) {
        D3D11_SAMPLER_DESC d;
        memset(&d, 0, sizeof d);
        d.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                       : D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.AddressU = (k & 2) ? D3D11_TEXTURE_ADDRESS_WRAP
                             : D3D11_TEXTURE_ADDRESS_CLAMP;
        d.AddressV = (k & 4) ? D3D11_TEXTURE_ADDRESS_WRAP
                             : D3D11_TEXTURE_ADDRESS_CLAMP;
        d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s_samp[k])))
            return NULL;
    }
    return s_samp[k];
}

/* Near-plane clipping, a port of raster_xf_clipped (nv2a_draw_sw.c): a
 * triangle with a corner behind the eye (w <= NV_CLIP_W) or in front of the
 * near plane (depth below 0) is taken back to clip space, clipped against
 * both planes, re-projected and fanned. The pieces are appended to the
 * batch's vertices (s_cv, after the batch's own n) and indexed like any
 * other triangle. Keep this in step with the C version: the same arithmetic
 * in the same order gives the same vertices, and the same corner sort keeps
 * depth bit-identical between passes that send the triangle in another
 * vertex order (the lighting layers test LEQUAL against the base layer). */
static Nv2aVshOutput *s_cv;             /* clipped vertices, grown */
static uint32_t       s_cv_cap, s_cv_n;

static Nv2aVshOutput *clip_vertex(void)
{
    if (s_cv_n == s_cv_cap) {
        uint32_t cap = s_cv_cap ? s_cv_cap * 2 : 1024;
        Nv2aVshOutput *p = (Nv2aVshOutput *)realloc(s_cv, cap * sizeof *p);
        if (!p)
            return NULL;
        s_cv = p;
        s_cv_cap = cap;
    }
    return &s_cv[s_cv_n++];
}

static void xf_to_clip(const Nv2aVshOutput *v, const float k[3],
                       const float o[3], float c[3])
{
    int i;
    for (i = 0; i < 3; i++)
        c[i] = k[i] != 0.0f ? (v->pos[i] - o[i]) / k[i] * v->pos[3] : 0.0f;
}

static void xf_lerp(const Nv2aVshOutput *a, const Nv2aVshOutput *b, float t,
                    Nv2aVshOutput *out)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    size_t i, n = sizeof(Nv2aVshOutput) / sizeof(float);
    for (i = 0; i < n; i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static float xf_plane_dist(const Nv2aVshOutput *v, int p)
{
    return p == 0 ? v->pos[3] - NV_CLIP_W : v->pos[2];
}

/* Clip triangle a b c of batch v (n vertices); writes the fan's indices at
 * ib and returns how many. */
static uint32_t clip_triangle(const Nv2aVshOutput *v, uint32_t n,
                              uint32_t a, uint32_t b, uint32_t c, uint32_t *ib)
{
    const Nv2aVshOutput *in[3];
    Nv2aVshOutput poly[2][8];
    const float *sc = nv2a_vsh_constant(58), *of = nv2a_vsh_constant(59);
    float k[3], o[3], cc[3];
    int m = 3, cur = 0, i, j, p;
    uint32_t base, out = 0;

    for (i = 0; i < 3; i++) { k[i] = sc[i]; o[i] = of[i]; }
    in[0] = &v[a]; in[1] = &v[b]; in[2] = &v[c];
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2 - i; j++)
            if (memcmp(in[j]->pos, in[j + 1]->pos, sizeof in[j]->pos) > 0) {
                const Nv2aVshOutput *t = in[j];
                in[j] = in[j + 1];
                in[j + 1] = t;
            }
    for (i = 0; i < 3; i++) {
        poly[0][i] = *in[i];
        xf_to_clip(in[i], k, o, cc);
        for (j = 0; j < 3; j++)
            poly[0][i].pos[j] = cc[j];
    }
    /* Sutherland-Hodgman, one plane at a time; 3 -> at most 5 vertices. */
    for (p = 0; p < 2; p++) {
        const Nv2aVshOutput *src = poly[cur];
        Nv2aVshOutput *dst = poly[cur ^ 1];
        int q = 0;
        for (i = 0; i < m; i++) {
            const Nv2aVshOutput *P = &src[i], *Q = &src[(i + 1) % m];
            float dp = xf_plane_dist(P, p), dq = xf_plane_dist(Q, p);
            if (dp >= 0.0f)
                dst[q++] = *P;
            if ((dp >= 0.0f) != (dq >= 0.0f))
                xf_lerp(P, Q, dp / (dp - dq), &dst[q++]);
        }
        m = q;
        cur ^= 1;
        if (m < 3) {
            s_stat.tris_behind++;               /* nothing left in front */
            return 0;
        }
    }
    base = n + s_cv_n;
    for (i = 0; i < m; i++) {
        Nv2aVshOutput *d = clip_vertex();
        if (!d) {
            s_cv_n -= (uint32_t)i;
            return 0;
        }
        *d = poly[cur][i];
        for (j = 0; j < 3; j++)
            d->pos[j] = d->pos[j] / d->pos[3] * k[j] + o[j];
    }
    for (i = 1; i + 1 < m; i++) {
        ib[out++] = base;
        ib[out++] = base + (uint32_t)i;
        ib[out++] = base + (uint32_t)i + 1;
    }
    s_stat.tris_clipped++;
    return out;
}

/* The triangle list for the batch's primitive, as raster_batch cuts it, as
 * indices into the batch's vertices v (n of them). Returns the index count.
 * Triangles with an unusable vertex are left out, as raster_indexed and
 * raster_xf_triangle leave them out; triangles crossing the near plane are
 * clipped into s_cv. */
static uint32_t build_triangles(const Nv2aVshOutput *v, uint32_t n)
{
    uint32_t i, out = 0;

    s_cv_n = 0;
    /* A strip's odd triangles wind the other way: culling tests them with
     * the first two corners swapped (the emitted order stays as it was). */
#define TRI(a, b, c) TRI_W(a, b, c, 0)
#define TRI_W(a, b, c, odd) do {                                        \
        uint8_t ka = s_bv_ok[a], kb = s_bv_ok[b], kc = s_bv_ok[c];      \
        if (ka && kb && kc && ((odd) ? pb_cull_tri(&v[b], &v[a], &v[c]) \
                                     : pb_cull_tri(&v[a], &v[b], &v[c]))) \
            s_stat.tris_culled++;                                       \
        else if (ka == BV_OK && kb == BV_OK && kc == BV_OK) {           \
            s_ib[out++] = (a); s_ib[out++] = (b); s_ib[out++] = (c);    \
        } else if (ka && kb && kc) {                                    \
            out += clip_triangle(v, n, (a), (b), (c), &s_ib[out]);      \
        } } while (0)

    switch (g_pb.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) TRI_W(i, i + 1, i + 2, i & 1);
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            TRI(i, i + 1, i + 2);
            TRI(i, i + 2, i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            TRI(i, i + 1, i + 3);
            TRI(i, i + 3, i + 2);
        }
        break;
    case NV_PRIM_POINTS:
        /* Each point a sprite square (pb_point_quad), its corners appended
         * after the batch like clipped vertices. */
        for (i = 0; i < n; i++) {
            Nv2aVshOutput q[4], *d;
            uint32_t base = n + s_cv_n, j;
            if (!s_bv_ok[i] || !pb_point_quad(&v[i], q))
                continue;
            for (j = 0; j < 4; j++) {
                if (!(d = clip_vertex()))
                    break;
                *d = q[j];
            }
            if (j < 4) {
                s_cv_n -= j;
                break;
            }
            s_ib[out++] = base;     s_ib[out++] = base + 1; s_ib[out++] = base + 2;
            s_ib[out++] = base;     s_ib[out++] = base + 2; s_ib[out++] = base + 3;
        }
        break;
    default:
        break;                          /* lines: not yet */
    }
#undef TRI
#undef TRI_W
    return out;
}

/* Which vertices of an Nv2aVshOutput batch can be drawn as they are (BV_OK:
 * in front of the eye and past the near plane, raster_xf_clipped's test),
 * need their triangles clipped (BV_CLIP), or can't be used (0: not
 * finite). */
static void mark_unclipped(const Nv2aVshOutput *v, uint32_t n)
{
    const float *zs = nv2a_vsh_constant(58), *zo = nv2a_vsh_constant(59);
    uint32_t i;

    for (i = 0; i < n; i++) {
        const float *p = v[i].pos;
        if (!(isfinite(p[0]) && isfinite(p[1])
              && isfinite(p[2]) && isfinite(p[3])))
            s_bv_ok[i] = 0;
        else if (p[3] > NV_CLIP_W
                 && !(zs[2] != 0.0f && (p[2] - zo[2]) / zs[2] < 0.0f))
            s_bv_ok[i] = BV_OK;
        else {
            s_bv_ok[i] = BV_CLIP;
            s_stat.verts_need_clip++;
        }
    }
}

/* The combiner registers and the per-pixel state around them. */
static void consts_combiners(GpuConsts *k)
{
    uint32_t s;

    k->stage[0] = g_pb.rc.stage_program;
    k->stage[1] = g_pb.clip_plane_mode;
    k->stage[2] = g_pb.rc.control;
    k->stage[3] = g_pb.fog_enable ? 0x10000u | (g_pb.fog_mode & 0xFFFF) : 0;
    for (s = 0; s < 8; s++) {
        k->icw[s][0] = g_pb.rc.color_icw[s];
        k->icw[s][1] = g_pb.rc.alpha_icw[s];
        k->icw[s][2] = g_pb.rc.color_ocw[s];
        k->icw[s][3] = g_pb.rc.alpha_ocw[s];
        nv2a_rc_unpack(g_pb.rc.factor0[s], k->cf0[s]);
        nv2a_rc_unpack(g_pb.rc.factor1[s], k->cf1[s]);
    }
    k->fin[0] = g_pb.rc.final0;
    k->fin[1] = g_pb.rc.final1;
    nv2a_rc_unpack(g_pb.rc.final_c0, k->fc0);
    nv2a_rc_unpack(g_pb.rc.final_c1, k->fc1);
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    nv2a_rc_unpack(g_pb.fog_color, k->fogc);
    { float r = k->fogc[2]; k->fogc[2] = k->fogc[0]; k->fogc[0] = r; }
    k->fogp[0] = g_pb.fog_param[0];
    k->fogp[1] = g_pb.fog_param[1];
}

static void heavy_note(const Nv2aVshOutput *verts, uint32_t n, uint32_t tris);
static void heavy_report(void);
static void vis_note_scale(float area);

/* Keep the executor at most PACE_SLOTS x PACE_DRAWS draws ahead of the host
 * GPU. The guest takes a batch as done once it is handed to D3D11, so a
 * title that draws without presenting (Conker's level loads: ~12,000
 * full-screen passes, 2026-10-13) queues seconds of GPU work, and the next
 * ring wrap's discard then waits for all of it while D3D's GPU DPC spins and
 * no interrupt gets through. An event query every PACE_DRAWS draws; before a
 * slot is reused, its query must have passed. A frame (~450-1000 draws) is
 * far inside the window, so ordinary frames never wait. RECOMP_GPU_PACE=0
 * turns it off. */
#define PACE_DRAWS 64
#define PACE_SLOTS 16
static ID3D11Query *s_pace_q[PACE_SLOTS];
static uint32_t s_pace_draws, s_pace_slot, s_pace_waits;
static double s_pace_wait_ms;

static void pace_draw(void)
{
    static int on = -1;
    ID3D11Query **q;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPU_PACE");
        on = !(e && e[0] == '0');
    }
    if (!on || ++s_pace_draws % PACE_DRAWS)
        return;
    q = &s_pace_q[s_pace_slot];
    s_pace_slot = (s_pace_slot + 1) % PACE_SLOTS;
    if (!*q) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_EVENT;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, q))) {
            *q = NULL;
            return;
        }
    } else {
        BOOL done = FALSE;
        LARGE_INTEGER f, t0, t1;
        HRESULT hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)*q,
                                                 &done, sizeof done, 0);
        if (hr == S_FALSE) {
            QueryPerformanceCounter(&t0);
            while ((hr = ID3D11DeviceContext_GetData(s_ctx,
                        (ID3D11Asynchronous *)*q, &done, sizeof done, 0)) == S_FALSE)
                SwitchToThread();
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            s_pace_waits++;
            s_pace_wait_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0
                              / (double)f.QuadPart;
        }
    }
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)*q);
}

static void d3d_draw(void)
{
    static int ready = -1, no_rc = -1, no_vsh = -1;
    static GpuConsts k;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11ShaderResourceView *srv[4] = {NULL, NULL, NULL, NULL};
    ID3D11SamplerState *samp[4] = {NULL, NULL, NULL, NULL};
    const Nv2aVshOutput *verts = s_bv;
    float bf[4];
    uint32_t n, i, nidx, bpp, bound = 0, stride = sizeof(Nv2aVshOutput);
    uint32_t offset = 0, ib_offset = 0, path, rtt;
    int si, st, combiners, program, zi;
    ID3D11DepthStencilState *ds;
    uint64_t ct_draw0;

    s_stat.batches++;
    if (ready < 0)
        ready = draw_setup();
    if (no_rc < 0)
        no_rc = getenv("RECOMP_NO_COMBINERS") != NULL;
    if (no_vsh < 0)
        no_vsh = getenv("RECOMP_NO_VSH") != NULL;
    if (!ready || g_pb.idx_count < (g_pb.prim == NV_PRIM_POINTS ? 1u : 3u))
        return;
    program = (g_pb.xf_mode & 3) == 2 && !no_vsh;
    if (!program && !pb_batch_screen_space()) {
        s_stat.skip_not_screen++;
        return;
    }
    bpp = pb_surface_bpp();
    si = bpp == 2 || bpp == 4 ? surface_current() : -1;
    if (si < 0) {
        s_stat.skip_surface++;
        return;
    }
    combiners = g_pb.rc_seen && !no_rc;
    n = g_pb.idx_count;
    memset(&k, 0, sizeof k);

    if (program || combiners) {
        /* Through the title's own program, or the fixed-function vertex
         * built the way the combiner path takes it. Every stage the program
         * samples is bound; coordinates are normalised for swizzled and DXT
         * formats and texels otherwise (rc_texel). */
        if (program) {
            verts = pb_transform_batch();
            if (!verts) {
                s_stat.skip_program++;
                return;
            }
        } else {
            for (i = 0; i < n; i++)
                pb_fixed_vertex(g_pb.idx[i], &s_bv[i]);
        }
        uint64_t ct0;
        mark_unclipped(verts, n);
        path = combiners ? PATH_COMBINERS : PATH_MODULATE;
        ct0 = ct_now();
        for (st = 0; st < 4; st++) {
            uint32_t m = (g_pb.rc.stage_program >> (st * 5)) & 0x1F;
            const Texture *t = &g_pb.texs[st];
            /* Without combiners only stage 0 is sampled (xf_rows). Bump-env
             * stages (6, 7) sample too, at coordinates the shader shifts. */
            if (combiners ? !((m >= 1 && m <= 3) || m == 6 || m == 7) : st != 0)
                continue;
            if (m == 6 || m == 7) {
                memcpy(k.bmat[st], t->bump_mat, sizeof k.bmat[st]);
                k.bsc[st][0] = t->bump_scale;
                k.bsc[st][1] = t->bump_offset;
                k.bsc[st][2] = (float)pb_rc_input_stage(st);
            }
            srv[st] = texture_stage(st, &rtt);
            samp[st] = srv[st] ? sampler_stage(st, combiners) : NULL;
            if (!samp[st]) {
                srv[st] = NULL;
                continue;
            }
            bound |= 1u << st;
            k.tscale[st][0] = tex_size_from_format(t->color)
                              ? 1.0f : 1.0f / (float)t->width;
            k.tscale[st][1] = tex_size_from_format(t->color)
                              ? 1.0f : 1.0f / (float)t->height;
            k.tscale[st][2] = (rtt & RTT_ALPHA_ONE) ? 1.0f : 0.0f;
            k.tscale[st][3] = (rtt & RTT_SWAP_RB) ? 1.0f : 0.0f;
        }
        ct_add(CT_TEXTURES, ct0);
        if (g_pb.alpha_test) {
            k.mode[2] = g_pb.alpha_func;
            k.mode[3] = g_pb.alpha_ref & 0xFF;
        }
        if (combiners)
            consts_combiners(&k);
    } else {
        /* raster_indexed: stage 0 when every vertex carries texcoords for
         * it, in texels; flat colour otherwise. */
        int textured = 1;
        for (i = 0; i < n; i++) {
            float c[4], uv[2];
            Nv2aVshOutput *v = &s_bv[i];
            memset(v, 0, sizeof *v);
            s_bv_ok[i] = pb_fetch_attr(&g_pb.attr[0], g_pb.idx[i], v->pos)
                         ? BV_OK : 0;
            v->pos[2] = 0.0f;
            v->pos[3] = 1.0f;
            if (!pb_fetch_color(g_pb.idx[i], c))
                c[0] = c[1] = c[2] = c[3] = 1.0f;
            memcpy(v->d0, c, sizeof c);
            if (pb_fetch_texcoord(g_pb.idx[i], uv)) {
                v->tex[0][0] = uv[0];
                v->tex[0][1] = uv[1];
            } else {
                textured = 0;
            }
        }
        path = PATH_FLAT;
        if (textured) {
            srv[0] = texture_stage(0, &rtt);
            samp[0] = srv[0] ? sampler_stage(0, 0) : NULL;
            if (samp[0]) {
                bound = 1;
                k.tscale[0][0] = 1.0f / (float)g_pb.texs[0].width;
                k.tscale[0][1] = 1.0f / (float)g_pb.texs[0].height;
                k.tscale[0][2] = (rtt & RTT_ALPHA_ONE) ? 1.0f : 0.0f;
                k.tscale[0][3] = (rtt & RTT_SWAP_RB) ? 1.0f : 0.0f;
            } else {
                srv[0] = NULL;
            }
        }
    }

    nidx = build_triangles(verts, n);
    if (g_pb_ftrace == 2) {
        /* What this batch draws where, from which textures: the GPU-side
         * counterpart of sw_draw's trace, for runs without the software
         * path. Screen box and texture coordinate ranges are over the
         * batch's vertices (positions in surface pixels, as the vertex
         * shader takes them; coordinates as the pixel shader gets them). */
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (i = 0; i < n; i++) {
            float x = verts[i].pos[0], y = verts[i].pos[1];
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        fprintf(stderr, "[FTRACE] d3d %s n %u surf %08X slot %d %ux%u+%u+%u"
                " | box x %.1f..%.1f y %.1f..%.1f | blend %u %X/%X eq %X"
                " | z %u st %u at %u | tris %u prim %u%c",
                path == PATH_FLAT ? "FLAT" : program ? "PRG" : "RC", n,
                g_pb.color_offset, si, g_pb.clip_w, g_pb.clip_h,
                g_pb.clip_x, g_pb.clip_y, x0, x1, y0, y1, g_pb.blend_enable,
                g_pb.blend_sfactor, g_pb.blend_dfactor, g_pb.blend_equation,
                g_pb.depth_test, g_pb.stencil_test, g_pb.alpha_test,
                nidx / 3, g_pb.prim, 10);
        fprintf(stderr, "[FTRACE]     cmask %08X | stencil func %X ref %02X"
                " r %02X w %02X op %X/%X/%X | alpha func %X ref %02X | zmask"
                " %u zfunc %X | d0 %.2f %.2f %.2f %.2f%c", g_pb.color_mask,
                g_pb.stencil_func, g_pb.stencil_ref & 0xFF,
                g_pb.stencil_rmask & 0xFF, g_pb.stencil_wmask & 0xFF,
                g_pb.stencil_op_fail, g_pb.stencil_op_zfail,
                g_pb.stencil_op_zpass, g_pb.alpha_func, g_pb.alpha_ref & 0xFF,
                g_pb.depth_mask, g_pb.depth_func, verts[0].d0[0],
                verts[0].d0[1], verts[0].d0[2], verts[0].d0[3], 10);
        if (g_pb.cull_enable)
            fprintf(stderr, "[FTRACE]     cull face %X front %X%c",
                    g_pb.cull_face, g_pb.front_face, 10);
        if (g_pb.prim == NV_PRIM_POINTS) {
            float p0 = 1e30f, p1 = -1e30f;
            for (i = 0; i < n; i++) {
                if (verts[i].pts < p0) p0 = verts[i].pts;
                if (verts[i].pts > p1) p1 = verts[i].pts;
            }
            fprintf(stderr, "[FTRACE]     points: oPts %.2f..%.2f | params %u"
                    " sprite %u size %u/8 | %u drawn%c", p0, p1,
                    g_pb.point_params_enable, g_pb.point_sprite,
                    g_pb.point_size, nidx / 6, 10);
        }
        if (path == PATH_COMBINERS) {
            /* The combiner setup: which registers each stage reads. */
            uint32_t s, ns = g_pb.rc.control & 0xFF;
            fprintf(stderr, "[FTRACE]     rc prog %05X control %X final"
                    " %08X %08X fc0 %08X fc1 %08X | d1 %.2f %.2f %.2f %.2f%c",
                    g_pb.rc.stage_program, g_pb.rc.control, g_pb.rc.final0,
                    g_pb.rc.final1, g_pb.rc.final_c0, g_pb.rc.final_c1,
                    verts[0].d1[0], verts[0].d1[1], verts[0].d1[2],
                    verts[0].d1[3], 10);
            for (s = 0; s < ns && s < 8; s++)
                fprintf(stderr, "[FTRACE]     rc %u: rgb %08X->%05X a %08X->"
                        "%05X c0 %08X c1 %08X%c", s, g_pb.rc.color_icw[s],
                        g_pb.rc.color_ocw[s], g_pb.rc.alpha_icw[s],
                        g_pb.rc.alpha_ocw[s], g_pb.rc.factor0[s],
                        g_pb.rc.factor1[s], 10);
        }
        for (st = 0; st < 4; st++) {
            const Texture *t = &g_pb.texs[st];
            float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
            uint32_t m = (g_pb.rc.stage_program >> (st * 5)) & 0x1F;
            /* Unbound stages too, when the program uses them: a stage the
             * GPU could not make a texture of is the first thing to know. */
            if (!(bound & (1u << st)) && !(combiners && m))
                continue;
            if (!(bound & (1u << st)))
                fprintf(stderr, "[FTRACE]     t%d unbound mode %u (%c)%c", st,
                        m, s_tex_src[st], 10);
            for (i = 0; i < n; i++) {
                float u = verts[i].tex[st][0], v = verts[i].tex[st][1];
                if (u < u0) u0 = u;
                if (u > u1) u1 = u;
                if (v < v0) v0 = v;
                if (v > v1) v1 = v;
            }
            fprintf(stderr, "[FTRACE]     t%d %c", st, s_tex_src[st]);
            if (s_tex_src[st] == 'S')
                fprintf(stderr, "%d", s_tex_slot[st]);
            fprintf(stderr, " %08X fmt %02X %ux%u pitch %u | u %.2f..%.2f"
                    " v %.2f..%.2f q0 %.2f | addr %u/%u filter %08X%c",
                    t->offset, t->color, t->width, t->height, t->pitch, u0,
                    u1, v0, v1, verts[0].tex[st][3], t->addr_u, t->addr_v,
                    t->filter, 10);
            if (combiners && (m == 6 || m == 7)) {
                /* Bump-env takes the coordinates as they are, with no
                 * divide by q: its q range says whether that matters. */
                float q0 = 1e30f, q1 = -1e30f;
                for (i = 0; i < n; i++) {
                    if (verts[i].tex[st][3] < q0) q0 = verts[i].tex[st][3];
                    if (verts[i].tex[st][3] > q1) q1 = verts[i].tex[st][3];
                }
                fprintf(stderr, "[FTRACE]     t%d bump mat %g %g %g %g scale %g"
                        " offset %g from t%u | q %.2f..%.2f%c", st,
                        t->bump_mat[0], t->bump_mat[1], t->bump_mat[2],
                        t->bump_mat[3], t->bump_scale, t->bump_offset,
                        pb_rc_input_stage(st), q0, q1, 10);
            }
        }
        if (program) {
            /* What the program was fed: each attribute with an array (format
             * and its value at the first vertex), then every constant that
             * changed since the last traced program batch (all non-zero ones
             * at the first). A skinned model's bone palette shows up as the
             * block of constants uploaded before its batches; whether those
             * matrices differ from one another, and from frame to frame, tells
             * a model the title never posed from one we pose wrongly. */
            static float prev[NV2A_VSH_CONSTANTS][4];
            static int have_prev;
            uint32_t at, r, changed = 0;
            float d[4];
            for (at = 0; at < NV_VERTEX_ATTRS; at++) {
                const VertexAttr *va = &g_pb.attr[at];
                if (!va->size || !va->stride)
                    continue;
                pb_fetch_attr(va, g_pb.idx[0], d);
                fprintf(stderr, "[FTRACE]     attr %u type %u size %u stride %u"
                        " @%08X: v0 %g %g %g %g%c", at, va->type, va->size,
                        va->stride, va->offset, d[0], d[1], d[2], d[3], 10);
            }
            for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
                const float *c = nv2a_vsh_constant(r);
                if (have_prev ? !memcmp(c, prev[r], sizeof prev[r])
                              : !(c[0] || c[1] || c[2] || c[3]))
                    continue;
                changed++;
                fprintf(stderr, "[FTRACE]     c[%u] %g %g %g %g%c", r, c[0],
                        c[1], c[2], c[3], 10);
                memcpy(prev[r], c, sizeof prev[r]);
            }
            if (!have_prev)
                for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
                    memcpy(prev[r], nv2a_vsh_constant(r), sizeof prev[r]);
            have_prev = 1;
            fprintf(stderr, "[FTRACE]     %u constants changed%c", changed, 10);
        }
        if (program && n) {
            /* The colour outputs' alpha over the whole batch: a layer that
             * takes its strength from oD0.a (Conker's water glints, added
             * with SRC_ALPHA/ONE) draws nothing if the program never lifts
             * it off zero, and verts[0] alone cannot say that. */
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            uint32_t top = 0, want, ts;
            const char *e;
            for (i = 0; i < n; i++) {
                if (verts[i].d0[3] < a0) a0 = verts[i].d0[3];
                if (verts[i].d0[3] > a1) { a1 = verts[i].d0[3]; top = i; }
                if (verts[i].d1[3] < b0) b0 = verts[i].d1[3];
                if (verts[i].d1[3] > b1) b1 = verts[i].d1[3];
            }
            fprintf(stderr, "[FTRACE]     d0.a %.3f..%.3f (top vertex %u)"
                    " d1.a %.3f..%.3f%c", a0, a1, top, b0, b1, 10);
            /* RECOMP_FRAME_TRACE_VSTEP=<texture address>: a traced program
             * batch that binds that texture on any stage runs its top-alpha
             * vertex through the program again, instruction by instruction
             * ([VSTEP]), on GPU runs where the pixel probe cannot reach. */
            e = getenv("RECOMP_FRAME_TRACE_VSTEP");
            want = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
            for (ts = 0; want && ts < 4; ts++)
                if ((bound & (1u << ts)) && g_pb.texs[ts].offset == want) {
                    float in[NV2A_VSH_INPUTS][4];
                    uint32_t at;
                    for (at = 0; at < NV2A_VSH_INPUTS; at++)
                        pb_fetch_attr(&g_pb.attr[at], g_pb.idx[top], in[at]);
                    nv2a_vsh_trace((const float (*)[4])in);
                    break;
                }
        }
    }
    if (!nidx)
        return;
    {
        uint64_t ct0 = ct_now();
        int ok = ring_put(&s_vb, D3D11_BIND_VERTEX_BUFFER,
                          verts, (size_t)n * sizeof(Nv2aVshOutput),
                          s_cv, (size_t)s_cv_n * sizeof(Nv2aVshOutput),
                          &offset);
        ct_add(CT_VB, ct0);
        if (!ok)
            return;
        ct0 = ct_now();
        ok = ring_put(&s_ib_ring, D3D11_BIND_INDEX_BUFFER,
                      s_ib, (size_t)nidx * 4, NULL, 0, &ib_offset);
        ct_add(CT_IB, ct0);
        if (!ok)
            return;
    }

    /* Depth and stencil where raster_xf_triangle has them: on the program
     * and combiner paths, when the batch tests or writes either (the flat
     * path has no depth). */
    zi = -1;
    if (path != PATH_FLAT
        && (g_pb.depth_test || g_pb.depth_mask || g_pb.stencil_test)) {
        if (s_surf[si].w == ZETA_W && s_surf[si].h == ZETA_H)
            zi = zeta_current(1, s_surf[si].scaled);
        else
            s_stat.zeta_too_big++;
    }
    ds = zi >= 0 ? ds_state() : NULL;
    if (!ds)
        zi = -1;
    if (zi >= 0)
        s_zeta[zi].used = ++s_zeta_tick;

    /* Guest pixels to clip space over the whole surface; the viewport is
     * the texture's host size, so a scaled surface draws the same picture
     * bigger. */
    k.rt[0] = 2.0f / (float)s_surf[si].w;
    k.rt[1] = -2.0f / (float)s_surf[si].h;
    k.rt[2] = -1.0f;
    k.rt[3] = 1.0f;
    k.mode[0] = path;
    k.mode[1] = bound;
    k.zsc[0] = zeta_scale();
    {
        uint64_t ct0 = ct_now();
        int ok = buf_fill(s_cb, &k, sizeof k);
        ct_add(CT_CB, ct0);
        if (!ok)
            return;
    }

    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)s_surf[si].tw;
    vp.Height = (float)s_surf[si].th;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = (LONG)to_host(g_pb.clip_x, surf_sx(si));
    sc.top = (LONG)to_host(g_pb.clip_y, surf_sy(si));
    sc.right = (LONG)to_host(g_pb.clip_x + g_pb.clip_w, surf_sx(si));
    sc.bottom = (LONG)to_host(g_pb.clip_y + g_pb.clip_h, surf_sy(si));
    blend_constant(bf);
    vis_note_scale(surf_sx(si) * surf_sy(si));

    ct_draw0 = ct_now();
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb.b, &stride,
                                           &offset);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib_ring.b,
                                         DXGI_FORMAT_R32_UINT, ib_offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, samp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(bpp), bf,
                                        0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, ds,
            g_pb.stencil_ref & 0xFFu);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_surf[si].rtv,
            zi >= 0 ? s_zeta[zi].dsv : NULL);
    ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
    if (zi >= 0)
        s_stat.zeta_draws++;
    /* Unbound again, so a later batch may draw into a texture this one
     * sampled without D3D refusing the overlap. */
    memset(srv, 0, sizeof srv);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv);
    ct_add(CT_DRAW, ct_draw0);
    pace_draw();

    s_stat.drawn++;
    s_stat.tris += nidx / 3;
    if (bound)
        s_stat.textured++;
    if (path == PATH_COMBINERS)
        s_stat.combined++;
    note_shown(si);
    if (s_ct_on > 0)
        heavy_note(verts, n, nidx / 3);
}

/* RECOMP_GPU_CALL_TIMES: a present after 3000+ batches (a frame is ~1000)
 * also lists, per surface drawn into, the batches, triangles and screen area
 * (each batch's vertex box clipped to its scissor, summed). A level load
 * gives the GPU seconds of work between two presents (2026-10-13). */
#define HEAVY_SURF 12
static struct {
    uint32_t off, w, h, batches, tris, maxn;
    double area;
} s_heavy[HEAVY_SURF];
static uint32_t s_heavy_n, s_heavy_batches, s_heavy_other;

static void heavy_note(const Nv2aVshOutput *verts, uint32_t n, uint32_t tris)
{
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    double a;
    uint32_t i;

    s_heavy_batches++;
    {
        /* RECOMP_HEAVY_TRACE=1: frame-trace batches 3000-3039 of a heavy
         * frame (the rest of it would be tens of thousands of lines). */
        static int on = -1;
        if (on < 0)
            on = getenv("RECOMP_HEAVY_TRACE") != NULL;
        if (on && s_heavy_batches == 3000 && g_pb_ftrace == 0) {
            fprintf(stderr, "[FTRACE] heavy frame: batches 3001-3040%c", 10);
            g_pb_ftrace = 2;
        } else if (on && s_heavy_batches == 3040 && g_pb_ftrace == 2) {
            fprintf(stderr, "[FTRACE] heavy frame: trace off%c", 10);
            g_pb_ftrace = 0;
        }
    }
    for (i = 0; i < n; i++) {
        float x = verts[i].pos[0], y = verts[i].pos[1];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x0 < (float)g_pb.clip_x) x0 = (float)g_pb.clip_x;
    if (y0 < (float)g_pb.clip_y) y0 = (float)g_pb.clip_y;
    if (x1 > (float)(g_pb.clip_x + g_pb.clip_w)) x1 = (float)(g_pb.clip_x + g_pb.clip_w);
    if (y1 > (float)(g_pb.clip_y + g_pb.clip_h)) y1 = (float)(g_pb.clip_y + g_pb.clip_h);
    a = (x1 > x0 && y1 > y0) ? (double)(x1 - x0) * (double)(y1 - y0) : 0.0;
    for (i = 0; i < s_heavy_n; i++)
        if (s_heavy[i].off == g_pb.color_offset && s_heavy[i].w == g_pb.clip_w
            && s_heavy[i].h == g_pb.clip_h)
            break;
    if (i == s_heavy_n) {
        if (s_heavy_n == HEAVY_SURF) {
            s_heavy_other++;
            return;
        }
        memset(&s_heavy[i], 0, sizeof s_heavy[i]);
        s_heavy[i].off = g_pb.color_offset;
        s_heavy[i].w = g_pb.clip_w;
        s_heavy[i].h = g_pb.clip_h;
        s_heavy_n++;
    }
    s_heavy[i].batches++;
    s_heavy[i].tris += tris;
    s_heavy[i].area += a;
    if (n > s_heavy[i].maxn)
        s_heavy[i].maxn = n;
}

static void heavy_report(void)
{
    uint32_t i;
    if (s_ct_on > 0 && s_heavy_batches >= 3000) {
        fprintf(stderr, "[GPU] d3d11: heavy frame before flip %u: %u batches%c",
                g_pb.flips, s_heavy_batches, 10);
        for (i = 0; i < s_heavy_n; i++)
            fprintf(stderr, "[GPU]   surf %08X clip %ux%u: %u batches, %u tris,"
                    " area %.0f px (%.1f screens of it), largest batch %u verts%c",
                    s_heavy[i].off, s_heavy[i].w, s_heavy[i].h, s_heavy[i].batches,
                    s_heavy[i].tris, s_heavy[i].area,
                    s_heavy[i].w && s_heavy[i].h
                        ? s_heavy[i].area / ((double)s_heavy[i].w * s_heavy[i].h) : 0.0,
                    s_heavy[i].maxn, 10);
        if (s_heavy_other)
            fprintf(stderr, "[GPU]   +%u batches on further surfaces%c",
                    s_heavy_other, 10);
    }
    s_heavy_n = s_heavy_batches = s_heavy_other = 0;
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

/* Rectangle x,y,w,h (guest pixels) of surface i (or of nothing, i < 0), read
 * back from the GPU and written as a 24-bit BMP, at the surface's scale: a
 * scaled frame dumps at the render resolution. What lies outside the
 * surface's texture comes out black. 1 when the file was written. */
static int dump_rect(const char *path, int i, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h)
{
    static const uint8_t black[4] = {0, 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *staging = NULL;
    uint32_t cw = 0, ch = 0, row_bytes, pad, filesz, r, c;
    uint8_t hdr[54];
    FILE *f;

    if (i >= 0 && s_surf[i].scaled) {
        uint32_t x1 = to_host(x + w, s_rsx), y1 = to_host(y + h, s_rsy);
        x = to_host(x, s_rsx);
        y = to_host(y, s_rsy);
        w = x1 - x;
        h = y1 - y;
    }
    if (!w || !h)
        return 0;
    if (i >= 0 && x < s_surf[i].tw && y < s_surf[i].th) {
        cw = s_surf[i].tw - x < w ? s_surf[i].tw - x : w;
        ch = s_surf[i].th - y < h ? s_surf[i].th - y : h;
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

    heavy_report();
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
            /* The shown rectangle at its surface's scale, at most the
             * swap chain's size. An unscaled surface shown while the frame
             * is scaled would sit small in the corner: said once. */
            D3D11_BOX box;
            float sx = surf_sx(s_shown), sy = surf_sy(s_shown);
            uint32_t bw = to_host(s_shown_x + s_shown_w, sx)
                        - to_host(s_shown_x, sx);
            uint32_t bh = to_host(s_shown_y + s_shown_h, sy)
                        - to_host(s_shown_y, sy);
            if (s_scale_on && !s_surf[s_shown].scaled) {
                static int said;
                if (!said++)
                    fprintf(stderr, "[GPU] d3d11: presenting unscaled"
                            " surface 0x%08X pitch %u (%ux%u) while the"
                            " render scale is on%c", s_surf[s_shown].addr,
                            s_surf[s_shown].pitch, s_shown_w, s_shown_h, 10);
            }
            box.left = to_host(s_shown_x, sx);
            box.top = to_host(s_shown_y, sy);
            box.front = 0;
            box.right = box.left + (bw < s_sc_w ? bw : s_sc_w);
            box.bottom = box.top + (bh < s_sc_h ? bh : s_sc_h);
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                    (ID3D11Resource *)back, 0, 0, 0, 0,
                    (ID3D11Resource *)s_surf[s_shown].tex, 0, &box);
            ID3D11Texture2D_Release(back);
        }
        /* No vsync: the title paces itself on the vblank the kernel
         * emulates, as it does with the software path. */
        uint64_t ct0 = ct_now();
        IDXGISwapChain1_Present(s_swap, 0, 0);
        ct_add(CT_PRESENT, ct0);
        s_stat.presents++;
    }
    s_shown_stale = 1;
}

/* Visibility reports (d3d_get_report, below): FIFO [head, tail) and counts. */
static uint32_t s_vis_head, s_vis_tail;
static uint32_t s_vis_reports, s_vis_dropped, s_vis_failed;

static void d3d_report(void)
{
    fprintf(stderr, "[GPU] d3d11: %u colour clears (%u without a surface),"
            " %u presents, %u surfaces made%c",
            s_stat.clears, s_stat.clears_skipped, s_stat.presents,
            s_stat.surfaces, 10);
    fprintf(stderr, "[GPU] d3d11: %u batches: %u drawn (%u textured, %u"
            " through the combiners, %u triangles); not drawn: %u program"
            " did not run, %u not screen-space, %u no surface; %u vertices"
            " needing a near clip: %u triangles clipped, %u wholly behind; %u"
            " triangles culled%c",
            s_stat.batches, s_stat.drawn, s_stat.textured, s_stat.combined,
            s_stat.tris, s_stat.skip_program,
            s_stat.skip_not_screen, s_stat.skip_surface,
            s_stat.verts_need_clip, s_stat.tris_clipped, s_stat.tris_behind,
            s_stat.tris_culled,
            10);
    fprintf(stderr, "[GPU] d3d11: textures: %u made, %u uploads, %u binds"
            " unchanged, %u binds of a format the sampler cannot read%c",
            s_stat.tex_made, s_stat.tex_uploads, s_stat.tex_reused,
            s_stat.tex_unreadable, 10);
    fprintf(stderr, "[GPU] d3d11: render to texture: %u binds of a GPU"
            " surface, %u of a surface in a reading not handled (guest"
            " memory used)%c", s_stat.rtt_binds, s_stat.rtt_unhandled, 10);
    fprintf(stderr, "[GPU] d3d11: depth/stencil: %u buffers made, %u clears,"
            " %u batches drawn with them, %u on a surface too big for them"
            " (drawn without); %u reads as a texture, %u in a reading not"
            " handled (guest memory used)%c", s_stat.zeta_made,
            s_stat.zeta_clears, s_stat.zeta_draws, s_stat.zeta_too_big,
            s_stat.zeta_reads, s_stat.zeta_unhandled, 10);
    fprintf(stderr, "[GPU] d3d11: %u texture binds read from guest memory"
            " inside a GPU surface or zeta buffer (stale); surface binds:"
            " %u of a surface not drawn for 2+ flips, %u with a twin at the"
            " same address%c", s_stat.stale_reads, s_stat.rtt_old,
            s_stat.rtt_twin, 10);
    fprintf(stderr, "[GPU] d3d11: surface table (%d slots): %u evictions"
            " (%d addresses), %u texture binds of an evicted surface%c",
            GPU_SURFACES, s_stat.evictions, s_nevicted,
            s_stat.evicted_reads, 10);
    fprintf(stderr, "[GPU] d3d11: rings: vertices %u wraps (%u MB), indices"
            " %u wraps (%u MB)%c", s_vb.wraps, s_vb.bytes >> 20,
            s_ib_ring.wraps, s_ib_ring.bytes >> 20, 10);
    fprintf(stderr, "[GPU] d3d11: pacing: %u waits for the GPU, %.0f ms%c",
            s_pace_waits, s_pace_wait_ms, 10);
    fprintf(stderr, "[GPU] d3d11: visibility reports: %u written, %u waiting,"
            " %u written as visible unmeasured (GPU too far behind), %u"
            " queries not made%c", s_vis_reports, s_vis_tail - s_vis_head,
            s_vis_dropped, s_vis_failed, 10);
    if (s_shadow)
        fprintf(stderr, "[GPU] d3d11 shadow: %u frames dumped beside the"
                " software ones (%u from a surface the GPU never made)%c",
                s_stat.shadow_dumps, s_stat.shadow_no_surface, 10);
    ct_report();
}

/* Visibility tests: NV2A counts the samples that pass the depth and stencil
 * tests while SET_ZPASS_PIXEL_COUNT_ENABLE is on, from CLEAR_REPORT_VALUE, and
 * GET_REPORT writes the count. D3D11's occlusion query counts the same thing.
 * One query per stretch of counting; a report sums the stretches since the
 * last clear and is written once the GPU has answered all of them, in order.
 * Until then the title's slot reads incomplete (D3D set it to 0xFFFFFFFF),
 * as it does on the console while its GPU catches up.
 *
 * Without this every report said 0 pixels: harmless while the reports went
 * astray (they landed in D3D's pushbuffer, fixed in xbox_ContiguousAllocEx),
 * but once they reached their slots Conker took its characters for hidden
 * and stopped drawing them (the throne, Berri; 2026-10-14).
 *
 * At a render scale a pixel the guest draws covers sx x sy host pixels, so
 * each stretch's count is divided by the largest such area drawn into while
 * it counted (vis_note_scale), and a count that rounds to nothing but saw
 * something is 1: what the title compares is guest pixels. */
#define VIS_STRETCHES 8               /* stretches summed into one report */
#define VIS_PENDING   1024            /* reports waiting for the GPU */
static ID3D11Query *s_vis_open;       /* counting now */
static float s_vis_open_area = 1.0f;  /* host pixels per guest pixel in it */
static ID3D11Query *s_vis_since[VIS_STRETCHES];
static float s_vis_since_area[VIS_STRETCHES];
static int s_vis_nsince, s_vis_overflow;
static struct {
    uint32_t at;
    uint64_t stamp;
    ID3D11Query *q[VIS_STRETCHES];
    float area[VIS_STRETCHES];
    int nq, overflow;
    LONGLONG since;                   /* QPC when queued, to flush once */
    int flushed;
} s_vis_pend[VIS_PENDING];

static void vis_begin(void)
{
    D3D11_QUERY_DESC qd;
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateQuery(s_dev, &qd, &s_vis_open))) {
        s_vis_open = NULL;
        s_vis_failed++;
        return;
    }
    s_vis_open_area = 1.0f;
    ID3D11DeviceContext_Begin(s_ctx, (ID3D11Asynchronous *)s_vis_open);
}

/* A batch is drawn into a surface of `area` host pixels per guest pixel. */
static void vis_note_scale(float area)
{
    if (s_vis_open && area > s_vis_open_area)
        s_vis_open_area = area;
}

static void vis_end(void)
{
    if (!s_vis_open)
        return;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_vis_open);
    if (s_vis_nsince < VIS_STRETCHES) {
        s_vis_since_area[s_vis_nsince] = s_vis_open_area;
        s_vis_since[s_vis_nsince++] = s_vis_open;
    } else {
        ID3D11Query_Release(s_vis_open);
        s_vis_overflow = 1;           /* reported as visible, see vis_write */
    }
    s_vis_open = NULL;
}

static void vis_clear(void)
{
    int i;
    for (i = 0; i < s_vis_nsince; i++)
        ID3D11Query_Release(s_vis_since[i]);
    s_vis_nsince = 0;
    s_vis_overflow = 0;
}

static void d3d_zpass(int what)
{
    if (s_shadow)
        return;
    if (what == 0) {                  /* clear: a count running now restarts */
        int was_open = s_vis_open != NULL;
        if (was_open) {
            ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)s_vis_open);
            ID3D11Query_Release(s_vis_open);
            s_vis_open = NULL;
        }
        vis_clear();
        if (was_open)
            vis_begin();
    } else if (what == 1) {
        if (!s_vis_open)
            vis_begin();
    } else {
        vis_end();
    }
}

static void vis_write(uint32_t at, uint64_t stamp, uint32_t count)
{
    volatile uint8_t *slot = (volatile uint8_t *)xbox_GetMemoryOffset() + at;
    memcpy((void *)slot, &stamp, 8);
    *(volatile uint32_t *)(slot + 8) = count;
    MemoryBarrier();                  /* the count before "done" */
    *(volatile uint32_t *)(slot + 12) = 0;
    s_vis_reports++;
}

static void d3d_get_report(uint32_t at, uint64_t stamp)
{
    int i, counting = s_vis_open != NULL;

    if (s_shadow)
        return;
    if (s_vis_tail - s_vis_head >= VIS_PENDING) {
        /* The GPU is far behind: say "visible" rather than lose the test. */
        vis_write(at, stamp, 0xFFFFu);
        s_vis_dropped++;
        return;
    }
    if (counting) {                   /* a report mid-count: split the stretch */
        vis_end();
    }
    {
        uint32_t k = s_vis_tail % VIS_PENDING;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        s_vis_pend[k].at = at;
        s_vis_pend[k].stamp = stamp;
        s_vis_pend[k].nq = s_vis_nsince;
        s_vis_pend[k].overflow = s_vis_overflow;
        for (i = 0; i < s_vis_nsince; i++) {
            ID3D11Query_AddRef(s_vis_since[i]);
            s_vis_pend[k].q[i] = s_vis_since[i];
            s_vis_pend[k].area[i] = s_vis_since_area[i];
        }
        s_vis_pend[k].since = t.QuadPart;
        s_vis_pend[k].flushed = 0;
        s_vis_tail++;
    }
    if (counting)
        vis_begin();
}

static void d3d_poll(void)
{
    static LONGLONG two_ms;
    if (!two_ms) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        two_ms = f.QuadPart / 500;
    }
    while (s_vis_head != s_vis_tail) {
        uint32_t k = s_vis_head % VIS_PENDING, count = 0;
        uint64_t sum = 0;
        double scaled = 0.0;
        int i, ready = 1;

        for (i = 0; i < s_vis_pend[k].nq; i++) {
            UINT64 n = 0;
            HRESULT hr = ID3D11DeviceContext_GetData(s_ctx,
                    (ID3D11Asynchronous *)s_vis_pend[k].q[i], &n, sizeof n,
                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) {
                ready = 0;
                break;
            }
            if (SUCCEEDED(hr)) {
                sum += n;
                scaled += (double)n / s_vis_pend[k].area[i];
            }
        }
        if (!ready) {
            /* Commands sit in the driver's buffer until something flushes
             * them; a title waiting on this result submits nothing more. */
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            if (!s_vis_pend[k].flushed && t.QuadPart - s_vis_pend[k].since > two_ms) {
                ID3D11DeviceContext_Flush(s_ctx);
                s_vis_pend[k].flushed = 1;
            }
            return;
        }
        if (s_scale_on && sum) {
            scaled += 0.5;
            sum = scaled < 1.0 ? 1 : (uint64_t)scaled;
        }
        count = s_vis_pend[k].overflow || sum > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                            : (uint32_t)sum;
        vis_write(s_vis_pend[k].at, s_vis_pend[k].stamp, count);
        for (i = 0; i < s_vis_pend[k].nq; i++)
            ID3D11Query_Release(s_vis_pend[k].q[i]);
        s_vis_head++;
    }
}

static const Nv2aPbBackend s_backend = {
    "d3d11", d3d_clear, d3d_draw, d3d_present, d3d_report,
    d3d_zpass, d3d_get_report, d3d_poll
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
    {
        /* Not in shadow mode: its dumps must match the software path's
         * 640x480 pixel for pixel. */
        unsigned w, h;
        if (!shadow && xbox_RenderRes(&w, &h) && (w != SC_W || h != SC_H)) {
            s_scale_on = 1;
            s_sc_w = w;
            s_sc_h = h;
            s_rsx = (float)w / (float)SC_W;
            s_rsy = (float)h / (float)SC_H;
            fprintf(stderr, "[GPU] d3d11: render resolution %ux%u (frame"
                    " scaled %.3g x %.3g)%c", w, h, s_rsx, s_rsy, 10);
        }
    }
    return &s_backend;
}

#else

const Nv2aPbBackend *nv2a_pb_backend_d3d11_open(int shadow)
{
    (void)shadow;
    return NULL;
}

#endif
