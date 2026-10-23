/**
 * Depth of field drawn by the host: the D3D11 back end's NV_HOST_FX_DOF
 * effect (nv2a_pb_state.h). A game whose own depth of field looks poor at
 * modern resolutions (Conker's: the frame shrunk into a 256x256 texture
 * and stretched back, so one texel covers ~7x4 pixels at 1080p) asks for
 * this one instead, in the pushbuffer, where its own would have drawn: the
 * frame and its depth buffer are then exactly what the title's pass saw,
 * and whatever the title draws after (subtitles, HUD) stays sharp.
 *
 * Four draws over the frame's rectangle:
 *
 *   copy      the rectangle into s_src (the composite reads it while it
 *             writes the frame)
 *   prep      at the working size (half the frame, at most 540 lines): the
 *             colour in linear light and each pixel's blur size (s_prep,
 *             RGBA16F), and its distance from the camera (s_dist, R32F),
 *             the nearest of the frame pixels it covers
 *   gather    at the working size: Dennis Gustafsson's single-pass
 *             depth of field ("Bokeh depth of field in a single pass",
 *             2018), written anew here. Samples on a golden-angle spiral out
 *             to the largest blur; a sample counts where its own blur
 *             reaches this pixel, so a blurred foreground spreads over what
 *             is behind it (soft outlines), and a sample behind this pixel
 *             counts at most twice this pixel's own blur, so a blurred
 *             background never covers a sharp foreground. A sample that
 *             does not count adds the average so far, which keeps the
 *             result a blend. Out: the blurred colour, and how much of it
 *             came from blurred things in front (s_blur)
 *   composite at the frame's size: the sharp pixel, the blurred one
 *             (stretched bilinear) by the pixel's own blur, worked out
 *             again from the full-size depth so in-focus edges stay crisp,
 *             or by the cover from the front, whichever is more
 *
 * Blur size (radius, guest pixels) from the depth d (0..1, as the depth
 * buffer holds it): none at the focus depth, growing away from it (size_of
 * in s_hlsl) -- style 0 (like the title's) with the square of the depth
 * distance over the range, the most blur at the range; style 1 (cinematic)
 * as a lens would, |1/z_focus - 1/z| with z the distance, which grows
 * without bound in front and up to 2.5 times the most behind, with bright
 * samples weighted up so highlights open into round bokeh.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include "dof_d3d11.h"
#include <d3dcompiler.h>

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"    float4 rect;   /* frame rectangle: x0, y0, w, h (host pixels) */\n"
"    float4 work;   /* working size: w, h, 1/w, 1/h */\n"
"    float4 dep;    /* focus depth, range (depth 0..1), zn, zf */\n"
"    float4 blur;   /* max blur (working px), style, working/frame, rad step */\n"
"};\n"
"Texture2D<float4> t_src  : register(t0);\n"
"Texture2D<float>  t_z    : register(t1);\n"
"Texture2D<float4> t_prep : register(t2);\n"
"Texture2D<float>  t_dist : register(t3);\n"
"Texture2D<float4> t_blur : register(t4);\n"
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt  : register(s1);\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"    V o; float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    o.uv = t; return o;\n"
"}\n"
/* The title's depth (Z24 / 0xFFFFFF, D3D's 0..1) to the distance. */
"float dist_of(float d) {\n"
"    float zn = dep.z, zf = dep.w;\n"
"    return zn / max(1.0 - d * (zf - zn) / zf, 1e-7);\n"
"}\n"
/* Blur radius in working pixels at depth d. Style 0: the square of the
 * depth's distance from the focus over the range, at most 1 (Conker's own
 * curve). Style 1: the circle of confusion of a lens focused there,
 * |1/z_focus - 1/z|, 1 at the range's far end, at most 2.5. */
"float size_of(float d) {\n"
"    float k;\n"
"    if (blur.y < 0.5) {\n"
"        float t = (d - dep.x) / dep.y;\n"
"        k = min(t * t, 1.0);\n"
"    } else {\n"
"        float zc = dist_of(dep.x), zb = dist_of(min(dep.x + dep.y, 0.9999));\n"
"        k = abs(1.0 / zc - 1.0 / dist_of(d))\n"
"            / max(abs(1.0 / zc - 1.0 / zb), 1e-9);\n"
"        k = min(k, 2.5);\n"
"    }\n"
"    return k * blur.x;\n"
"}\n"
"float3 lin(float3 c) { return pow(max(c, 0.0), 2.2); }\n"
"float3 gam(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }\n"
"struct P { float4 c : SV_Target0; float z : SV_Target1; };\n"
/* Working pixel: the colour (bilinear: the average of the ~2x2 frame
 * pixels it covers), the nearest of their depths. */
