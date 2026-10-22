/**
 * The filter chain of the D3D11 back end: what happens to the shown frame on
 * its way to the back buffer, before a host overlay draws on top.
 *
 * shown_to_back (nv2a_draw_d3d11.c) asks postfx_active once per present,
 * held presents too (a paused game, an open menu), so a change shows at
 * once. Every filter off: it copies the frame exactly as before and nothing
 * here runs. Any filter on: postfx_draw draws the frame through the chain
 * instead, pixel for pixel, or stretched bilinearly when the frame was drawn
 * at an older render scale.
 *
 * The chain, in order (one full-screen draw per stage that is on):
 *
 *   aa        anti-aliasing: NVIDIA FXAA 3.11 (RECOMP_AA=off|fxaa;
 *             xbox_SetAntiAlias), off by default; its shader is compiled
 *             the first time it is used
 *   sharpen   AMD FidelityFX CAS, sharpen-only (RECOMP_SHARPEN, percent;
 *             xbox_SetSharpen), 0 = off
 *   picture   gamma, contrast, saturation (RECOMP_GAMMA, RECOMP_CONTRAST,
 *             RECOMP_SATURATION; xbox_SetPicture), each 1.0 = unchanged;
 *             also the stretch of a frame drawn at an older render scale
 *
 * Every stage but the last draws into an intermediate texture (s_mid, two
 * taken in turn, 16-bit float, the source rectangle's size), which the
 * next one reads; the last draws into the target. Anti-aliasing and
 * sharpening run at the source's own size, before any stretch. The picture
 * stage is left out when it would only copy pixel for pixel behind another
 * stage. Anti-aliasing samples by texture coordinates, so a source
 * rectangle that is not its whole texture is first copied out alone
 * (s_in): what lies around it never bleeds in.
 *
 * Values come from the environment once, then only from the setters (no
 * getenv per frame). A menu changes them on the executor thread, between
 * presents; anything else changing them may tear for one frame, harmlessly.
 *
 * RECOMP_POSTFX_FORCE=1 (debug) runs the chain even when every filter is
 * neutral: with RECOMP_BB_DUMP its back buffers must match the copied ones.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include "postfx_d3d11.h"
#include <d3dcompiler.h>

/* ---- settings ------------------------------------------------------------- */

/* Each as float bits in a LONG, swapped whole. */
static volatile LONG s_gamma, s_contrast, s_saturation, s_sharpen;
static volatile LONG s_aa;            /* AA_*, a plain number */
static int s_force;

/* Anti-aliasing (xbox_SetAntiAlias; RECOMP_AA by these names). */
enum { AA_OFF, AA_FXAA, AA_MODES };
static const char *const s_aa_name[AA_MODES] = { "off", "fxaa" };
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static LONG fbits(float f)
{
    LONG l;
    memcpy(&l, &f, sizeof l);
    return l;
}

static float fval(volatile LONG *p)
{
    LONG l = *p;
    float f;
    memcpy(&f, &l, sizeof f);
    return f;
}

/* Wider than a menu offers: the toolkit only refuses nonsense. */
static int in_range(float v, float lo, float hi)
{
    return v >= lo && v <= hi;
}

/* def when unset or not taken. */
static float env_float(const char *name, float lo, float hi, float def)
{
    const char *v = getenv(name);
    char *end;
    float f;

    if (!v || !*v)
        return def;
    f = strtof(v, &end);
    if (*end || !in_range(f, lo, hi)) {
        fprintf(stderr, "[GPU] postfx: %s=%s not a number from %g to %g;"
                " %g%c", name, v, lo, hi, def, 10);
        return def;
    }
    return f;
}

#define GAMMA_LO 0.1f
#define GAMMA_HI 4.0f
#define ADJ_LO   0.0f
#define ADJ_HI   4.0f
#define SHARP_HI 100.0f

