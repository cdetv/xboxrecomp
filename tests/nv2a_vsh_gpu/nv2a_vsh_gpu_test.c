/* Translated vertex programs (nv2a_vsh_hlsl.c) on a D3D11 device against
 * the CPU interpreter (nv2a_vsh_run): the Xbox D3D epilogue, then random
 * programs as in nv2a_vsh_lanes_test (every MAC and ILU op, random sources,
 * swizzles, negates, masks, outputs, relative constants after an ARL).
 *
 * The GPU's rsq, exp2, log2 and divide are not the CPU's to the bit, so
 * results are compared with nv2a_vsh_outputs_close. Every program must
 * translate and compile, and on WARP (the default: the same results on
 * every machine) every vertex must be close: 47,645 of 47,645 were, once the
 * zeroes stopped being literals and lit's pow of 0 was spelled out
 * (nv2a_vsh_hlsl.c). "hw" as the argument runs on the default hardware
 * adapter instead, where a driver's maths may differ a little: under 0.5%
 * of vertices may differ there (0 on the development PC's GPU, 2026-10-18).
 *
 * VSH_TEST_SHOW=<program> [VSH_TEST_VERTEX=<n>]: that program's HLSL and a
 * step-by-step CPU run of one of its vertices, for a difference. */
#include "nv2a_vsh_interp.h"
#include "nv2a_vsh_hlsl.h"
#include "nv2a_vsh_gpu.h"
#include "nv2a_attr.h"

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_rng = 12345u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    uint32_t k = rnd() % 16;
    if (k == 0) return 0.0f;
    if (k == 1) return (float)((int)(rnd() % 7) - 3);
    return ((float)(rnd() % 20001) - 10000.0f) / 1000.0f;
}

static void random_instruction(uint32_t w[4], int final)
{
    w[0] = 0;
    w[1] = rnd() & 0x0FFFFFFFu;
    w[1] = (w[1] & ~(0xFFu << 13)) | ((rnd() % 200u) << 13);
    w[2] = rnd();
    w[3] = rnd() & ~1u;
    w[3] = (w[3] & ~(0xFFu << 3)) | ((rnd() % 13u) << 3);
    if (rnd() % 8)
        w[3] |= 1u << 11;
    if ((w[1] >> 21 & 15) >= 14)
        w[1] &= ~(15u << 21);
    if (!(w[1] >> 21 & 15) && !(w[1] >> 25 & 7))
        w[1] |= 4u << 21;
    if (final)
        w[3] |= 1u;
}

enum { T = 1, V = 2, C = 3 };
#define XYZW 0x1B
#define WWWW 0xFF
#define XXXX 0x00

static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (mux << 26) | (reg << 28); }
    else if (which == 1) { w[2] |= (swz << 17) | (mux << 11) | (reg << 13); }
    else { w[2] |= (swz << 2) | (reg >> 2); w[3] |= (mux << 28) | ((reg & 3) << 30); }
}

static void print_diff(const char *what, const Nv2aVshOutput *want,
                       const Nv2aVshOutput *got)
{
    /* The fields that are not close (one at a time into a copy of want). */
    const float *p = (const float *)want, *q = (const float *)got;
    int j;
    printf("%s:", what);
    for (j = 0; j < (int)(sizeof *want / sizeof(float)); j++) {
        Nv2aVshOutput t = *want;
        ((float *)&t)[j] = q[j];
        if (nv2a_vsh_outputs_close(want, &t) >= 0)
            printf(" %s %g vs %g", nv2a_vsh_output_name(j), p[j], q[j]);
    }
    putchar('\n');
}

static float s_in[1024][NV2A_VSH_INPUTS][4];
static Nv2aVshOutput s_got[1024];

/* ---- step 2: the GPU's vertex fetch against nv2a_attr_decode ----------- */

static uint8_t s_mem[4 << 20];
static uint32_t s_idx[1024];
static float s_fetched[1024][NV2A_VSH_INPUTS][4];

/* pb_fetch_attr, on a host array. */
static void fetch_ref(const Nv2aVshArray *a, uint32_t i, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->src || !a->size || !a->stride || i > a->last)
        return;
    if (!nv2a_attr_decode(a->src + (size_t)i * a->stride, a->type, a->size,
                          out))
        out[0] = out[1] = out[2] = 0.0f, out[3] = 1.0f;
}

