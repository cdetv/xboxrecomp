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
 *   sharpen   AMD FidelityFX CAS, sharpen-only (RECOMP_SHARPEN, percent;
 *             xbox_SetSharpen), 0 = off
 *   picture   gamma, contrast, saturation (RECOMP_GAMMA, RECOMP_CONTRAST,
 *             RECOMP_SATURATION; xbox_SetPicture), each 1.0 = unchanged;
 *             also the stretch of a frame drawn at an older render scale
 *
 * Every stage but the last draws into an intermediate texture (s_mid,
 * 16-bit float, the source rectangle's size), which the next one reads;
 * the last draws into the target. Sharpening runs at the source's own
 * size, before any stretch. The picture stage is left out when it would
 * only copy pixel for pixel behind another stage.
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
static int s_force;
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

static BOOL CALLBACK params_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    const char *f = getenv("RECOMP_POSTFX_FORCE");
    float g = env_float("RECOMP_GAMMA", GAMMA_LO, GAMMA_HI, 1.0f);
    float c = env_float("RECOMP_CONTRAST", ADJ_LO, ADJ_HI, 1.0f);
    float s = env_float("RECOMP_SATURATION", ADJ_LO, ADJ_HI, 1.0f);
    float sh = env_float("RECOMP_SHARPEN", 0.0f, SHARP_HI, 0.0f);

    (void)once; (void)p; (void)ctx;
    InterlockedExchange(&s_gamma, fbits(g));
    InterlockedExchange(&s_contrast, fbits(c));
    InterlockedExchange(&s_saturation, fbits(s));
    InterlockedExchange(&s_sharpen, fbits(sh));
    s_force = f && !strcmp(f, "1");
    if (g != 1.0f || c != 1.0f || s != 1.0f)
        fprintf(stderr, "[GPU] postfx: picture gamma %.2f contrast %.2f"
                " saturation %.2f%c", g, c, s, 10);
    if (sh != 0.0f)
        fprintf(stderr, "[GPU] postfx: sharpen %.0f %%%c", sh, 10);
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

static int picture_on(void)
{
    return fval(&s_gamma) != 1.0f || fval(&s_contrast) != 1.0f
        || fval(&s_saturation) != 1.0f;
}

int postfx_active(void)
{
    params();
    return s_force || picture_on() || fval(&s_sharpen) != 0.0f;
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

static ID3D11Device          *s_dev;      /* the objects below are its */
static ID3D11VertexShader    *s_vs;
static ID3D11PixelShader     *s_ps;       /* picture */
static ID3D11PixelShader     *s_cas;      /* sharpen */
static ID3D11Buffer          *s_cb;
static ID3D11SamplerState    *s_samp;
static ID3D11RasterizerState *s_rs;
static int                    s_broken;   /* could not be made: copy */

/* Between two stages: made at the source rectangle's size, again when that
 * changes. */
static ID3D11Texture2D          *s_mid;
static ID3D11RenderTargetView   *s_mid_rtv;
static ID3D11ShaderResourceView *s_mid_srv;
static UINT                      s_mid_w, s_mid_h;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "postfx_d3d11", NULL,
                            NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
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

static HRESULT make_ps(ID3D11Device *dev, const char *entry,
                       ID3D11PixelShader **ps)
{
    ID3DBlob *code = compile(entry, "ps_4_0");
    HRESULT hr;

    if (!code)
        return E_FAIL;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, ps);
    ID3D10Blob_Release(code);
    return hr;
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
    vs = compile("pvs", "vs_4_0");
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

static void mid_free(void)
{
    if (s_mid_srv) ID3D11ShaderResourceView_Release(s_mid_srv);
    if (s_mid_rtv) ID3D11RenderTargetView_Release(s_mid_rtv);
    if (s_mid) ID3D11Texture2D_Release(s_mid);
    s_mid_srv = NULL;
    s_mid_rtv = NULL;
    s_mid = NULL;
    s_mid_w = s_mid_h = 0;
}

/* The intermediate texture at w x h. 0 (said once) when it cannot be made. */
static int mid_fit(ID3D11Device *dev, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (s_mid && s_mid_w == w && s_mid_h == h)
        return 1;
    mid_free();
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s_mid);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s_mid,
                                                 NULL, &s_mid_rtv);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)s_mid, NULL, &s_mid_srv);
    if (FAILED(hr)) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GPU] postfx: no %ux%u intermediate texture (hr"
                    " 0x%08lX); sharpening skipped%c", w, h,
                    (unsigned long)hr, 10);
        mid_free();
        return 0;
    }
    s_mid_w = w;
    s_mid_h = h;
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

int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11ShaderResourceView *srv = NULL;
    float k[20], sharpen = fval(&s_sharpen);
    float tw, th;
    int same = sw == dw && sh == dh;
    int pic = sharpen == 0.0f || !same || picture_on();
    int ok = 1;

    if (!src || !dst || dw < 1.0f || dh < 1.0f || sw <= 0.0f || sh <= 0.0f
        || !setup(dev))
        return 0;
    ID3D11Texture2D_GetDesc(src, &td);
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)src, NULL, &srv)))
        return 0;
    tw = (float)td.Width;
    th = (float)td.Height;
    memset(k, 0, sizeof k);

    if (sharpen != 0.0f) {
        UINT mw = (UINT)(sw + 0.5f), mh = (UINT)(sh + 0.5f);
        /* Into the intermediate texture when the picture stage follows,
         * else straight into the target. */
        if (!pic || mid_fit(dev, mw, mh)) {
            k[4] = pic ? 0.0f : dx0;
            k[5] = pic ? 0.0f : dy0;
            k[8] = sx0;
            k[9] = sy0;
            k[10] = sx0 + sw;
            k[11] = sy0 + sh;
            /* 100 % = CasSetup's sharpness 1 (-1/5); AMD's default,
             * sharpness 0 (-1/8), is 62.5 %. */
            k[16] = -0.2f * sharpen / 100.0f;
            if (pic) {
                ok = run_pass(ctx, s_cas, srv, s_mid_rtv, 0.0f, 0.0f,
                              (float)mw, (float)mh, k);
                ID3D11ShaderResourceView_Release(srv);
                srv = s_mid_srv;
                ID3D11ShaderResourceView_AddRef(srv);
                tw = (float)mw;
                th = (float)mh;
                sx0 = sy0 = 0.0f;
            } else {
                ok = run_pass(ctx, s_cas, srv, dst, dx0, dy0, dw, dh, k);
            }
        } else {
            pic = 1;
        }
    }
    if (ok && pic) {
        k[0] = sw / (dw * tw);
        k[1] = sh / (dh * th);
        k[2] = sx0 / tw;
        k[3] = sy0 / th;
        k[4] = dx0;
        k[5] = dy0;
        k[6] = same ? 1.0f : 0.0f;
        k[7] = 0.0f;
        k[8] = sx0;
        k[9] = sy0;
        k[10] = sx0 + sw;
        k[11] = sy0 + sh;
        k[12] = fval(&s_gamma);
        k[13] = fval(&s_contrast);
        k[14] = fval(&s_saturation);
        k[15] = 0.0f;
        ok = run_pass(ctx, s_ps, srv, dst, dx0, dy0, dw, dh, k);
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

#endif
