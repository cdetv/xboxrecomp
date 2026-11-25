/**
 * Ambient occlusion drawn by the host: the D3D11 back end's NV_HOST_FX_AO
 * effect (nv2a_pb_state.h). A game asks for it in the pushbuffer after its
 * opaque scenery and before anything blended, furry or see-through, so the
 * depth buffer then holds only solid surfaces and everything drawn later
 * (characters, grass, water, effects, the HUD) is left as it was.
 *
 * The occlusion shader is yacineolivier-maker's GTAO from ConkerLR-Recomp
 * (src/common/rendu_shaders.h, commit 2013160; MIT, Copyright (c) 2026
 * yacineolivier-maker and the ConkerLR-Recomp contributors, LICENSES/
 * ConkerLR-Recomp.txt), after Jimenez et al. 2016 with XeGTAO's "high"
 * preset: 3 slices, 3 steps each way, a 4x4 ordered dither for the slice
 * angle and step, the falloff over the last ~60 % of the radius, sky and
 * pixels whose radius covers less than one pixel left lit. Taken over to
 * D3D11 here; the working size, the stencil skip, the depth-aware
 * upsample, the brightness weight and the floor are ours.
 *
 * Four draws:
 *
 *   gtao     at the working size (half the frame above 1080 lines, else the
 *            frame's): the visibility (0 = closed in, 1 = open) and the
 *            pixel's distance (s_a, R16G16F), from the full-size depth
 *   blur x   5 taps across (weights 1/2, 1, 1, 1, 1/2), each dropped where
 *            its distance is more than ~2 % off: smooths the dither without
 *            crossing edges (s_b)
 *   blur y   the same down (s_a)
 *   apply    at the frame's size, into the frame: the blurred term from
 *            the 4 nearest working pixels, weighted by how close their
 *            distance is to this pixel's (no halos at edges at half size),
 *            faded out with distance (the game's fog wins there), weighted
 *            down on dark pixels if the brightness knee is set (off by
 *            default: in Conker's dark mansion it hid nearly all of it),
 *            never below the floor; pixels with stencil >= 8
 *            (a title's own tags, e.g. Conker's glow) and the sky are left
 *            alone. Reads a copy of the rectangle (s_src) and writes RGB
 *            only: alpha, depth and stencil stay the title's
 *
 * Test switches: RECOMP_AO_SPAN, RECOMP_AO_LUMA, RECOMP_AO_FLOOR (the
 * tuning below, also xbox_SetAoTuning); RECOMP_AO_VIEW=dist|normal: the frame
 * replaced by what the pass reads: the distance as grey ramps, one per
 * 1000 units (dist:<units> another length), or the surface direction as
 * colour (x red, y green up, z blue away: a wall facing the camera is dark
 * olive (0.5, 0.5, 0), a floor light green (0.5, 1, 0.5)).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ao_d3d11.h"

/* Tuning (xbox_SetAoTuning; RECOMP_AO_SPAN / _LUMA / _FLOOR set where it
 * starts): the most the radius may span on screen, as a share of the
 * picture's height (near things: else a radius in world units covers half
 * the screen; ConkerLR-Recomp's 0.12 kept near creases a few pixels
 * wide in Conker); the brightness below which it weakens, as a share of white
 * (0 = never: dark corners the level's painted light already darkened get
 * it too); the darkest it may make a pixel. On every platform, so a caller
 * links without the D3D11 back end. */
#define AO_SPAN_DEF  0.25f
#define AO_LUMA_DEF  0.0f
#define AO_FLOOR_DEF 0.3f               /* Conker's look B (user 2026-11-25) */
static float s_span = -1.0f, s_luma = -1.0f, s_floor = -1.0f;

static float env_or(const char *name, float lo, float hi, float def)
{
    const char *e = getenv(name);
    float v = e && *e ? (float)atof(e) : def;
    return v >= lo && v <= hi ? v : def;
}

static void tuning_read(void)
{
    if (s_span < 0.0f)
        s_span = env_or("RECOMP_AO_SPAN", 0.02f, 1.0f, AO_SPAN_DEF);
    if (s_luma < 0.0f)
        s_luma = env_or("RECOMP_AO_LUMA", 0.0f, 1.0f, AO_LUMA_DEF);
    if (s_floor < 0.0f)
        s_floor = env_or("RECOMP_AO_FLOOR", 0.0f, 1.0f, AO_FLOOR_DEF);
}