/* How far apart two floats are in units in the last place; both NaN = 0. */
static uint32_t ulps(float x, float y)
{
    int32_t a, b;
    int64_t d;
    if (x != x || y != y)
        return x != x && y != y ? 0 : 0xFFFFFFFFu;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    d = (a < 0 ? (int64_t)INT32_MIN - a : a) - (b < 0 ? (int64_t)INT32_MIN - b : b);
    d = d < 0 ? -d : d;
    return d > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)d;
}

/* Random batches: interleaved vertices of 4..80 bytes with attributes at
 * any byte offset in them (so unaligned ubyte, short and even float reads),
 * separate arrays, every format and some no decoder knows, sizes 0..5,
 * INLINE_ARRAY-style ends, absent and unused inputs, indices anywhere in a
 * range. Floats are compared to the bit (they are copied, not converted);
 * the normalising divides may differ by an ulp. */
static int fetch_test(int hw)
{
    static const uint32_t types[] = {0, 1, 2, 4, 5, 6, 3, 7};
    long values = 0, inexact = 0, worst = 0;
    int b, k, l, c;
    uint32_t i;

    for (i = 0; i < sizeof s_mem; i++)
        s_mem[i] = (uint8_t)rnd();
    for (b = 0; b < 400; b++) {
        Nv2aVshArray a[NV2A_VSH_INPUTS];
        uint32_t stride = 4 + rnd() % 77, used = rnd() & 0xFFFFu;
        uint32_t count = 1 + rnd() % 1024, range = 1 + rnd() % 4000;
        uint32_t base = rnd() % 100000, lo = ~0u, hi = 0;
        uint8_t *vtx = s_mem + 1024 + rnd() % (1u << 20);

        if (b % 50 == 0)
            used = 0xFFFFu;
        memset(a, 0, sizeof a);
        for (k = 0; k < NV2A_VSH_INPUTS; k++) {
            uint32_t r = rnd() % 16;
            a[k].type = types[rnd() % (r < 2 ? 8 : 6)];
            a[k].size = r == 3 ? 0 : r == 4 ? 5 : 1 + rnd() % 4;
            a[k].last = r == 5 ? base + rnd() % (range + 1) : UINT32_MAX;
            if (r == 6) {
                continue;                       /* absent */
            } else if (r < 10) {                /* interleaved */
                a[k].stride = stride;
                a[k].src = vtx + rnd() % stride;
            } else {                            /* an array of its own */
                a[k].stride = nv2a_attr_bytes(a[k].type, a[k].size);
                a[k].stride += rnd() % 3;
                if (!a[k].stride)
                    a[k].stride = 4;
                a[k].src = s_mem + 1024 + (1u << 20) + rnd() % (1u << 20);
            }
            /* Element 0 lies `base` elements back: the copy must not need
             * it. Floats get values a program would see, mostly. */
            a[k].src -= (size_t)base * a[k].stride;
            if (a[k].type == 2 && rnd() % 4)
                for (i = base; i < base + range; i++)
                    for (c = 0; c < (int)a[k].size && c < 4; c++) {
                        float f = rndf();
                        memcpy((uint8_t *)a[k].src + (size_t)i * a[k].stride
                               + 4 * c, &f, 4);
                    }
        }
        for (l = 0; l < (int)count; l++) {
            s_idx[l] = base + rnd() % range;
            if (s_idx[l] < lo) lo = s_idx[l];
            if (s_idx[l] > hi) hi = s_idx[l];
        }
        if (!nv2a_vsh_gpu_inputs(a, used, lo, hi)
            || !nv2a_vsh_gpu_fetch(s_idx, count, s_fetched)) {
            printf("FAIL: fetch batch %d did not run\n", b);
            return 0;
        }
        for (l = 0; l < (int)count; l++)
            for (k = 0; k < NV2A_VSH_INPUTS; k++) {
                float want[4];
                if (!(used >> k & 1))
                    continue;
                fetch_ref(&a[k], s_idx[l], want);
                for (c = 0; c < 4; c++) {
                    uint32_t u = ulps(want[c], s_fetched[l][k][c]);
                    int exact = !memcmp(&want[c], &s_fetched[l][k][c], 4);
                    values++;
                    if (exact)
                        continue;
                    inexact++;
                    if ((long)u > worst)
                        worst = (long)u;
                    if (u > 1 || a[k].type == 2 || a[k].type == 5) {
                        printf("FAIL: fetch batch %d vertex %d (index %u)"
                               " input %d type %u size %u stride %u: %c cpu"
                               " %.9g gpu %.9g\n", b, l, s_idx[l], k,
                               a[k].type, a[k].size, a[k].stride, "xyzw"[c],
                               want[c], s_fetched[l][k][c]);
                        return 0;
                    }
                }
            }
    }
    {
        uint64_t batches, bytes, wraps;
        nv2a_vsh_gpu_input_stats(&batches, &bytes, &wraps);
        printf("fetch: %d batches, %ld values, %ld not to the bit (at most"
               " %ld ulp); %llu bytes copied, %llu ring restarts%s\n", b,
               values, inexact, worst, (unsigned long long)bytes,
               (unsigned long long)wraps, hw ? " (hw)" : "");
    }
    return 1;
}

