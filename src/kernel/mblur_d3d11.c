/**
 * Camera motion blur drawn by the host: the D3D11 back end's
 * NV_HOST_FX_MBLUR effect (nv2a_pb_state.h). A game asks for it once its 3D
 * picture is drawn and before its HUD, with how its camera moved since the
 * last frame; each pixel's point is put back where the last frame's camera
 * saw it (its distance from the depth buffer, hostfx_view.h), and the pixel
 * is smeared along the path between the two places on screen. Only the
 * camera's motion: things that move by themselves are blurred as if they
 * stood still, which is why a game keeps the middle of the screen (where
 * its character usually is) sharp, and why it asks only during quick turns.
 *
 * One copy and one draw over the frame's rectangle:
 *
 *   copy      the rectangle into s_src (the draw reads it while it writes
 *             the frame)
 *   blur      the motion of the pixel's point (now minus then, host pixels)
 *             times the share smeared, capped at the longest smear, faded
 *             to nothing inside the sharp middle; then the average of 8
 *             samples (12 above 1440 lines) spread evenly along it, centred
 *             on the pixel, the whole row of them shifted by a per-pixel
 *             dither so the steps between samples read as fine noise, not as
 *             copies of the picture; averaged in linear light (power 2.2) so
 *             lights smear bright. Depth 1 (sky, nothing drawn there): a
 *             direction, moved by the turn only. RGB only: alpha, depth and
 *             stencil stay the title's
 *
 * NV_HOST_MBLUR_VIEW: the frame replaced by the motion as colour instead,
 * after the share, the cap and the sharp middle: red = to the right, green =
 * to the left, blue = up or down, full at the longest smear; black = none.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mblur_d3d11.h"

#if defined(_WIN32)

#include "hostfx_view.h"
#include "nv2a_pb_state.h"
#include <d3dcompiler.h>

static const char s_hlsl[] =
HOSTFX_VIEW_HLSL
"cbuffer C : register(b0) {\n"
"    float4 rect;  /* frame rectangle: x0, y0, w, h (host pixels) */\n"
"    float4 rot;   /* the camera's turn since the last frame (quaternion) */\n"
"    float4 move;  /* its move (xyz); the share of the motion smeared */\n"
"    float4 cam;   /* near, far, 1 / x scale, 1 / y scale */\n"
"    float4 opt;   /* x scale, y scale, longest smear (host px), sharp\n"
"                     middle (share of the half diagonal) */\n"
"    float4 misc;  /* samples, motion view */\n"
"};\n"
"Texture2D<float>  t_z   : register(t0);\n"
"Texture2D<float4> t_src : register(t1);\n"
"SamplerState s_lin : register(s0);\n"
"float3 qrot(float4 q, float3 v) {\n"
"    float3 t = 2.0 * cross(q.xyz, v);\n"
"    return v + q.w * t + cross(q.xyz, t);\n"
"}\n"
/* A point that ends up behind the last frame's camera is put at its near
 * plane: the direction stays about right and the cap takes the length. */
"float2 motion(float2 px) {\n"
"    float d = t_z.Load(int3(px, 0));\n"
"    float3 v = hfx_view(px, d, rect, cam.zw, cam.x, cam.y);\n"
"    float3 l = qrot(rot, v) + (d < 1.0 ? move.xyz : float3(0, 0, 0));\n"
"    l.z = max(l.z, cam.x);\n"
"    float2 ndc = l.xy * opt.xy / l.z;\n"
"    float2 past = rect.xy + (ndc * float2(0.5, -0.5) + 0.5) * rect.zw;\n"
"    float2 m = (px - past) * move.w;\n"
"    float len = length(m);\n"
"    if (len > opt.z) m *= opt.z / len;\n"
"    float r = length(px - rect.xy - rect.zw * 0.5) / length(rect.zw * 0.5);\n"
"    return opt.w > 0.0 ? m * smoothstep(opt.w, opt.w * 2.0, r) : m;\n"
"}\n"
"float4 ps_mblur(float4 pos : SV_Position) : SV_Target {\n"
"    float2 m = motion(pos.xy);\n"
"    if (misc.y > 0.5)\n"
"        return float4(saturate(float3(m.x, -m.x, abs(m.y))\n"
"                               / max(opt.z, 1e-3)), 1);\n"
"    float2 lp = pos.xy - rect.xy;\n"
"    float4 c = t_src.Load(int3(lp, 0));\n"
"    if (dot(m, m) < 0.25) return c;\n"
"    int n = (int)misc.x;\n"
"    float j = frac(52.9829189 * frac(dot(pos.xy,\n"
"                                         float2(0.06711056, 0.00583715))));\n"
"    float3 sum = float3(0, 0, 0);\n"
"    [loop] for (int i = 0; i < n; i++) {\n"
"        float2 sp = (lp + m * ((i + j) / n - 0.5)) / rect.zw;\n"
"        sum += pow(max(t_src.SampleLevel(s_lin, sp, 0).rgb, 0.0), 2.2);\n"
"    }\n"
"    return float4(pow(sum / n, 1.0 / 2.2), c.a);\n"
"}\n";

typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    UINT w, h;
    DXGI_FORMAT fmt;
} Tex;