void xbox_SetAoTuning(float span, float luma, float floor_)
{
    tuning_read();
    if (span >= 0.02f && span <= 1.0f)
        s_span = span;
    if (luma >= 0.0f && luma <= 1.0f)
        s_luma = luma;
    if (floor_ >= 0.0f && floor_ <= 1.0f)
        s_floor = floor_;
}

void xbox_GetAoTuning(float *span, float *luma, float *floor_)
{
    tuning_read();
    if (span) *span = s_span;
    if (luma) *luma = s_luma;
    if (floor_) *floor_ = s_floor;
}

#if defined(_WIN32)

#include "hostfx_view.h"
#include "nv2a_pb_state.h"
#include <d3dcompiler.h>

static const char s_hlsl[] =
HOSTFX_VIEW_HLSL
"cbuffer C : register(b0) {\n"
"    float4 rect;  /* frame rectangle: x0, y0, w, h (host pixels) */\n"
"    float4 work;  /* working size w, h; frame px per working px; inverse */\n"
"    float4 cam;   /* near, far, 1 / x scale, 1 / y scale */\n"
"    float4 opt;   /* radius (world), most span (frame px), strength, floor */\n"
"    float4 fade;  /* fade start, end (distance); AO alone; stencil skip */\n"
"    float4 dbg;   /* RECOMP_AO_VIEW (1 distance, 2 normal), its distance\n"
"                     scale; brightness knee (0 = none) */\n"
"};\n"
"Texture2D<float>  t_z   : register(t0);\n"
"Texture2D<uint2>  t_s   : register(t1);\n"
"Texture2D<float2> t_ao  : register(t2);\n"
"Texture2D<float4> t_src : register(t3);\n"
"float3 view_at(float2 px) {\n"
"    int2 p = clamp(int2(px), int2(rect.xy), int2(rect.xy + rect.zw) - 1);\n"
"    return hfx_view(p + 0.5, t_z.Load(int3(p, 0)), rect, cam.zw, cam.x,\n"
"                    cam.y);\n"
"}\n"
/* The normal from the nearer neighbour on each side: no crease at depth
 * edges. On the rectangle's outermost pixels the inner one (the outer
 * would be the pixel itself: no direction, a dark speck). */