/* AA_OFF when unset or not one of s_aa_name. */
static int env_aa(void)
{
    const char *v = getenv("RECOMP_AA");
    int i;

    if (!v || !*v)
        return AA_OFF;
    for (i = 0; i < AA_MODES; i++)
        if (!_stricmp(v, s_aa_name[i]))
            return i;
    fprintf(stderr, "[GPU] postfx: RECOMP_AA=%s not off or fxaa; off%c", v,
            10);
    return AA_OFF;
}

static BOOL CALLBACK params_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    const char *f = getenv("RECOMP_POSTFX_FORCE");
    float g = env_float("RECOMP_GAMMA", GAMMA_LO, GAMMA_HI, 1.0f);
    float c = env_float("RECOMP_CONTRAST", ADJ_LO, ADJ_HI, 1.0f);
    float s = env_float("RECOMP_SATURATION", ADJ_LO, ADJ_HI, 1.0f);
    float sh = env_float("RECOMP_SHARPEN", 0.0f, SHARP_HI, 0.0f);
    int aa = env_aa();

    (void)once; (void)p; (void)ctx;
    InterlockedExchange(&s_gamma, fbits(g));
    InterlockedExchange(&s_contrast, fbits(c));
    InterlockedExchange(&s_saturation, fbits(s));
    InterlockedExchange(&s_sharpen, fbits(sh));
    InterlockedExchange(&s_aa, aa);
    s_force = f && !strcmp(f, "1");
    if (g != 1.0f || c != 1.0f || s != 1.0f)
        fprintf(stderr, "[GPU] postfx: picture gamma %.2f contrast %.2f"
                " saturation %.2f%c", g, c, s, 10);
    if (sh != 0.0f)
        fprintf(stderr, "[GPU] postfx: sharpen %.0f %%%c", sh, 10);
    if (aa != AA_OFF)
        fprintf(stderr, "[GPU] postfx: anti-aliasing %s%c", s_aa_name[aa],
                10);
    if (s_force)
        fprintf(stderr, "[GPU] postfx: RECOMP_POSTFX_FORCE: the chain runs"
                " even when every filter is off%c", 10);
    return TRUE;
}

static void params(void)
{
    InitOnceExecuteOnce(&s_once, params_init, NULL, NULL);
}

/* For a settings menu: gamma, contrast and saturation, each 1.0 =
 * unchanged; gamma above 1 brightens the mid-tones. A negative value keeps
 * that one as it is. Applies from the next present. 1 when every value
 * given was taken; one out of range is refused and the others still taken. */
int xbox_SetPicture(float gamma, float contrast, float saturation)
{
    int ok = 1;

    params();
    if (gamma >= 0.0f) {
        if (in_range(gamma, GAMMA_LO, GAMMA_HI))
            InterlockedExchange(&s_gamma, fbits(gamma));
        else
            ok = 0;
    }
    if (contrast >= 0.0f) {
        if (in_range(contrast, ADJ_LO, ADJ_HI))
            InterlockedExchange(&s_contrast, fbits(contrast));
        else
            ok = 0;
    }
    if (saturation >= 0.0f) {
        if (in_range(saturation, ADJ_LO, ADJ_HI))
            InterlockedExchange(&s_saturation, fbits(saturation));
        else
            ok = 0;
    }
    return ok;
}

/* For a settings menu: sharpening in percent, 0 = off, 100 = CAS at its
 * strongest. Applies from the next present. 1 when taken. */
int xbox_SetSharpen(float percent)
{
    params();
    if (!in_range(percent, 0.0f, SHARP_HI))
        return 0;
    InterlockedExchange(&s_sharpen, fbits(percent));
    return 1;
}

/* For a settings menu: anti-aliasing, 0 = off, 1 = FXAA. Applies from the
 * next present. 1 when taken. */
int xbox_SetAntiAlias(int mode)
{
    params();
    if (mode < 0 || mode >= AA_MODES)
        return 0;
    InterlockedExchange(&s_aa, mode);
    return 1;
}

static int picture_on(void)
{
    return fval(&s_gamma) != 1.0f || fval(&s_contrast) != 1.0f
        || fval(&s_saturation) != 1.0f;
}

int postfx_active(void)
{
    params();
    return s_force || picture_on() || fval(&s_sharpen) != 0.0f
        || s_aa != AA_OFF;
}