"P ps_prep(V i) {\n"
"    P o;\n"
"    float2 f = i.uv * rect.zw;\n"
"    float2 h = 0.5 * rect.zw * work.zw;\n"
"    int2 a = int2(f - h + 0.5), b = int2(f + h - 0.5);\n"
"    float d = t_z.Load(int3(rect.xy + a, 0));\n"
"    d = min(d, t_z.Load(int3(rect.xy + int2(b.x, a.y), 0)));\n"
"    d = min(d, t_z.Load(int3(rect.xy + int2(a.x, b.y), 0)));\n"
"    d = min(d, t_z.Load(int3(rect.xy + b, 0)));\n"
"    o.c = float4(lin(t_src.SampleLevel(s_lin, i.uv, 0).rgb), size_of(d));\n"
"    o.z = dist_of(d);\n"
"    return o;\n"
"}\n"
"float4 ps_gather(V i) : SV_Target {\n"
"    float4 c0 = t_prep.SampleLevel(s_pt, i.uv, 0);\n"
"    float cz = t_dist.SampleLevel(s_pt, i.uv, 0);\n"
"    float cs = c0.a;\n"
"    float3 acc = c0.rgb;\n"
"    float tot = 1.0, front = 0.0, n = 1.0;\n"
"    float r = blur.w, ang = 0.0;\n"
"    [loop] for (int k = 0; k < 192 && r < blur.x + 0.5; k++) {\n"
"        float2 uv = i.uv + float2(cos(ang), sin(ang)) * r * work.zw;\n"
"        float4 s = t_prep.SampleLevel(s_pt, uv, 0);\n"
"        float sz = t_dist.SampleLevel(s_pt, uv, 0);\n"
"        float ss = s.a;\n"
"        if (sz > cz) ss = min(ss, cs * 2.0);\n"
"        float m = smoothstep(r - 0.5, r + 0.5, ss);\n"
"        float w = 1.0;\n"
"        if (blur.y > 0.5) {\n"
"            float l = dot(s.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"            w = 1.0 + 6.0 * saturate(l - 0.6);\n"
"        }\n"
"        acc += (acc / tot) * (1.0 - m) + s.rgb * w * m;\n"
"        tot += (1.0 - m) + w * m;\n"
"        if (sz < cz) front += m;\n"
"        n += 1.0;\n"
"        r += blur.w / r;\n"
"        ang += 2.39996323;\n"
"    }\n"
"    return float4(acc / tot, saturate(front / n * 4.0));\n"
"}\n"
"float4 ps_comp(V i) : SV_Target {\n"
"    int2 px = int2(i.pos.xy);\n"
"    float4 sharp = t_src.Load(int3(px - int2(rect.xy), 0));\n"
"    float own = size_of(t_z.Load(int3(px, 0)));\n"
"    float4 b = t_blur.SampleLevel(s_lin, i.uv, 0);\n"
"    float k = max(smoothstep(0.15, 0.9, own), b.a);\n"
"    return float4(lerp(sharp.rgb, gam(b.rgb), k), sharp.a);\n"
"}\n";

typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView   *rtv;
    UINT w, h;
    DXGI_FORMAT fmt;
} Tex;

static ID3D11VertexShader *s_vs;
static ID3D11PixelShader  *s_prep_ps, *s_gather_ps, *s_comp_ps;
static ID3D11Buffer       *s_cb;
static ID3D11SamplerState *s_lin, *s_pt;
static ID3D11RasterizerState *s_rs;
static Tex s_src, s_prep, s_dist, s_blur;
static int s_failed;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "dof_d3d11", NULL,
                            NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] dof: shader %s failed: %s%c", entry,
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