"float3 normal_at(float2 px, float3 c) {\n"
"    float2 lo = rect.xy + 0.5, hi = rect.xy + rect.zw - 0.5;\n"
"    float3 l = view_at(px - float2(1, 0)), r = view_at(px + float2(1, 0));\n"
"    float3 u = view_at(px - float2(0, 1)), d = view_at(px + float2(0, 1));\n"
"    bool use_r = px.x - 1.0 < lo.x\n"
"                 || (px.x + 1.0 <= hi.x && abs(r.z - c.z) < abs(c.z - l.z));\n"
"    bool use_d = px.y - 1.0 < lo.y\n"
"                 || (px.y + 1.0 <= hi.y && abs(d.z - c.z) < abs(c.z - u.z));\n"
"    float3 dx = use_r ? r - c : c - l;\n"
"    float3 dy = use_d ? d - c : c - u;\n"
"    return normalize(cross(dx, dy));\n"
"}\n"
"float on_rect(float2 p) {\n"
"    return all(p >= rect.xy) && all(p < rect.xy + rect.zw) ? 1.0 : 0.0;\n"
"}\n"
"static const float k_dither[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1,\n"
"                                    9, 15, 7, 13, 5 };\n"
"static const float k_half_pi = 1.5707963;\n"
"float2 ps_gtao(float4 pos : SV_Position) : SV_Target {\n"
"    float2 px = rect.xy + floor(pos.xy * work.z) + 0.5;\n"
"    float3 c = view_at(px);\n"
"    if (!(c.z > 0.0) || c.z > fade.y) return float2(1, min(c.z, 60000.0));\n"
"    float2 rad = opt.x * 0.5 * rect.zw / (cam.zw * c.z);\n"
"    float span = max(rad.x, rad.y);\n"
"    if (span < 1.0) return float2(1, c.z);\n"
"    float3 n = normal_at(px, c);\n"
"    float3 v = normalize(-c);\n"
"    float k = min(1.0, opt.y / span);\n"
"    float R = opt.x * k;\n"
"    int2 cell = int2(pos.xy) & 3;\n"
"    float slice_noise = k_dither[cell.y * 4 + cell.x] / 16.0;\n"
"    float step_noise = k_dither[cell.x * 4 + cell.y] / 16.0;\n"
"    float vis = 0.0;\n"
"    [unroll] for (int s = 0; s < 3; s++) {\n"
"        float phi = (s + slice_noise) * (k_half_pi * 2.0 / 3.0);\n"
"        float3 dir = float3(cos(phi), sin(phi), 0);\n"
"        float2 omega = float2(dir.x, -dir.y) * rad * k;\n"
"        float3 ortho = dir - dot(dir, v) * v;\n"
"        float3 axis = normalize(cross(ortho, v));\n"
"        float3 pn = n - axis * dot(n, axis);\n"
"        float pn_len = max(length(pn), 1e-4);\n"
"        float cos_n = saturate(dot(pn, v) / pn_len);\n"
"        float an = sign(dot(ortho, pn)) * acos(cos_n);\n"
"        float2 low = float2(cos(an + k_half_pi), cos(an - k_half_pi));\n"
"        float2 hc = low;\n"
"        [unroll] for (int j = 0; j < 3; j++) {\n"
"            float f = (j + step_noise) / 3.0;\n"
"            float2 off = omega * (f * f + 1.3 / max(length(omega), 1e-3));\n"
"            float3 d0 = view_at(px + off) - c;\n"
"            float3 d1 = view_at(px - off) - c;\n"
"            float2 len = max(float2(length(d0), length(d1)), 1e-4);\n"
/* Ours: a sample off the picture (clamped to its edge) does not count. */
"            float2 w = saturate((R - len) / (R * 0.615))\n"
"                       * float2(on_rect(px + off), on_rect(px - off));\n"
"            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;\n"
"            hc = max(hc, lerp(low, sc, w));\n"
"        }\n"
"        float h0 = an + clamp(-acos(hc.y) - an, -k_half_pi, k_half_pi);\n"
"        float h1 = an + clamp(acos(hc.x) - an, -k_half_pi, k_half_pi);\n"
"        float sin_n = sin(an);\n"
"        float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - an)) / 4.0;\n"
"        float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - an)) / 4.0;\n"
"        vis += pn_len * (arc0 + arc1);\n"
"    }\n"
"    return float2(pow(saturate(vis / 3.0), 2.2), c.z);\n"
"}\n"
"float2 blur_dir(float2 pos, int2 dir) {\n"
"    int2 p = int2(pos), hi = int2(work.xy) - 1;\n"
"    float z = t_ao.Load(int3(p, 0)).y;\n"
"    float sum = 0.0, n = 0.0;\n"
"    [unroll] for (int i = -2; i <= 2; i++) {\n"
"        float2 s = t_ao.Load(int3(clamp(p + i * dir, 0, hi), 0));\n"
"        float w = (abs(i) == 2 ? 0.5 : 1.0)\n"
"                  * saturate(2.0 - abs(s.y - z) / max(z * 0.02, 1e-3));\n"
"        sum += s.x * w;\n"
"        n += w;\n"
"    }\n"
"    return float2(n > 0.0 ? sum / n : 1.0, z);\n"
"}\n"
"float2 ps_blur_x(float4 pos : SV_Position) : SV_Target {\n"
"    return blur_dir(pos.xy, int2(1, 0));\n"
"}\n"
"float2 ps_blur_y(float4 pos : SV_Position) : SV_Target {\n"
"    return blur_dir(pos.xy, int2(0, 1));\n"
"}\n"
"float4 ps_apply(float4 pos : SV_Position) : SV_Target {\n"
"    int2 fp = int2(pos.xy);\n"
"    if (dbg.x > 0.5) {\n"
"        float3 vc = view_at(pos.xy);\n"
"        if (dbg.x < 1.5) return float4(frac(vc.z / dbg.y).xxx, 1);\n"
"        return float4(normal_at(pos.xy, vc) * 0.5 + 0.5, 1);\n"
"    }\n"
"    float4 c = fade.z > 0.5 ? float4(1, 1, 1, 1)\n"
"                            : t_src.Load(int3(fp - int2(rect.xy), 0));\n"
"    if (fade.w > 0.5 && t_s.Load(int3(fp, 0)).y >= 8) return c;\n"
"    float z = hfx_dist(t_z.Load(int3(fp, 0)), cam.x, cam.y);\n"
"    if (!(z < fade.y)) return c;\n"
"    float2 wc = (pos.xy - rect.xy) * work.w - 0.5;\n"
"    float2 b = floor(wc), f = wc - b;\n"
"    int2 hi = int2(work.xy) - 1;\n"
"    float sum = 0.0, n = 0.0, best = 1.0, bd = 1e30;\n"
"    [unroll] for (int q = 0; q < 4; q++) {\n"
"        int2 o = int2(q & 1, q >> 1);\n"
"        float2 s = t_ao.Load(int3(clamp(int2(b) + o, 0, hi), 0));\n"
"        float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);\n"
"        float dz = abs(s.y - z);\n"
"        float w = wb * saturate(2.0 - dz / max(z * 0.02, 1e-3));\n"
"        sum += s.x * w;\n"
"        n += w;\n"
"        if (dz < bd) { bd = dz; best = s.x; }\n"
"    }\n"
"    float a = n > 1e-4 ? sum / n : best;\n"
"    float amt = opt.z * (1.0 - saturate((z - fade.x)\n"
"                                        / max(fade.y - fade.x, 1.0)));\n"
"    if (fade.z < 0.5 && dbg.z > 0.0)\n"
"        amt *= saturate(dot(c.rgb, float3(0.299, 0.587, 0.114)) / dbg.z);\n"
"    float m = max(lerp(1.0, a, amt), opt.w);\n"
"    return float4(c.rgb * m, c.a);\n"
"}\n";

typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView   *rtv;
    UINT w, h;
    DXGI_FORMAT fmt;
} Tex;

static ID3D11VertexShader *s_vs;
static ID3D11PixelShader  *s_gtao_ps, *s_blurx_ps, *s_blury_ps, *s_apply_ps;
static ID3D11Buffer       *s_cb;
static ID3D11RasterizerState *s_rs;
static ID3D11BlendState   *s_bs;        /* no blending, RGB only */
static Tex s_src, s_a, s_b;
static int s_failed;
static float s_view = -1.0f, s_view_scale = 1000.0f;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "ao_d3d11", NULL,
                            NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] ao: shader %s failed: %s%c", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?",
                10);
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static HRESULT make_ps(ID3D11Device *dev, const char *entry,
                       ID3D11PixelShader **ps)
{
    ID3DBlob *code = compile(entry, "ps_5_0");
    HRESULT hr;

    if (!code)
        return E_FAIL;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, ps);
    ID3D10Blob_Release(code);
    return hr;
}

/* Shaders, constants, rasteriser and blend state: once. */
static int setup(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    ID3DBlob *code;
    HRESULT hr;

    if (s_apply_ps)
        return 1;
    if (s_failed)
        return 0;
    code = compile("hfx_vs", "vs_5_0");
    hr = code ? ID3D11Device_CreateVertexShader(dev,
            ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
            NULL, &s_vs) : E_FAIL;
    if (code)
        ID3D10Blob_Release(code);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_gtao", &s_gtao_ps);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_blur_x", &s_blurx_ps);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_blur_y", &s_blury_ps);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 96;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    }
    if (SUCCEEDED(hr)) {
        memset(&bl, 0, sizeof bl);
        bl.RenderTarget[0].BlendEnable = FALSE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
                | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        hr = ID3D11Device_CreateBlendState(dev, &bl, &s_bs);
    }
    /* Last: its presence says everything above was made. */
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_apply", &s_apply_ps);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] ao: could not be set up (hr %08lX); ambient"
                " occlusion is not drawn%c", (unsigned long)hr, 10);
        s_failed = 1;
        return 0;
    }
    return 1;
}

static void tex_free(Tex *t)
{
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
}

/* t at w x h in fmt, made again when either changed. rt: also a target. */
static int tex_need(ID3D11Device *dev, Tex *t, UINT w, UINT h,
                    DXGI_FORMAT fmt, int rt)
{
    D3D11_TEXTURE2D_DESC d;
    HRESULT hr;

    if (t->tex && t->w == w && t->h == h && t->fmt == fmt)
        return 1;
    tex_free(t);
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE
                  | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &t->tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)t->tex, NULL, &t->srv);
    if (SUCCEEDED(hr) && rt)
        hr = ID3D11Device_CreateRenderTargetView(dev,
                (ID3D11Resource *)t->tex, NULL, &t->rtv);
    if (FAILED(hr)) {
        tex_free(t);
        return 0;
    }
    t->w = w;
    t->h = h;
    t->fmt = fmt;
    return 1;
}

static void viewport(ID3D11DeviceContext *ctx, float x, float y, float w,
                     float h)
{
    D3D11_VIEWPORT vp;

    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}