/* ---- the passes ----------------------------------------------------------- */

/* One full-screen triangle over the viewport. pk.xy: source texture units
 * per target pixel, pk.zw: the source rectangle's corner in texture units;
 * po.xy: the viewport's corner in target pixels, po.z nonzero: pixel for
 * pixel, read from pl.xy (the source corner in pixels) on; pl.zw: the
 * source rectangle's far corner in pixels; pa: gamma, contrast,
 * saturation; pc.x: CAS's peak (negative lobe), from the sharpen setting.
 *
 * pps, the picture stage: a value at 1 is skipped, so a neutral pass gives
 * back exactly what it read.
 *
 * cps, the sharpen stage, is AMD FidelityFX CAS 1.20190610 (ffx_cas.h,
 * CasFilter), sharpen-only, ported to plain HLSL: exact rcp and sqrt
 * (CAS_GO_SLOWER), the 5-tap min/max (no CAS_BETTER_DIAGONALS), the green
 * weight for all three channels (no CAS_SLOW). Pixel for pixel, taps
 * clamped to the source rectangle. The game's colours are gamma-encoded
 * and CAS wants linear ones: squared on load and square-rooted on output,
 * the gamma 2.0 approximation ffx_cas.h gives for UNORM input.
 *
 *   Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights
 *   reserved. MIT licence; the full text is in LICENSES/FidelityFX-CAS.txt
 *   (see NOTICE). */
static const char s_hlsl[] =
"cbuffer P : register(b0) {\n"
"  float4 pk; float4 po; float4 pl; float4 pa; float4 pc;\n"
"};\n"
"Texture2D pt : register(t0); SamplerState psm : register(s0);\n"
"float4 pvs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"}\n"
"float4 pps(float4 p : SV_Position) : SV_Target {\n"
"  float2 t = p.xy - po.xy;\n"
"  float4 c = po.z != 0.0 ? pt.Load(int3(t + pl.xy, 0))\n"
"                         : pt.Sample(psm, t * pk.xy + pk.zw);\n"
"  float3 v = c.rgb;\n"
"  [branch] if (pa.x != 1.0) v = pow(saturate(v), 1.0 / pa.x);\n"
"  [branch] if (pa.y != 1.0) v = saturate((v - 0.5) * pa.y + 0.5);\n"
"  [branch] if (pa.z != 1.0)\n"
"    v = saturate(lerp(dot(v, float3(0.2126, 0.7152, 0.0722)).xxx, v,\n"
"                      pa.z));\n"
"  return float4(v, c.a);\n"
"}\n"
"float3 cl(int2 q) {\n"
"  float3 c = pt.Load(int3(clamp(q, int2(pl.xy), int2(pl.zw) - 1), 0)).rgb;\n"
"  return c * c;\n"
"}\n"
"float4 cps(float4 p : SV_Position) : SV_Target {\n"
"  int2 q = int2(p.xy - po.xy + pl.xy);\n"
"  float3 b = cl(q + int2(0, -1)), d = cl(q + int2(-1, 0)), e = cl(q);\n"
"  float3 f = cl(q + int2(1, 0)), h = cl(q + int2(0, 1));\n"
"  float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);\n"
"  float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);\n"
"  float amp = sqrt(saturate(min(mn, 1.0 - mx) * rcp(mx)));\n"
"  float w = amp * pc.x;\n"
"  float3 o = saturate(((b + d + f + h) * w + e) * rcp(1.0 + 4.0 * w));\n"
"  return float4(sqrt(o), pt.Load(int3(q, 0)).a);\n"
"}\n";

/* fps, the anti-aliasing stage: NVIDIA FXAA 3.11 (third_party/fxaa/
 * Fxaa3_11.h, unchanged; its text is built into the library by
 * cmake/embed_text.cmake), the PC quality shader at preset 39 (the
 * highest), luma from green (the game's colours have none in alpha), and
 * the header's own default strengths: subpixel 0.75, edge threshold 0.166,
 * minimum 0.0833. Compiled after s_hlsl (constants and texture) and the
 * defines below. pk.xy: one over the texture's size. Alpha is the
 * source's, not FXAA's luma.
 *
 *   NVIDIA FXAA 3.11 by Timothy Lottes, (c) 2010, 2011 NVIDIA
 *   Corporation; its notice is in the header (see NOTICE). */