/* ---- step 3: drawing with the program on the GPU ---------------------- */

/* The back end's shaders, as nv2a_draw_d3d11.c compiles them. */
#include "nv2a_draw_d3d11_hlsl.h"
#include <d3dcompiler.h>

#define DT_W 256
#define DT_H 256
#define DT_TRIS 200

static ID3DBlob *dt_compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(s_hlsl, sizeof s_hlsl - 1, "nv2a_draw_d3d11", NULL,
                          NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                          0, &code, &err))) {
        printf("FAIL: draw test shader %s: %s\n", entry,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

typedef struct {
    ID3D11Texture2D *rt, *ds, *rt_stage, *ds_stage;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
} DtTarget;

static int dt_target(ID3D11Device *dev, DtTarget *t)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    memset(&td, 0, sizeof td);
    td.Width = DT_W;
    td.Height = DT_H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t->rt))
        || FAILED(ID3D11Device_CreateRenderTargetView(dev,
                  (ID3D11Resource *)t->rt, NULL, &t->rtv)))
        return 0;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    memset(&dd, 0, sizeof dd);
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t->ds))
        || FAILED(ID3D11Device_CreateDepthStencilView(dev,
                  (ID3D11Resource *)t->ds, &dd, &t->dsv)))
        return 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t->ds_stage)))
        return 0;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL,
                                                  &t->rt_stage));
}

static void dt_read(ID3D11DeviceContext *ctx, ID3D11Texture2D *from,
                    ID3D11Texture2D *stage, uint32_t *out)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int y;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stage,
                                     (ID3D11Resource *)from);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stage, 0,
                                       D3D11_MAP_READ, 0, &m))) {
        memset(out, 0, DT_W * DT_H * 4);
        return;
    }
    for (y = 0; y < DT_H; y++)
        memcpy(out + y * DT_W, (const uint8_t *)m.pData + y * m.RowPitch,
               DT_W * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage, 0);
}

/* A program of the Xbox D3D epilogue plus mov oD0, v3, drawn twice into
 * 256 x 256 with depth: through vs_gpu (the batch's bytes fetched on the
 * GPU, the program run there), and as the back end draws it today, the
 * interpreter's vertices through vs. Same pixel shader (ps, path 1 with
 * nothing bound: the interpolated colour). Triangles lie wholly in front,
 * where the CPU path does no clipping, and must give the same picture and
 * depth but for edge pixels. Then triangles wholly behind the eye or the
 * near plane, which vs_gpu's clip distances must drop. */