static float ao_view(void)
{
    if (s_view < 0.0f) {
        const char *e = getenv("RECOMP_AO_VIEW");
        s_view = 0.0f;
        if (e && !strncmp(e, "dist", 4)) {
            s_view = 1.0f;
            if (e[4] == ':' && atof(e + 5) > 0.0)
                s_view_scale = (float)atof(e + 5);
        } else if (e && !strcmp(e, "normal")) {
            s_view = 2.0f;
        }
    }
    return s_view;
}

int ao_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
            ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
            ID3D11ShaderResourceView *depth, ID3D11ShaderResourceView *stencil,
            UINT x0, UINT y0, UINT w, UINT h, const AoParams *p)
{
    static ID3D11ShaderResourceView *const none[4] = { NULL, NULL, NULL,
                                                        NULL };
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_TEXTURE2D_DESC fd;
    D3D11_BOX box;
    ID3D11ShaderResourceView *srv[4];
    float k[24], ws;
    UINT ww, wh;
    int alone = (p->flags & NV_HOST_AO_ALONE) != 0;
    static int said;

    if (!tex || !rtv || !depth || !stencil || !w || !h || p->zn <= 0.0f
        || p->zf <= p->zn || p->sx == 0.0f || p->sy == 0.0f
        || p->radius <= 0.0f || !setup(dev))
        return 0;
    /* Half size above 1080 lines: four times fewer of the dear pixels; the
     * upsample keeps edges. */
    ws = h > 1080 && !(p->flags & NV_HOST_AO_FULL) ? 0.5f : 1.0f;
    ww = (UINT)ceilf((float)w * ws);
    wh = (UINT)ceilf((float)h * ws);
    ID3D11Texture2D_GetDesc(tex, &fd);
    if ((!alone && !tex_need(dev, &s_src, w, h, fd.Format, 0))
        || !tex_need(dev, &s_a, ww, wh, DXGI_FORMAT_R16G16_FLOAT, 1)
        || !tex_need(dev, &s_b, ww, wh, DXGI_FORMAT_R16G16_FLOAT, 1)) {
        if (!said++)
            fprintf(stderr, "[GPU] ao: textures for %ux%u (format %d) could"
                    " not be made; not drawn%c", w, h, (int)fd.Format, 10);
        return 0;
    }

    k[0] = (float)x0; k[1] = (float)y0; k[2] = (float)w; k[3] = (float)h;
    k[4] = (float)ww; k[5] = (float)wh; k[6] = 1.0f / ws; k[7] = ws;
    k[8] = p->zn; k[9] = p->zf;
    k[10] = 1.0f / fabsf(p->sx); k[11] = 1.0f / fabsf(p->sy);
    tuning_read();
    k[12] = p->radius; k[13] = s_span * (float)h;
    k[14] = p->strength < 0.0f ? 0.0f : p->strength > 1.0f ? 1.0f
                                                         : p->strength;
    k[15] = s_floor;
    k[17] = p->fade_end > p->zn ? p->fade_end : p->zf;
    k[16] = 0.3f * k[17];
    k[18] = alone ? 1.0f : 0.0f;
    k[19] = (p->flags & NV_HOST_AO_STENCIL) ? 1.0f : 0.0f;
    k[20] = ao_view();
    k[21] = s_view_scale;
    k[22] = s_luma;
    k[23] = 0.0f;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return 0;
    memcpy(map.pData, k, sizeof k);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    if (!alone) {
        box.left = x0;
        box.top = y0;
        box.right = x0 + w;
        box.bottom = y0 + h;
        box.front = 0;
        box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(ctx,
                (ID3D11Resource *)s_src.tex, 0, 0, 0, 0,
                (ID3D11Resource *)tex, 0, &box);
    }

    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);

    /* gtao: the depth buffer is unbound as a target here (no DSV) */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_a.rtv, NULL);
    srv[0] = depth;
    srv[1] = stencil;
    srv[2] = NULL;
    srv[3] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_gtao_ps, NULL, 0);
    viewport(ctx, 0.0f, 0.0f, (float)ww, (float)wh);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* blur across: s_a -> s_b */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_b.rtv, NULL);
    srv[2] = s_a.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_blurx_ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* blur down: s_b -> s_a */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &none[0]);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_a.rtv, NULL);
    srv[2] = s_b.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 3, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_blury_ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* apply, into the frame (RGB only) */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &none[0]);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_bs, NULL, 0xFFFFFFFFu);
    srv[2] = s_a.srv;
    srv[3] = alone ? NULL : s_src.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_apply_ps, NULL, 0);
    viewport(ctx, (float)x0, (float)y0, (float)w, (float)h);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    return 1;
}

#endif