static ID3D11VertexShader *s_vs;
static ID3D11PixelShader  *s_ps;
static ID3D11Buffer       *s_cb;
static ID3D11SamplerState *s_lin;
static ID3D11RasterizerState *s_rs;
static ID3D11BlendState   *s_bs;        /* no blending, RGB only */
static Tex s_src;
static int s_failed;

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "mblur_d3d11", NULL,
                            NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] mblur: shader %s failed: %s%c", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?",
                10);
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

/* Shaders, constants, sampler, rasteriser and blend state: once. */
static int setup(ID3D11Device *dev)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bl;
    ID3DBlob *code;
    HRESULT hr;

    if (s_ps)
        return 1;
    if (s_failed)
        return 0;
    code = compile("hfx_vs", "vs_5_0");
    hr = code ? ID3D11Device_CreateVertexShader(dev,
            ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
            NULL, &s_vs) : E_FAIL;
    if (code)
        ID3D10Blob_Release(code);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 96;
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
    if (SUCCEEDED(hr)) {
        code = compile("ps_mblur", "ps_5_0");
        hr = code ? ID3D11Device_CreatePixelShader(dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &s_ps) : E_FAIL;
        if (code)
            ID3D10Blob_Release(code);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] mblur: could not be set up (hr %08lX); motion"
                " blur is not drawn%c", (unsigned long)hr, 10);
        s_failed = 1;
        return 0;
    }
    return 1;
}

/* The copy of the rectangle at w x h in fmt, made again when either
 * changed. */
static int src_need(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    D3D11_TEXTURE2D_DESC d;
    HRESULT hr;

    if (s_src.tex && s_src.w == w && s_src.h == h && s_src.fmt == fmt)
        return 1;
    if (s_src.srv) ID3D11ShaderResourceView_Release(s_src.srv);
    if (s_src.tex) ID3D11Texture2D_Release(s_src.tex);
    memset(&s_src, 0, sizeof s_src);
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &s_src.tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)s_src.tex, NULL, &s_src.srv);
    if (FAILED(hr)) {
        if (s_src.tex) ID3D11Texture2D_Release(s_src.tex);
        memset(&s_src, 0, sizeof s_src);
        return 0;
    }
    s_src.w = w;
    s_src.h = h;
    s_src.fmt = fmt;
    return 1;
}

int mblur_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
               ID3D11Texture2D *tex, ID3D11RenderTargetView *rtv,
               ID3D11ShaderResourceView *depth,
               UINT x0, UINT y0, UINT w, UINT h, float guest_h,
               const MblurParams *p)
{
    static ID3D11ShaderResourceView *const none[2] = { NULL, NULL };
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_TEXTURE2D_DESC fd;
    D3D11_VIEWPORT vp;
    D3D11_BOX box;
    ID3D11ShaderResourceView *srv[2];
    float k[24], ql;
    int view = (p->flags & NV_HOST_MBLUR_VIEW) != 0;
    static int said;

    if (!tex || !rtv || !depth || !w || !h || !(guest_h > 0.0f)
        || p->zn <= 0.0f || p->zf <= p->zn || p->sx == 0.0f
        || p->sy == 0.0f || !(p->max_blur > 0.0f) || !setup(dev))
        return 0;
    if (!(p->amount > 0.0f) && !view)
        return 1;                       /* nothing moves: nothing to do */
    ql = sqrtf(p->q[0] * p->q[0] + p->q[1] * p->q[1] + p->q[2] * p->q[2]
               + p->q[3] * p->q[3]);
    if (!(ql > 0.5f && ql < 2.0f))
        return 0;
    ID3D11Texture2D_GetDesc(tex, &fd);
    if (!view && !src_need(dev, w, h, fd.Format)) {
        if (!said++)
            fprintf(stderr, "[GPU] mblur: a copy for %ux%u (format %d) could"
                    " not be made; not drawn%c", w, h, (int)fd.Format, 10);
        return 0;
    }

    k[0] = (float)x0; k[1] = (float)y0; k[2] = (float)w; k[3] = (float)h;
    k[4] = p->q[0] / ql; k[5] = p->q[1] / ql; k[6] = p->q[2] / ql;
    k[7] = p->q[3] / ql;
    k[8] = p->t[0]; k[9] = p->t[1]; k[10] = p->t[2];
    k[11] = p->amount > 0.0f ? p->amount : 0.0f;
    k[12] = p->zn; k[13] = p->zf;
    k[14] = 1.0f / p->sx; k[15] = 1.0f / p->sy;
    k[16] = p->sx; k[17] = p->sy;
    k[18] = p->max_blur * (float)h / guest_h;
    k[19] = p->centre < 0.0f ? 0.0f : p->centre > 1.0f ? 1.0f : p->centre;
    k[20] = h > 1440 ? 12.0f : 8.0f;
    k[21] = view ? 1.0f : 0.0f;
    k[22] = k[23] = 0.0f;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return 0;
    memcpy(map.pData, k, sizeof k);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    if (!view) {
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

    /* The depth buffer is read, so it is not bound as a target (no DSV). */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, s_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s_lin);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    srv[0] = depth;
    srv[1] = view ? NULL : s_src.srv;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srv);
    vp.TopLeftX = (float)x0;
    vp.TopLeftY = (float)y0;
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    return 1;
}

#else

typedef int mblur_d3d11_none;           /* no D3D11 here: nothing to draw */

#endif