static const char s_fxaa_pre[] =
"#define FXAA_PC 1\n"
"#define FXAA_HLSL_5 1\n"
"#define FXAA_QUALITY__PRESET 39\n"
"#define FXAA_GREEN_AS_LUMA 1\n";
static const char s_fxaa_main[] =
"float4 fps(float4 p : SV_Position) : SV_Target {\n"
"  float2 q = p.xy - po.xy + pl.xy;\n"
"  FxaaTex t;\n"
"  t.smpl = psm;\n"
"  t.tex = pt;\n"
"  float4 c = FxaaPixelShader(q * pk.xy, 0.0, t, t, t, pk.xy, 0.0, 0.0,\n"
"                             0.0, 0.75, 0.166, 0.0833, 0.0, 0.0, 0.0,\n"
"                             0.0);\n"
"  return float4(c.rgb, pt.Load(int3(q, 0)).a);\n"
"}\n";
extern const unsigned char postfx_text_fxaa[];
extern const unsigned postfx_text_fxaa_len;

static ID3D11Device          *s_dev;      /* the objects below are its */
static ID3D11VertexShader    *s_vs;
static ID3D11PixelShader     *s_ps;       /* picture */
static ID3D11PixelShader     *s_cas;      /* sharpen */
static ID3D11PixelShader     *s_fxaa;     /* aa (FXAA), made on first use */
static ID3D11Buffer          *s_cb;
static ID3D11SamplerState    *s_samp;
static ID3D11RasterizerState *s_rs;
static int                    s_broken;   /* could not be made: copy */
static int                    s_aa_broken;  /* FXAA: none, said once */

/* A texture the chain draws into or copies to: made at a size, again when
 * that changes. */
typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;      /* NULL for s_in */
    ID3D11ShaderResourceView *srv;
    UINT                      w, h;
    DXGI_FORMAT               fmt;
} Surf;

static Surf s_mid[2];   /* between stages, taken in turn */
static Surf s_in;       /* the source rectangle alone, for anti-aliasing */

static ID3DBlob *compile(const char *src, size_t len, const char *entry,
                         const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, "postfx_d3d11", NULL, NULL, entry,
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                            &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: shader %s failed: %s%c", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?",
                10);
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static HRESULT make_ps_from(ID3D11Device *dev, const char *src, size_t len,
                            const char *entry, const char *target,
                            ID3D11PixelShader **ps)
{
    ID3DBlob *code = compile(src, len, entry, target);
    HRESULT hr;

    if (!code)
        return E_FAIL;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, ps);
    ID3D10Blob_Release(code);
    return hr;
}

static HRESULT make_ps(ID3D11Device *dev, const char *entry,
                       ID3D11PixelShader **ps)
{
    return make_ps_from(dev, s_hlsl, sizeof s_hlsl - 1, entry, "ps_4_0", ps);
}

/* The FXAA shader, the first time anti-aliasing is on (it takes a moment
 * to compile). 0 (said once) when it cannot be made: anti-aliasing is
 * then skipped. */
static int fxaa_setup(ID3D11Device *dev)
{
    size_t n1 = sizeof s_hlsl - 1, n2 = sizeof s_fxaa_pre - 1;
    size_t n3 = postfx_text_fxaa_len, n4 = sizeof s_fxaa_main - 1;
    char *src;
    DWORD t0;
    HRESULT hr;

    if (s_fxaa)
        return 1;
    if (s_aa_broken)
        return 0;
    src = (char *)malloc(n1 + n2 + n3 + n4);
    if (!src) {
        s_aa_broken = 1;
        return 0;
    }
    memcpy(src, s_hlsl, n1);
    memcpy(src + n1, s_fxaa_pre, n2);
    memcpy(src + n1 + n2, postfx_text_fxaa, n3);
    memcpy(src + n1 + n2 + n3, s_fxaa_main, n4);
    t0 = GetTickCount();
    hr = make_ps_from(dev, src, n1 + n2 + n3 + n4, "fps", "ps_5_0", &s_fxaa);
    free(src);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: FXAA could not be made (hr 0x%08lX);"
                " anti-aliasing skipped%c", (unsigned long)hr, 10);
        s_aa_broken = 1;
        return 0;
    }
    fprintf(stderr, "[GPU] postfx: FXAA ready (%lu ms)%c",
            (unsigned long)(GetTickCount() - t0), 10);
    return 1;
}

