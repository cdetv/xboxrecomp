/**
 * NV2A vertex programs on the GPU (D3D11). See nv2a_vsh_gpu.h.
 *
 * RECOMP_VSH_GPU_DUMP=1 prints each program's HLSL when it is first
 * compiled; a program that fails to compile prints its HLSL anyway (the
 * first four).
 */
#if defined(_WIN32)
#include "nv2a_vsh_gpu.h"
#include "nv2a_vsh_hlsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#define VSH_CACHE    512                /* programs kept */
#define VSH_RUN_MAX  1024               /* vertices per stream-out pass */
#define VSH_IN_SIZE  (NV2A_VSH_INPUTS * 16)

typedef struct {
    uint64_t hash;
    uint32_t count;
    int      failed;
    ID3D11VertexShader   *vs;           /* vs_check */
    ID3D11GeometryShader *gs;           /* vs_check's outputs to stream-out */
} VshProg;

static ID3D11Device         *s_dev;
static ID3D11DeviceContext  *s_ctx;
static ID3D11Buffer *s_cb, *s_vb, *s_so, *s_stage;
static ID3D11InputLayout *s_layout;     /* ATTR0..15, from the first vs_check */
static VshProg s_prog[VSH_CACHE];
static uint32_t s_nprog, s_ncompiled, s_nfailed;
static VshProg *s_cur;                  /* the last prepared program */
/* What the cbuffer holds: c[0..191], then the zero row (nv2a_vsh_hlsl.h). */
static float s_crows[NV2A_VSH_HLSL_CROWS][4];
static double s_compile_ms;
static int s_ready;

int nv2a_vsh_gpu_init(struct ID3D11Device *dev, struct ID3D11DeviceContext *ctx)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA init;
    HRESULT hr;

    if (s_ready)
        return 1;
    s_dev = dev;
    s_ctx = ctx;

    /* The constants start as the interpreter's do, zero; whatever a title
     * wrote before this ran is still in the dirty range. */
    init.pSysMem = s_crows;
    init.SysMemPitch = init.SysMemSlicePitch = 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof s_crows;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(dev, &bd, &init, &s_cb);
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = VSH_RUN_MAX * VSH_IN_SIZE;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_vb);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = VSH_RUN_MAX * sizeof(Nv2aVshOutput);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        bd.CPUAccessFlags = 0;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_so);
    }
    if (SUCCEEDED(hr)) {
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_stage);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[VSHGPU] buffers failed: 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    fprintf(stderr, "[VSHGPU] ready\n");
    s_ready = 1;
    return 1;
}

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void compile_program(VshProg *p, const Nv2aVshIns *ins, uint32_t count)
{
    static char src[96 * 1024];
    static int dump = -1, dumped_failures;
    /* vs_check's outputs, in Nv2aVshOutput's order: 33 floats. */
    static const D3D11_SO_DECLARATION_ENTRY so[] = {
        {0, "TEXCOORD", 0, 0, 4, 0}, {0, "TEXCOORD", 1, 0, 4, 0},
        {0, "TEXCOORD", 2, 0, 4, 0}, {0, "TEXCOORD", 3, 0, 4, 0},
        {0, "TEXCOORD", 4, 0, 4, 0}, {0, "TEXCOORD", 5, 0, 4, 0},
        {0, "TEXCOORD", 6, 0, 4, 0}, {0, "TEXCOORD", 7, 0, 4, 0},
        {0, "TEXCOORD", 8, 0, 1, 0},
    };
    UINT stride = sizeof(Nv2aVshOutput);
    ID3DBlob *code = NULL, *err = NULL;
    size_t len;
    double t0 = now_ms(), ms;
    HRESULT hr;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_GPU_DUMP") != NULL;
    p->failed = 1;
    len = nv2a_vsh_hlsl(ins, count, src, sizeof src);
    if (!len) {
        fprintf(stderr, "[VSHGPU] program %016llX: %u instructions do not"
                " translate\n", (unsigned long long)p->hash, count);
        s_nfailed++;
        return;
    }
    /* IEEE strictness: no folding x*0 to 0 or dropping NaN checks, which
     * the interpreter's results depend on. */
    hr = D3DCompile(src, len, "nv2a_vsh", NULL, NULL, "vs_check", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                    0, &code, &err);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(s_dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &p->vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(s_dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), so,
                (UINT)(sizeof so / sizeof so[0]), &stride, 1,
                D3D11_SO_NO_RASTERIZED_STREAM, NULL, &p->gs);
    if (SUCCEEDED(hr) && !s_layout) {
        D3D11_INPUT_ELEMENT_DESC el[NV2A_VSH_INPUTS];
        UINT i;
        for (i = 0; i < NV2A_VSH_INPUTS; i++) {
            el[i].SemanticName = "ATTR";
            el[i].SemanticIndex = i;
            el[i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            el[i].InputSlot = 0;
            el[i].AlignedByteOffset = i * 16;
            el[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            el[i].InstanceDataStepRate = 0;
        }
        hr = ID3D11Device_CreateInputLayout(s_dev, el, NV2A_VSH_INPUTS,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), &s_layout);
    }
    ms = now_ms() - t0;
    s_compile_ms += ms;
    if (FAILED(hr)) {
        fprintf(stderr, "[VSHGPU] program %016llX: %u instructions, compile"
                " failed (0x%08lX): %s\n", (unsigned long long)p->hash, count,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (!dump && dumped_failures++ < 4)
            fprintf(stderr, "%s\n", src);
        if (p->vs) { ID3D11VertexShader_Release(p->vs); p->vs = NULL; }
        if (p->gs) { ID3D11GeometryShader_Release(p->gs); p->gs = NULL; }
        s_nfailed++;
    } else {
        p->failed = 0;
        s_ncompiled++;
        fprintf(stderr, "[VSHGPU] program %016llX: %u instructions, %u bytes"
                " of HLSL, compiled in %.1f ms\n", (unsigned long long)p->hash,
                count, (unsigned)len, ms);
    }
    if (dump)
        fprintf(stderr, "%s\n", src);
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
}

int nv2a_vsh_gpu_prepare(int *is_new)
{
    static VshProg spare;                   /* when the cache is full */
    const Nv2aVshIns *ins;
    uint32_t count, i;
    uint64_t hash;
    VshProg *p;

    if (is_new)
        *is_new = 0;
    if (!s_ready || !(ins = nv2a_vsh_decoded(&count, &hash))) {
        s_cur = NULL;
        return 0;
    }
    if (s_cur && s_cur->hash == hash && s_cur->count == count)
        return !s_cur->failed;
    for (i = 0; i < s_nprog; i++)
        if (s_prog[i].hash == hash && s_prog[i].count == count) {
            s_cur = &s_prog[i];
            return !s_cur->failed;
        }
    if (is_new)
        *is_new = 1;
    if (s_nprog < VSH_CACHE) {
        p = &s_prog[s_nprog++];
    } else {
        /* Full: the newcomer is compiled into the spare every time it is
         * seen. A title with 512 programs is not one this was sized for. */
        static int said;
        if (!said++)
            fprintf(stderr, "[VSHGPU] program cache full (%d)\n", VSH_CACHE);
        if (spare.vs) ID3D11VertexShader_Release(spare.vs);
        if (spare.gs) ID3D11GeometryShader_Release(spare.gs);
        p = &spare;
    }
    memset(p, 0, sizeof *p);
    p->hash = hash;
    p->count = count;
    compile_program(p, ins, count);
    s_cur = p;
    return !p->failed;
}

int nv2a_vsh_gpu_constants(void)
{
    uint32_t lo, hi;

    if (!s_ready)
        return 0;
    /* All 3 KB whenever any changed. A box (UpdateSubresource1) is only
     * allowed on a constant buffer with COPY_DISCARD, which drops the rest,
     * or COPY_NO_OVERWRITE, which races batches still reading the old
     * values; with a box and neither, WARP left the buffer as it was. */
    if (nv2a_vsh_constants_take_dirty(&lo, &hi)) {
        memcpy(s_crows[lo], nv2a_vsh_constant(lo), (hi - lo + 1) * 16);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_cb,
                0, NULL, s_crows, 0, 0);
    }
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, NV2A_VSH_HLSL_CBUFFER, 1,
                                             &s_cb);
    return 1;
}

