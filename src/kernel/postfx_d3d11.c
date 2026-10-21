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
 * The chain, in order (one full-screen draw per stage that is on; only the
 * last is in yet):
 *
 *   picture   gamma, contrast, saturation (RECOMP_GAMMA, RECOMP_CONTRAST,
 *             RECOMP_SATURATION; xbox_SetPicture), each 1.0 = unchanged
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
static volatile LONG s_gamma, s_contrast, s_saturation;
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

static float env_float(const char *name, float lo, float hi)
{
    const char *v = getenv(name);
    char *end;
    float f;

    if (!v || !*v)
        return 1.0f;
    f = strtof(v, &end);
    if (*end || !in_range(f, lo, hi)) {
        fprintf(stderr, "[GPU] postfx: %s=%s not a number from %g to %g;"
                " 1%c", name, v, lo, hi, 10);
        return 1.0f;
    }
    return f;
}

#define GAMMA_LO 0.1f
#define GAMMA_HI 4.0f
#define ADJ_LO   0.0f
#define ADJ_HI   4.0f

static BOOL CALLBACK params_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    const char *f = getenv("RECOMP_POSTFX_FORCE");
    float g = env_float("RECOMP_GAMMA", GAMMA_LO, GAMMA_HI);
    float c = env_float("RECOMP_CONTRAST", ADJ_LO, ADJ_HI);
    float s = env_float("RECOMP_SATURATION", ADJ_LO, ADJ_HI);

    (void)once; (void)p; (void)ctx;
    InterlockedExchange(&s_gamma, fbits(g));
    InterlockedExchange(&s_contrast, fbits(c));
    InterlockedExchange(&s_saturation, fbits(s));
    s_force = f && !strcmp(f, "1");
    if (g != 1.0f || c != 1.0f || s != 1.0f)
        fprintf(stderr, "[GPU] postfx: picture gamma %.2f contrast %.2f"
                " saturation %.2f%c", g, c, s, 10);
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

static int picture_on(void)
{
    return fval(&s_gamma) != 1.0f || fval(&s_contrast) != 1.0f
        || fval(&s_saturation) != 1.0f;
}

int postfx_active(void)
{
    params();
    return s_force || picture_on();
}

/* ---- the passes ----------------------------------------------------------- */

/* One full-screen triangle over the viewport. pk.xy: source texture units
 * per target pixel, pk.zw: the source rectangle's corner in texture units;
 * po.xy: the viewport's corner in target pixels, po.z nonzero: pixel for
 * pixel, read from pl.xy (the source corner in pixels) on; pa: gamma,
 * contrast, saturation. A value at 1 is skipped, so a neutral pass gives
 * back exactly what it read. */
static const char s_hlsl[] =
"cbuffer P : register(b0) { float4 pk; float4 po; float4 pl; float4 pa; };\n"
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
"}\n";

static ID3D11Device          *s_dev;      /* the objects below are its */
static ID3D11VertexShader    *s_vs;
static ID3D11PixelShader     *s_ps;
static ID3D11Buffer          *s_cb;
static ID3D11SamplerState    *s_samp;
static ID3D11RasterizerState *s_rs;
static int                    s_broken;   /* could not be made: copy */

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

/* The pipeline objects, made on first use. 0 (said once) when they cannot
 * be: the frame is then copied unfiltered. */
static int setup(ID3D11Device *dev)
{
    ID3DBlob *vs, *ps;
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
    ps = compile("pps", "ps_4_0");
    hr = vs && ps ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(dev,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(dev,
                ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps),
                NULL, &s_ps);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 64;
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

int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srv = NULL, *none = NULL;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    float k[16];

    if (!src || !dst || dw < 1.0f || dh < 1.0f || sw <= 0.0f || sh <= 0.0f
        || !setup(dev))
        return 0;
    ID3D11Texture2D_GetDesc(src, &td);
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)src, NULL, &srv)))
        return 0;
    k[0] = sw / (dw * (float)td.Width);
    k[1] = sh / (dh * (float)td.Height);
    k[2] = sx0 / (float)td.Width;
    k[3] = sy0 / (float)td.Height;
    k[4] = dx0;
    k[5] = dy0;
    k[6] = sw == dw && sh == dh ? 1.0f : 0.0f;
    k[7] = 0.0f;
    k[8] = sx0;
    k[9] = sy0;
    k[10] = k[11] = 0.0f;
    k[12] = fval(&s_gamma);
    k[13] = fval(&s_contrast);
    k[14] = fval(&s_saturation);
    k[15] = 0.0f;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ID3D11ShaderResourceView_Release(srv);
        return 0;
    }
    memcpy(m.pData, k, sizeof k);
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
    ID3D11DeviceContext_PSSetShader(ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11ShaderResourceView_Release(srv);
    return 1;
}

#else

int xbox_SetPicture(float gamma, float contrast, float saturation)
{
    (void)gamma; (void)contrast; (void)saturation;
    return 0;
}

#endif