/* Shaders, constants, samplers, rasteriser state: once. */
static int setup(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    ID3DBlob *code;
    HRESULT hr;

    if (s_comp_ps)
        return 1;
    if (s_failed)
        return 0;
    code = compile("vs", "vs_5_0");
    hr = code ? ID3D11Device_CreateVertexShader(dev,
            ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
            NULL, &s_vs) : E_FAIL;
    if (code)
        ID3D10Blob_Release(code);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_prep", &s_prep_ps);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_gather", &s_gather_ps);
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
        hr = ID3D11Device_CreateSamplerState(dev, &sd, &s_lin);
        if (SUCCEEDED(hr)) {
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            hr = ID3D11Device_CreateSamplerState(dev, &sd, &s_pt);
        }
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    }
    /* Last: its presence says everything above was made. */
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "ps_comp", &s_comp_ps);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] dof: could not be set up (hr %08lX); the"
                " depth of field is not drawn%c", (unsigned long)hr, 10);
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
    D3D11_RECT sc;

    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = (LONG)x;
    sc.top = (LONG)y;
    sc.right = (LONG)(x + w);
    sc.bottom = (LONG)(y + h);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
}

int dof_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
             ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
             ID3D11ShaderResourceView *depth, UINT x0, UINT y0, UINT w,
             UINT h, float guest_h, const DofParams *p)
{
    static ID3D11ShaderResourceView *const none[5];
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_BOX box;
    ID3D11RenderTargetView *mrt[2];
    ID3D11ShaderResourceView *srv[5];
    ID3D11SamplerState *samp[2];
    float k[16], ws, max_blur, step;
    UINT ww, wh;
    static int said;

    if (!tex || !rtv || !depth || !w || !h || guest_h <= 0.0f || !setup(dev))
        return 0;
    /* Half the frame, but no more than 540 lines: past that the blur only
     * costs more and looks the same. */
    ws = 540.0f / (float)h;
    if (ws > 0.5f)
        ws = 0.5f;
    ww = (UINT)ceilf((float)w * ws);
    wh = (UINT)ceilf((float)h * ws);
    if (!tex_need(dev, &s_src, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0)
        || !tex_need(dev, &s_prep, ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1)
        || !tex_need(dev, &s_dist, ww, wh, DXGI_FORMAT_R32_FLOAT, 1)
        || !tex_need(dev, &s_blur, ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, 1)) {
        if (!said++)
            fprintf(stderr, "[GPU] dof: textures for %ux%u could not be"
                    " made; not drawn%c", w, h, 10);
        return 0;
    }

    /* Blur in working pixels; the spiral's step keeps the samples near
     * ~100 at any size (n ~ max^2 / (2 step)). */
    max_blur = p->max_blur * (float)wh / guest_h;
    if (max_blur < 0.5f)
        max_blur = 0.5f;
    step = max_blur * max_blur / 200.0f;
    if (step < 0.5f)
        step = 0.5f;
    k[0] = (float)x0; k[1] = (float)y0; k[2] = (float)w; k[3] = (float)h;
    k[4] = (float)ww; k[5] = (float)wh;
    k[6] = 1.0f / (float)ww; k[7] = 1.0f / (float)wh;
    k[8] = p->focus; k[9] = p->range > 1e-6f ? p->range : 1e-6f;
    k[10] = p->zn; k[11] = p->zf;
    k[12] = max_blur; k[13] = (float)p->style; k[14] = ws; k[15] = step;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return 0;
    memcpy(map.pData, k, sizeof k);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    box.left = x0;
    box.top = y0;
    box.right = x0 + w;
    box.bottom = y0 + h;
    box.front = 0;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(ctx,
            (ID3D11Resource *)s_src.tex, 0, 0, 0, 0, (ID3D11Resource *)tex,
            0, &box);

    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    samp[0] = s_lin;
    samp[1] = s_pt;
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);

    /* prep */
    mrt[0] = s_prep.rtv;
    mrt[1] = s_dist.rtv;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 2, mrt, NULL);
    srv[0] = s_src.srv;
    srv[1] = depth;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_prep_ps, NULL, 0);
    viewport(ctx, 0.0f, 0.0f, (float)ww, (float)wh);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* gather */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s_blur.rtv, NULL);
    srv[2] = s_prep.srv;
    srv[3] = s_dist.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_gather_ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* composite, into the frame */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    srv[4] = s_blur.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, srv);
    ID3D11DeviceContext_PSSetShader(ctx, s_comp_ps, NULL, 0);
    viewport(ctx, (float)x0, (float)y0, (float)w, (float)h);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    return 1;
}

#endif