int nv2a_vsh_gpu_run(const float (*in)[NV2A_VSH_INPUTS][4], uint32_t count,
                     Nv2aVshOutput *out)
{
    VshProg *p = s_cur;
    UINT stride = VSH_IN_SIZE, zero = 0;
    ID3D11Buffer *none = NULL;
    uint32_t done = 0;

    if (!s_ready || !p || p->failed || !s_layout)
        return 0;
    if (!nv2a_vsh_gpu_constants())
        return 0;
    while (done < count) {
        uint32_t m = count - done;
        D3D11_MAPPED_SUBRESOURCE ms;
        D3D11_BOX box;
        if (m > VSH_RUN_MAX)
            m = VSH_RUN_MAX;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &ms)))
            return 0;
        memcpy(ms.pData, in[done], (size_t)m * VSH_IN_SIZE);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);

        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
                D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride,
                                               &zero);
        ID3D11DeviceContext_VSSetShader(s_ctx, p->vs, NULL, 0);
        ID3D11DeviceContext_GSSetShader(s_ctx, p->gs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, NULL, NULL, 0);
        ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &s_so, &zero);
        ID3D11DeviceContext_Draw(s_ctx, m, 0);
        ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &none, &zero);
        ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);

        box.left = 0;
        box.right = m * (UINT)sizeof(Nv2aVshOutput);
        box.top = box.front = 0;
        box.bottom = box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                (ID3D11Resource *)s_stage, 0, 0, 0, 0,
                (ID3D11Resource *)s_so, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_stage,
                0, D3D11_MAP_READ, 0, &ms)))
            return 0;
        memcpy(&out[done], ms.pData, (size_t)m * sizeof(Nv2aVshOutput));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_stage, 0);
        done += m;
    }
    return 1;
}

void nv2a_vsh_gpu_stats(uint32_t *compiled, uint32_t *failed, double *ms)
{
    *compiled = s_ncompiled;
    *failed = s_nfailed;
    *ms = s_compile_ms;
}

#endif /* _WIN32 */