static int draw_test(ID3D11Device *dev, ID3D11DeviceContext *ctx, int hw)
{
    static uint32_t a_rgb[DT_W * DT_H], a_z[DT_W * DT_H];
    static uint32_t b_rgb[DT_W * DT_H], b_z[DT_W * DT_H];
    static Nv2aVshOutput xf[DT_TRIS * 3];
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
#define EL(name, idx, off) { name, idx, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, \
                             off, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        EL("POSITION", 0, 0), EL("COLOR", 0, 16), EL("COLOR", 1, 32),
        EL("TEXCOORD", 4, 48), EL("TEXCOORD", 0, 64), EL("TEXCOORD", 1, 80),
        EL("TEXCOORD", 2, 96), EL("TEXCOORD", 3, 112),
#undef EL
    };
    struct { float pos[4]; uint32_t col; } vtx[DT_TRIS * 3];
    uint32_t idx[DT_TRIS * 3], k[180], used, i, pass, bad_rgb = 0, bad_z = 0;
    uint32_t covered = 0, worst_z = 0, w0[4] = {0}, w1[4] = {0}, w2[4] = {0};
    uint32_t w3[4] = {0};
    const float c58[4] = {DT_W / 2.0f, -DT_H / 2.0f, 16777215.0f, 0};
    const float c59[4] = {DT_W / 2.0f, DT_H / 2.0f, 0, 0};
    ID3DBlob *vsb = dt_compile("vs", "vs_4_0"), *psb = dt_compile("ps", "ps_4_0");
    ID3D11VertexShader *cpu_vs = NULL, *gvs;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11Buffer *cb = NULL, *vb = NULL, *ib = NULL;
    ID3D11RasterizerState *rs = NULL;
    ID3D11DepthStencilState *dss = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dsd;
    D3D11_VIEWPORT vp = {0, 0, DT_W, DT_H, 0, 1};
    Nv2aVshArray arr[NV2A_VSH_INPUTS];
    DtTarget t;
    double t0;
    UINT stride = sizeof(Nv2aVshOutput), zero = 0;

    if (!vsb || !psb)
        return 0;
    memset(&t, 0, sizeof t);
    if (FAILED(ID3D11Device_CreateVertexShader(dev,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
            NULL, &cpu_vs))
        || FAILED(ID3D11Device_CreatePixelShader(dev,
            ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb),
            NULL, &ps))
        || FAILED(ID3D11Device_CreateInputLayout(dev, el, 8,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb),
            &layout))
        || !dt_target(dev, &t)) {
        puts("FAIL: draw test objects");
        return 0;
    }

    /* mov oPos, v0; mov oD0, v3; mul oPos.xyz, R12, c58 + rcc R1.x, R12.w;
     * mad oPos.xyz, R12, R1.x, c59 (the epilogue as in main). */
    w0[1] = (1u << 21);
    src(w0, 0, V, 0, XYZW);
    w0[3] |= (0xFu << 12) | (1u << 11);
    w1[1] = (1u << 21) | (3u << 9);
    src(w1, 0, V, 3, XYZW);
    w1[3] |= (0xFu << 12) | (1u << 11) | (3u << 3);
    w2[1] = (3u << 25) | (2u << 21) | (58u << 13);
    src(w2, 0, T, 12, XYZW); src(w2, 1, C, 0, XYZW); src(w2, 2, T, 12, WWWW);
    w2[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);
    w3[1] = (4u << 21) | (59u << 13);
    src(w3, 0, T, 12, XYZW); src(w3, 1, T, 1, XXXX); src(w3, 2, C, 0, XYZW);
    w3[3] |= (0xEu << 12) | (1u << 11) | 1u;
    nv2a_vsh_set_instruction(0, w0);
    nv2a_vsh_set_instruction(1, w1);
    nv2a_vsh_set_instruction(2, w2);
    nv2a_vsh_set_instruction(3, w3);
    nv2a_vsh_set_start_slot(1);
    nv2a_vsh_set_start_slot(0);
    nv2a_vsh_set_constant(58, c58);
    nv2a_vsh_set_constant(59, c59);
    nv2a_vsh_gpu_draw_source(s_hlsl_vsh_head, s_hlsl_vsh_draw, "vs_gpu");
    t0 = (double)GetTickCount64();
    if (!nv2a_vsh_gpu_prepare(NULL) || !(gvs = nv2a_vsh_gpu_draw_shader(1))) {
        puts("FAIL: draw shader did not compile");
        return 0;
    }
    printf("draw: shader ready after %.0f ms\n",
           (double)GetTickCount64() - t0);
    nv2a_vsh_prepare(&used);

    /* cbuffer C (nv2a_draw_d3d11_hlsl.h): rt, mode (path 1), zsc. */
    memset(k, 0, sizeof k);
    {
        float rt[4] = {2.0f / DT_W, -2.0f / DT_H, -1.0f, 1.0f};
        float zsc = 1.0f / 16777215.0f;
        memcpy(&k[0], rt, sizeof rt);
        k[20] = 1;
        memcpy(&k[144], &zsc, 4);
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof k;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    init.pSysMem = k;
    init.SysMemPitch = init.SysMemSlicePitch = 0;
    ID3D11Device_CreateBuffer(dev, &bd, &init, &cb);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    memset(&dsd, 0, sizeof dsd);
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11Device_CreateDepthStencilState(dev, &dsd, &dss);

    for (pass = 0; pass < 2; pass++) {
        uint32_t n = DT_TRIS * 3, v;
        float clear[4] = {0, 0, 0, 0};
        /* pass 0: in front; pass 1: wholly behind the eye or the near
         * plane, so nothing may be drawn. */
        for (v = 0; v < n; v++) {
            float w = 0.5f + (float)(rnd() % 3500) / 1000.0f;
            float nx = ((float)(rnd() % 2401) - 1200.0f) / 1000.0f;
            float ny = ((float)(rnd() % 2401) - 1200.0f) / 1000.0f;
            float nz = (float)(rnd() % 1000) / 1000.0f;
            if (pass == 1) {
                if ((v / 3) & 1) w = -w;        /* behind the eye */
                else nz = -0.01f - nz;          /* before the near plane */
            }
            vtx[v].pos[0] = nx * w;
            vtx[v].pos[1] = ny * w;
            vtx[v].pos[2] = nz * w;
            vtx[v].pos[3] = w;
            vtx[v].col = rnd() | 0xFF000000u;
            idx[v] = n - 1 - v;                 /* not 0..n-1 in order */
        }
        memset(arr, 0, sizeof arr);
        arr[0].src = (const uint8_t *)vtx[0].pos;
        arr[0].type = 2; arr[0].size = 4; arr[0].stride = sizeof vtx[0];
        arr[0].last = UINT32_MAX;
        arr[3].src = (const uint8_t *)&vtx[0].col;
        arr[3].type = 0; arr[3].size = 4; arr[3].stride = sizeof vtx[0];
        arr[3].last = UINT32_MAX;
        for (v = 0; v < n; v++) {
            float in[NV2A_VSH_INPUTS][4];
            memset(in, 0, sizeof in);
            for (i = 0; i < NV2A_VSH_INPUTS; i++) in[i][3] = 1.0f;
            memcpy(in[0], vtx[idx[v]].pos, 16);
            nv2a_attr_decode((const uint8_t *)&vtx[idx[v]].col, 0, 4, in[3]);
            nv2a_vsh_run((const float (*)[4])in, &xf[v]);
        }
        if (vb) ID3D11Buffer_Release(vb);
        if (ib) ID3D11Buffer_Release(ib);
        bd.ByteWidth = n * sizeof(Nv2aVshOutput);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        init.pSysMem = xf;
        ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);
        bd.ByteWidth = n * 4;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = idx;
        ID3D11Device_CreateBuffer(dev, &bd, &init, &ib);

        ID3D11DeviceContext_RSSetState(ctx, rs);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 0);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &t.rtv, t.dsv);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);

        /* A: the program on the GPU, indexed by the batch's own indices. */
        ID3D11DeviceContext_ClearRenderTargetView(ctx, t.rtv, clear);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, t.dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        if (!nv2a_vsh_gpu_inputs(arr, used, 0, n - 1)
            || !nv2a_vsh_gpu_constants()) {
            puts("FAIL: draw test inputs");
            return 0;
        }
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_VSSetShader(ctx, gvs, NULL, 0);
        ID3D11DeviceContext_DrawIndexed(ctx, n, 0, 0);
        dt_read(ctx, t.rt, t.rt_stage, a_rgb);
        dt_read(ctx, t.ds, t.ds_stage, a_z);
        if (pass == 1) {
            for (i = 0; i < DT_W * DT_H; i++)
                if (a_rgb[i] || (a_z[i] & 0xFFFFFFu) != 0xFFFFFFu) {
                    printf("FAIL: draw: a triangle behind the eye or the near"
                           " plane drew pixel %u,%u\n", i % DT_W, i / DT_W);
                    return 0;
                }
            break;
        }

        /* B: today's path, the interpreter's vertices through vs. */
        ID3D11DeviceContext_ClearRenderTargetView(ctx, t.rtv, clear);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, t.dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &zero);
        ID3D11DeviceContext_VSSetShader(ctx, cpu_vs, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, n, 0);
        dt_read(ctx, t.rt, t.rt_stage, b_rgb);
        dt_read(ctx, t.ds, t.ds_stage, b_z);

        for (i = 0; i < DT_W * DT_H; i++) {
            uint32_t za = a_z[i] & 0xFFFFFFu, zb = b_z[i] & 0xFFFFFFu, c;
            uint32_t dz = za > zb ? za - zb : zb - za;
            if (b_rgb[i])
                covered++;
            for (c = 0; c < 32; c += 8) {
                int d = (int)(a_rgb[i] >> c & 255) - (int)(b_rgb[i] >> c & 255);
                if (d > 2 || d < -2) {
                    bad_rgb++;
                    break;
                }
            }
            if (dz > 16)
                bad_z++;
            else if (dz > worst_z)
                worst_z = dz;
        }
    }
    printf("draw: %u of %u pixels covered; %u differ in colour, %u in depth"
           " by more than 16 units (2^-24), otherwise at most %u%s\n",
           covered, DT_W * DT_H, bad_rgb, bad_z, worst_z, hw ? " (hw)" : "");
    /* Edge pixels may go either way between two positions a few ulps
     * apart: at most 1% of the covered ones. */
    if (covered < DT_W * DT_H / 2 || bad_rgb * 100 > covered
        || bad_z * 100 > covered) {
        puts("FAIL: draw");
        return 0;
    }
    return 1;
}