/* The pipeline objects, made on first use. 0 (said once) when they cannot
 * be: the frame is then copied unfiltered. */
static int setup(ID3D11Device *dev)
{
    ID3DBlob *vs;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (s_dev == dev && s_rs)
        return 1;
    if (s_broken)
        return 0;
    s_dev = dev;
    vs = compile(s_hlsl, sizeof s_hlsl - 1, "pvs", "vs_4_0");
    hr = vs ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(dev,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                NULL, &s_vs);
    if (vs) ID3D10Blob_Release(vs);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "pps", &s_ps);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "cps", &s_cas);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb);
    }
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(dev, &sd, &s_samp);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: setup failed (hr 0x%08lX); frames are"
                " shown unfiltered%c", (unsigned long)hr, 10);
        s_broken = 1;
        return 0;
    }
    fprintf(stderr, "[GPU] postfx: filter chain ready%c", 10);
    return 1;
}

static void surf_free(Surf *s)
{
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
}

/* s at w x h in format fmt, a render target too when rt. 0 (said once)
 * when it cannot be made. */
static int surf_fit(ID3D11Device *dev, Surf *s, UINT w, UINT h,
                    DXGI_FORMAT fmt, int rt)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s->tex && s->w == w && s->h == h && s->fmt == fmt)
        return 1;
    surf_free(s);
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE
                 | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->tex);
    if (SUCCEEDED(hr) && rt)
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s->tex,
                                                 NULL, &s->rtv);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)s->tex, NULL, &s->srv);
    if (FAILED(hr)) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GPU] postfx: no %ux%u texture (format %d, hr"
                    " 0x%08lX) for the chain; stages skipped%c", w, h,
                    (int)fmt, (unsigned long)hr, 10);
        surf_free(s);
        return 0;
    }
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    return 1;
}

/* One stage: pixel shader ps over the rectangle dx0,dy0 dw x dh of dst,
 * reading srv, with constants k (see s_hlsl). Sets every piece of pipeline
 * state it uses and leaves no target or view bound. */
static int run_pass(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps,
                    ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *dst,
                    float dx0, float dy0, float dw, float dh, const float *k)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, k, 20 * sizeof *k);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    vp.TopLeftX = dx0;
    vp.TopLeftY = dy0;
    vp.Width = dw;
    vp.Height = dh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = (LONG)dx0;
    sc.top = (LONG)dy0;
    sc.right = (LONG)(dx0 + dw + 0.5f);
    sc.bottom = (LONG)(dy0 + dh + 0.5f);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    return 1;
}

enum { ST_AA, ST_SHARPEN, ST_PICTURE };