/* nv2a_vsh_gpu_precompile_stats's waiting count reaching 0, or 60 s. */
static int pre_wait(uint32_t *compiled, uint32_t *had, uint32_t *failed)
{
    ULONGLONG t0 = GetTickCount64();
    uint32_t waiting;

    do {
        Sleep(5);
        nv2a_vsh_gpu_precompile_stats(compiled, had, failed, &waiting);
    } while (waiting && GetTickCount64() - t0 < 60000);
    return waiting == 0;
}

/* Shader pre-compilation: a program given as words (as a memory scan finds
 * it) decodes as the uploaded one does, its drawing shader is made into the
 * disk cache, a second request finds it there, and the drawing path then
 * has it at once, without a compile. After draw_test (the back end's HLSL
 * is set) and before the random programs fill the program cache. */
static int precompile_test(void)
{
    static Nv2aVshIns dec[NV2A_VSH_SLOTS];
    const Nv2aVshIns *ins;
    uint32_t w[6][4], i, n, n2, used, used2, compiled, had, failed;
    uint32_t keep = s_rng;
    uint64_t h, h2;

    s_rng = 0x5EED1234u;                 /* the random programs' run unchanged */
    for (i = 0; i < 6; i++) {
        random_instruction(w[i], i == 5);
        nv2a_vsh_set_instruction(i, w[i]);
    }
    s_rng = keep;
    nv2a_vsh_set_start_slot(1);
    nv2a_vsh_set_start_slot(0);
    n = nv2a_vsh_decode_words(&w[0][0], 6, dec, &used, &h);
    ins = nv2a_vsh_decoded(&n2, &h2);
    nv2a_vsh_prepare(&used2);
    if (!ins || n != 6 || n != n2 || h != h2 || used != used2
        || memcmp(ins, dec, n * sizeof *ins)) {
        puts("FAIL: precompile: words decode differently from the upload");
        return 0;
    }
    w[5][3] &= ~1u;
    if (nv2a_vsh_decode_words(&w[0][0], 6, dec, &used, &h)) {
        puts("FAIL: precompile: a program without FINAL decoded");
        return 0;
    }
    w[5][3] |= 1u;

    if (!nv2a_vsh_gpu_precompile(&w[0][0], 6)
        || !pre_wait(&compiled, &had, &failed) || compiled != 1 || failed) {
        printf("FAIL: precompile: not made (%u made, %u failed)\n", compiled,
               failed);
        return 0;
    }
    if (!nv2a_vsh_gpu_precompile(&w[0][0], 6)
        || !pre_wait(&compiled, &had, &failed) || compiled != 1 || had != 1) {
        printf("FAIL: precompile: asked again, %u made, %u already there\n",
               compiled, had);
        return 0;
    }
    if (!nv2a_vsh_gpu_prepare(NULL) || !nv2a_vsh_gpu_draw_shader(0)) {
        puts("FAIL: precompile: the drawing path did not find the shader");
        return 0;
    }
    puts("precompile: made from words, found again, drawn from the cache");
    return 1;
}