int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11ShaderResourceView *srv = NULL, *in;
    float k[20], sharpen = fval(&s_sharpen);
    float tw, th, ix0, iy0;
    UINT mw = (UINT)(sw + 0.5f), mh = (UINT)(sh + 0.5f);
    int same = sw == dw && sh == dh;
    int aa = s_aa, pic, stage[3], n = 0, i, ok = 1;

    if (!src || !dst || dw < 1.0f || dh < 1.0f || sw <= 0.0f || sh <= 0.0f
        || !setup(dev))
        return 0;
    ID3D11Texture2D_GetDesc(src, &td);
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)src, NULL, &srv)))
        return 0;
    in = srv;
    tw = (float)td.Width;
    th = (float)td.Height;
    ix0 = sx0;
    iy0 = sy0;

    if (aa != AA_OFF && !fxaa_setup(dev))
        aa = AA_OFF;
    if (aa != AA_OFF && (sx0 != 0.0f || sy0 != 0.0f || (float)mw != tw
                         || (float)mh != th)) {
        /* The rectangle alone, so that sampling around its edge clamps
         * there instead of reading what lies next to it. */
        if (surf_fit(dev, &s_in, mw, mh, td.Format, 0)) {
            D3D11_BOX box;
            box.left = (UINT)sx0;
            box.top = (UINT)sy0;
            box.front = 0;
            box.right = box.left + mw;
            box.bottom = box.top + mh;
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(ctx,
                    (ID3D11Resource *)s_in.tex, 0, 0, 0, 0,
                    (ID3D11Resource *)src, 0, &box);
            in = s_in.srv;
            tw = (float)mw;
            th = (float)mh;
            ix0 = iy0 = 0.0f;
        } else {
            aa = AA_OFF;
        }
    }

    /* The stages that are on; every one but the last draws into s_mid. */
    pic = (aa == AA_OFF && sharpen == 0.0f) || !same || picture_on();
    if (aa != AA_OFF)
        stage[n++] = ST_AA;
    if (sharpen != 0.0f)
        stage[n++] = ST_SHARPEN;
    if (pic)
        stage[n++] = ST_PICTURE;
    if ((n > 1 && !surf_fit(dev, &s_mid[0], mw, mh,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, 1))
        || (n > 2 && !surf_fit(dev, &s_mid[1], mw, mh,
                               DXGI_FORMAT_R16G16B16A16_FLOAT, 1))) {
        /* Only the picture stage, from the source itself. */
        n = 0;
        stage[n++] = ST_PICTURE;
        in = srv;
        tw = (float)td.Width;
        th = (float)td.Height;
        ix0 = sx0;
        iy0 = sy0;
    }

    for (i = 0; ok && i < n; i++) {
        int last = i == n - 1;
        Surf *o = last ? NULL : &s_mid[i & 1];
        ID3D11RenderTargetView *rtv = last ? dst : o->rtv;
        float ox = last ? dx0 : 0.0f, oy = last ? dy0 : 0.0f;
        float ow = last ? dw : (float)mw, oh = last ? dh : (float)mh;

        memset(k, 0, sizeof k);
        k[4] = ox;
        k[5] = oy;
        k[8] = ix0;
        k[9] = iy0;
        k[10] = ix0 + sw;
        k[11] = iy0 + sh;
        switch (stage[i]) {
        case ST_AA:
            k[0] = 1.0f / tw;
            k[1] = 1.0f / th;
            ok = run_pass(ctx, s_fxaa, in, rtv, ox, oy, ow, oh, k);
            break;
        case ST_SHARPEN:
            /* 100 % = CasSetup's sharpness 1 (-1/5); AMD's default,
             * sharpness 0 (-1/8), is 62.5 %. */
            k[16] = -0.2f * sharpen / 100.0f;
            ok = run_pass(ctx, s_cas, in, rtv, ox, oy, ow, oh, k);
            break;
        default:
            k[0] = sw / (dw * tw);
            k[1] = sh / (dh * th);
            k[2] = ix0 / tw;
            k[3] = iy0 / th;
            k[6] = same ? 1.0f : 0.0f;
            k[12] = fval(&s_gamma);
            k[13] = fval(&s_contrast);
            k[14] = fval(&s_saturation);
            ok = run_pass(ctx, s_ps, in, rtv, ox, oy, ow, oh, k);
            break;
        }
        if (!last) {
            in = o->srv;
            tw = (float)mw;
            th = (float)mh;
            ix0 = iy0 = 0.0f;
        }
    }
    ID3D11ShaderResourceView_Release(srv);
    return ok;
}

#else

int xbox_SetPicture(float gamma, float contrast, float saturation)
{
    (void)gamma; (void)contrast; (void)saturation;
    return 0;
}

int xbox_SetSharpen(float percent)
{
    (void)percent;
    return 0;
}

int xbox_SetAntiAlias(int mode)
{
    (void)mode;
    return 0;
}

#endif