/* *.vso in dir and dir itself. */
static void remove_cache(const char *dir)
{
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(path, sizeof path, "%s\\*.vso", dir);
    h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
            DeleteFileA(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

int main(int argc, char **argv)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    int hw = argc > 1 && !strcmp(argv[1], "hw");
    int prog, l, i, j, k, shown = 0;
    long checked = 0, differ = 0;
    uint32_t compiled, failed;
    double ms;
    HRESULT hr;
    char tmp[MAX_PATH], cache[MAX_PATH];

    /* A disk cache of this run's own (the pre-compiler needs one), removed
     * after the pre-compile test: the random programs make none. */
    GetTempPathA(sizeof tmp, tmp);
    snprintf(cache, sizeof cache, "%snv2a_vsh_gpu_test_%lu", tmp,
             GetCurrentProcessId());
    _putenv_s("RECOMP_VSH_GPU_CACHE", cache);

    hr = D3D11CreateDevice(NULL, hw ? D3D_DRIVER_TYPE_HARDWARE
                                    : D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        printf("FAIL: D3D11CreateDevice 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    if (!nv2a_vsh_gpu_init(dev, ctx)) {
        puts("FAIL: nv2a_vsh_gpu_init");
        return 1;
    }

    /* The epilogue (nv2a_vsh_test.c): the rcc beside a MAC op lands in R1. */
    {
        uint32_t i0[4] = {0}, i1[4] = {0}, i2[4] = {0};
        const float c58[4] = {320, -240, 1000, 0}, c59[4] = {320, 240, 0, 0};
        Nv2aVshOutput want;
        i0[1] = (1u << 21);
        src(i0, 0, V, 0, XYZW);
        i0[3] |= (0xFu << 12) | (1u << 11);
        i1[1] = (3u << 25) | (2u << 21) | (58u << 13);
        src(i1, 0, T, 12, XYZW); src(i1, 1, C, 0, XYZW); src(i1, 2, T, 12, WWWW);
        i1[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);
        i2[1] = (4u << 21) | (59u << 13);
        src(i2, 0, T, 12, XYZW); src(i2, 1, T, 1, XXXX); src(i2, 2, C, 0, XYZW);
        i2[3] |= (0xEu << 12) | (1u << 11) | 1u;
        nv2a_vsh_set_instruction(0, i0);
        nv2a_vsh_set_instruction(1, i1);
        nv2a_vsh_set_instruction(2, i2);
        nv2a_vsh_set_constant(58, c58);
        nv2a_vsh_set_constant(59, c59);
        memset(s_in, 0, sizeof s_in[0]);
        s_in[0][0][0] = 1.0f; s_in[0][0][1] = 0.5f;
        s_in[0][0][2] = 0.25f; s_in[0][0][3] = 2.0f;
        if (!nv2a_vsh_gpu_prepare(NULL)
            || !nv2a_vsh_gpu_run((const float (*)[NV2A_VSH_INPUTS][4])s_in, 1,
                                 s_got)) {
            puts("FAIL: epilogue did not run on the GPU");
            return 1;
        }
        nv2a_vsh_run((const float (*)[4])s_in[0], &want);
        if (nv2a_vsh_outputs_close(&want, &s_got[0]) >= 0
            || fabsf(s_got[0].pos[0] - 480) > 1e-3f) {
            print_diff("FAIL: epilogue", &want, &s_got[0]);
            return 1;
        }
    }

    if (!fetch_test(hw))
        return 1;
    if (!draw_test(dev, ctx, hw))
        return 1;
    i = precompile_test();
    remove_cache(cache);
    if (!i)
        return 1;

    for (prog = 0; prog < 1500; prog++) {
        int len = 1 + (int)(rnd() % 24), count = 1 + (int)(rnd() % 64);
        uint32_t w[4];

        for (i = 0; i < len; i++) {
            random_instruction(w, i == len - 1);
            if (i == 0 && rnd() % 3 == 0) {
                w[1] = (w[1] & ~(15u << 21) & ~(7u << 25)) | (13u << 21);
                w[2] = (w[2] & ~(3u << 26)) | (2u << 26);
            }
            nv2a_vsh_set_instruction((uint32_t)i, w);
        }
        nv2a_vsh_set_start_slot(1);
        nv2a_vsh_set_start_slot(0);
        for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
            float c[4];
            for (j = 0; j < 4; j++) c[j] = rndf();
            nv2a_vsh_set_constant((uint32_t)i, c);
        }
        for (l = 0; l < count; l++)
            for (i = 0; i < NV2A_VSH_INPUTS; i++)
                for (j = 0; j < 4; j++)
                    s_in[l][i][j] = rndf();

        if (!nv2a_vsh_gpu_prepare(NULL)) {
            printf("FAIL: program %d (%d instructions) did not compile\n",
                   prog, len);
            return 1;
        }
        /* VSH_TEST_DRAW_TIMES=1: every program's drawing shader is made
         * too, for its compile times (the last line); the first 400 (the
         * cache holds 512, and nothing draws from programs past it). */
        if (getenv("VSH_TEST_DRAW_TIMES") && prog < 400
            && !nv2a_vsh_gpu_draw_shader(1)) {
            printf("FAIL: program %d: no draw shader\n", prog);
            return 1;
        }
        if (!nv2a_vsh_gpu_run((const float (*)[NV2A_VSH_INPUTS][4])s_in,
                              (uint32_t)count, s_got)) {
            printf("FAIL: program %d did not run\n", prog);
            return 1;
        }
        /* VSH_TEST_SHOW=<n>: program n's HLSL, and its first vertex (or
         * VSH_TEST_VERTEX) step by step on the CPU ([VSTEP] on stderr). */
        if (getenv("VSH_TEST_SHOW") && atoi(getenv("VSH_TEST_SHOW")) == prog) {
            static char text[96 * 1024];
            uint32_t n;
            uint64_t h;
            const Nv2aVshIns *ins = nv2a_vsh_decoded(&n, &h);
            nv2a_vsh_hlsl(ins, n, text, sizeof text);
            fprintf(stderr, "%s\n", text);
            {
                Nv2aVshOutput want;
                int sv = getenv("VSH_TEST_VERTEX")
                         ? atoi(getenv("VSH_TEST_VERTEX")) % count : 0;
                nv2a_vsh_trace((const float (*)[4])s_in[sv]);
                nv2a_vsh_run((const float (*)[4])s_in[sv], &want);
                print_diff("shown vertex, cpu vs gpu", &want, &s_got[sv]);
                for (j = 0; j < 33; j++)
                    fprintf(stderr, "  %s cpu %g gpu %g\n",
                            nv2a_vsh_output_name(j), ((float *)&want)[j],
                            ((float *)&s_got[sv])[j]);
            }
        }
        for (l = 0, k = 0; l < count; l++) {
            Nv2aVshOutput want;
            nv2a_vsh_run((const float (*)[4])s_in[l], &want);
            checked++;
            if (nv2a_vsh_outputs_close(&want, &s_got[l]) >= 0) {
                char what[64];
                differ++;
                if (!k++ && shown++ < 25) {        /* one per program */
                    snprintf(what, sizeof what, "program %d (%d instructions)"
                             " vertex %d", prog, len, l);
                    print_diff(what, &want, &s_got[l]);
                }
            }
        }
    }
    nv2a_vsh_gpu_stats(&compiled, &failed, &ms);
    printf("%d programs (%u compiled, %.0f ms), %ld vertices, %ld differ"
           " (%.3f%%)\n", prog, compiled, ms, checked, differ,
           100.0 * (double)differ / (double)checked);
    {
        uint32_t made, dfailed, queued;
        double dms;
        nv2a_vsh_gpu_draw_stats(&made, &dfailed, &queued, &dms);
        printf("draw shaders: %u made (%u failed), %.0f ms compiling, %.1f ms"
               " each\n", made, dfailed, dms, made ? dms / made : 0.0);
        failed += dfailed;
    }
    if (failed || (hw ? differ * 200 > checked : differ > 0)) {
        puts("FAIL");
        return 1;
    }
    puts("ok");
    return 0;
}
