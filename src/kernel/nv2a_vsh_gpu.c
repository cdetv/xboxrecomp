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
#include "nv2a_attr.h"

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

/* draw_state: the drawing shader not asked for yet, waiting for a worker,
 * made, or failed. */
enum { DRAW_NONE, DRAW_QUEUED, DRAW_READY, DRAW_FAILED };

typedef struct {
    uint64_t hash;
    uint32_t count;
    int      failed;                    /* does not translate */
    int      check_tried;
    ID3D11VertexShader   *vs;           /* vs_check */
    ID3D11GeometryShader *gs;           /* vs_check's outputs to stream-out */
    volatile LONG         draw_state;
    ID3D11VertexShader   *draw_vs;      /* the drawing entry, once READY */
} VshProg;

static ID3D11Device         *s_dev;
static ID3D11DeviceContext  *s_ctx;
static ID3D11Buffer *s_cb, *s_vb, *s_so, *s_stage;
static ID3D11InputLayout *s_layout;     /* ATTR0..15, from the first vs_check */
static VshProg s_prog[VSH_CACHE];
static VshProg s_spare;                 /* when the cache is full */
static uint32_t s_nprog, s_ncompiled, s_nfailed;
static VshProg *s_cur;                  /* the last prepared program */
void (*nv2a_vsh_gpu_new_program_hook)(uint64_t hash);

/* Step 3: the drawing shaders, compiled on worker threads (the D3D11
 * device is free-threaded) so a program seen for the first time costs the
 * executor nothing; its batches go through the CPU until the shader is
 * ready. fxc took 31 ms a program on average, 95 at most (It's War,
 * 2026-10-18): each one was a hitch when compiled on the executor. */
typedef struct {
    VshProg *p;
    char    *src;
    size_t   len;
    uint64_t key;                       /* src_key(src): the disk cache's name */
} DrawJob;

/* RECOMP_VSH_GPU_CACHE=<folder>: every drawing shader compiled is kept
 * there as <key>.vso (its bytecode), and all of them are loaded on a
 * thread of their own when drawing starts, so a program compiles once per
 * machine instead of once per run. Worker threads alone did not hide a
 * compile: a new program's batches go the CPU way until its shader is
 * ready, and a burst of new effects -- It's War's boat opening, 27
 * programs in 4 s -- drew ~83,000 batches that way, 30 -> 22 fps
 * (2026-10-20). The key hashes the whole source and the compile options,
 * so a changed translator or head names new files and never loads an old
 * shader. Unset: no disk cache. 4096: the pre-compiler adds every program a
 * scan finds, ~830 in Conker's soak test (1024 until 2026-10-24). */
#define VSH_DISK_MAX 4096
typedef struct {
    uint64_t key;
    ID3D11VertexShader *vs;
    int pre;                            /* made by the pre-compiler this run */
} DiskShader;
static DiskShader s_disk[VSH_DISK_MAX];  /* under s_q_cs */
static uint32_t s_ndisk;
static char s_cache_dir[MAX_PATH];
static int s_cache_on = -1;
static const char *s_dhead, *s_dtail, *s_dentry;
static INIT_ONCE          s_q_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION   s_q_cs;
static CONDITION_VARIABLE s_q_work, s_q_done;
static DrawJob s_q[VSH_CACHE];          /* a program is queued once at most */
static uint32_t s_q_head, s_q_tail;
static volatile LONG s_workers;         /* 0 = not started, -1 = none */
static uint32_t s_dcompiled, s_dfailed; /* under s_q_cs */
static double s_dms;

/* Shader pre-compilation (nv2a_vsh_gpu_precompile): programs found in guest
 * memory before anything draws them, compiled into the disk cache by the
 * same workers, but only while no drawing shader is waiting: a program on
 * screen now comes before one that may be drawn later. A job holds the
 * program's words; decoding and the HLSL are the worker's, so the caller
 * (the scan thread) only copies. */
#define VSH_PRE_MAX 4096
typedef struct {
    uint32_t *words;                    /* count instructions of 4 words */
    uint32_t count;
} PreJob;
static PreJob s_pq[VSH_PRE_MAX];        /* under s_q_cs */
static uint32_t s_pq_head, s_pq_tail, s_pre_busy;
static uint32_t s_pre_compiled, s_pre_loaded, s_pre_had, s_pre_failed;
static uint32_t s_pre_dropped, s_pre_done_said;
static double s_pre_ms;
/* What the cbuffer holds: c[0..191], then the zero row (nv2a_vsh_hlsl.h). */
static float s_crows[NV2A_VSH_HLSL_CROWS][4];
static double s_compile_ms;
static int s_ready;

/* Step 2: the input ring, appended to batch after batch (NO_OVERWRITE) and
 * started again from the top with a DISCARD when full. Without the D3D11.1
 * option that allows NO_OVERWRITE on a buffer read through a view, every
 * batch DISCARDs. */
#define VSH_RING_SIZE  (32u << 20)
#define VSH_MERGE_GAP  64u              /* bytes between arrays copied anyway */
#define VSH_FETCH_SIZE (NV2A_VSH_INPUTS * 16)
static ID3D11Buffer *s_ring, *s_acb, *s_ib, *s_fso, *s_fstage;
static ID3D11ShaderResourceView *s_ring_srv;
static ID3D11VertexShader *s_fetch_vs;
static ID3D11GeometryShader *s_fetch_gs;
static int s_fetch_tried, s_no_overwrite;
static uint32_t s_ring_at;
static uint64_t s_in_batches, s_in_bytes, s_in_wraps;

static HRESULT make_input_buffers(ID3D11Device *dev)
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    memset(&opt, 0, sizeof opt);
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(dev,
            D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof opt)))
        s_no_overwrite = opt.MapNoOverwriteOnDynamicBufferSRV != 0;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VSH_RING_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_ring);
    if (SUCCEEDED(hr)) {
        memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements = VSH_RING_SIZE / 4;
        sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)s_ring, &sv, &s_ring_srv);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = NV2A_VSH_INPUTS * 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.MiscFlags = 0;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_acb);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = VSH_RUN_MAX * 4;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_ib);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = VSH_RUN_MAX * VSH_FETCH_SIZE;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
        bd.CPUAccessFlags = 0;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_fso);
    }
    if (SUCCEEDED(hr)) {
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_fstage);
    }
    return hr;
}

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
    if (SUCCEEDED(hr))
        hr = make_input_buffers(dev);
    if (FAILED(hr)) {
        fprintf(stderr, "[VSHGPU] buffers failed: 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    fprintf(stderr, "[VSHGPU] ready (input ring %u MB, %s)\n",
            VSH_RING_SIZE >> 20, s_no_overwrite ? "appended"
            : "no NO_OVERWRITE on views: DISCARD per batch");
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

static int s_dump = -1, s_dumped_failures;

/* vs_check and its stream-out geometry shader for p, which is the program
 * nv2a_vsh_decoded has now. */
static void compile_check(VshProg *p, const Nv2aVshIns *ins, uint32_t count)
{
    static char src[96 * 1024];
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

    if (s_dump < 0)
        s_dump = getenv("RECOMP_VSH_GPU_DUMP") != NULL;
    p->check_tried = 1;
    len = nv2a_vsh_hlsl(ins, count, src, sizeof src);
    if (!len) {
        p->failed = 1;
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
        if (!s_dump && s_dumped_failures++ < 4)
            fprintf(stderr, "%s\n", src);
        if (p->vs) { ID3D11VertexShader_Release(p->vs); p->vs = NULL; }
        if (p->gs) { ID3D11GeometryShader_Release(p->gs); p->gs = NULL; }
        s_nfailed++;
    } else {
        s_ncompiled++;
        fprintf(stderr, "[VSHGPU] program %016llX: %u instructions, %u bytes"
                " of HLSL, check shader compiled in %.1f ms\n",
                (unsigned long long)p->hash, count, (unsigned)len, ms);
    }
    if (s_dump)
        fprintf(stderr, "%s\n", src);
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
}

int nv2a_vsh_gpu_prepare(int *is_new)
{
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
         * seen, for checks only (nothing draws from the spare). A title
         * with 512 programs is not one this was sized for. */
        static int said;
        if (!said++)
            fprintf(stderr, "[VSHGPU] program cache full (%d)\n", VSH_CACHE);
        if (s_spare.vs) ID3D11VertexShader_Release(s_spare.vs);
        if (s_spare.gs) ID3D11GeometryShader_Release(s_spare.gs);
        p = &s_spare;
    }
    memset(p, 0, sizeof *p);
    p->hash = hash;
    p->count = count;
    s_cur = p;
    return 1;
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

    if (!s_ready || !p || p->failed)
        return 0;
    if (!p->check_tried) {
        uint32_t nins;
        uint64_t hash;
        const Nv2aVshIns *ins = nv2a_vsh_decoded(&nins, &hash);
        if (!ins || hash != p->hash || nins != p->count)
            return 0;
        compile_check(p, ins, nins);
    }
    if (!p->vs || !s_layout)
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

/* ---- step 3: drawing with them ----------------------------------------- */

/* The options compile_draw passes to D3DCompile, as part of every key. */
#define VSH_DRAW_PROFILE "vs_5_0"
#define VSH_DRAW_FLAGS   (D3DCOMPILE_OPTIMIZATION_LEVEL3 \
                          | D3DCOMPILE_IEEE_STRICTNESS)

static uint64_t src_key(const char *s, size_t n)
{
    static const char tag[] = "nv2a_vsh_draw v1 " VSH_DRAW_PROFILE;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t flags = VSH_DRAW_FLAGS;
    size_t i;

    for (i = 0; i < sizeof tag; i++)
        h = (h ^ (uint8_t)tag[i]) * 0x100000001B3ull;
    for (i = 0; i < sizeof flags; i++)
        h = (h ^ ((flags >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    for (i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 0x100000001B3ull;
    return h;
}

static int cache_on(void)
{
    if (s_cache_on < 0) {
        const char *e = getenv("RECOMP_VSH_GPU_CACHE");
        s_cache_on = e && e[0] && strcmp(e, "0") != 0
                     && strlen(e) < sizeof s_cache_dir - 32;
        if (s_cache_on) {
            strcpy(s_cache_dir, e);
            CreateDirectoryA(s_cache_dir, NULL);
        }
    }
    return s_cache_on;
}

static void cache_path(char *out, size_t cap, uint64_t key)
{
    snprintf(out, cap, "%s\\%016llX.vso", s_cache_dir,
             (unsigned long long)key);
}

/* Callers hold s_q_cs. */
static DiskShader *disk_find(uint64_t key)
{
    uint32_t i;
    for (i = 0; i < s_ndisk; i++)
        if (s_disk[i].key == key)
            return &s_disk[i];
    return NULL;
}

/* Callers hold s_q_cs. The shader kept for key: vs, or the one already
 * there (vs is then released). Shaders are never freed, as draw_vs. */
static ID3D11VertexShader *disk_add(uint64_t key, ID3D11VertexShader *vs,
                                    int pre)
{
    DiskShader *had = disk_find(key);
    if (had) {
        if (had->vs != vs)
            ID3D11VertexShader_Release(vs);
        return had->vs;
    }
    if (s_ndisk < VSH_DISK_MAX) {
        s_disk[s_ndisk].key = key;
        s_disk[s_ndisk].pre = pre;
        s_disk[s_ndisk++].vs = vs;
    }
    return vs;
}

/* <key>.vso as a shader, or NULL (absent, unreadable or rejected). */
static ID3D11VertexShader *cache_load_file(uint64_t key)
{
    char path[MAX_PATH];
    ID3D11VertexShader *vs = NULL;
    FILE *f;
    long n;
    void *buf;

    cache_path(path, sizeof path, key);
    if (fopen_s(&f, path, "rb") || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 && n < (1 << 20) ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n
        && FAILED(ID3D11Device_CreateVertexShader(s_dev, buf, (SIZE_T)n,
                                                  NULL, &vs)))
        vs = NULL;
    free(buf);
    fclose(f);
    return vs;
}

/* Written beside its final name and renamed, so a run that stops halfway
 * never leaves a short file to be loaded next time. */
static void cache_save(uint64_t key, const void *code, size_t n)
{
    char path[MAX_PATH], tmp[MAX_PATH + 16];
    FILE *f;
    int ok;

    cache_path(path, sizeof path, key);
    snprintf(tmp, sizeof tmp, "%s.%lu", path, GetCurrentThreadId());
    if (fopen_s(&f, tmp, "wb") || !f)
        return;
    ok = fwrite(code, 1, n, f) == n;
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

/* Loads every <key>.vso in the folder, once, beside the compile threads. */
static DWORD WINAPI cache_loader(LPVOID arg)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    uint32_t loaded = 0, bad = 0;
    double t0 = now_ms();

    (void)arg;
    snprintf(pattern, sizeof pattern, "%s\\*.vso", s_cache_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long key;
            ID3D11VertexShader *vs;
            char end;
            if (sscanf_s(fd.cFileName, "%16llX.vs%c", &key, &end, 1) != 2
                || end != 'o')
                continue;
            vs = cache_load_file((uint64_t)key);
            if (!vs) {
                bad++;
                continue;
            }
            EnterCriticalSection(&s_q_cs);
            disk_add((uint64_t)key, vs, 0);
            LeaveCriticalSection(&s_q_cs);
            loaded++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fprintf(stderr, "[VSHGPU] shader cache: %u loaded from %s in %.0f ms"
            "%s\n", loaded, s_cache_dir, now_ms() - t0,
            bad ? " (some files unreadable, compiled again when seen)" : "");
    return 0;
}

static void compile_draw(DrawJob *j)
{
    ID3DBlob *code = NULL, *err = NULL;
    ID3D11VertexShader *vs = NULL;
    double t0 = now_ms(), ms;
    HRESULT hr;

    /* The loader may not have reached this one yet: its file first. */
    if (cache_on() && (vs = cache_load_file(j->key)) != NULL) {
        EnterCriticalSection(&s_q_cs);
        vs = disk_add(j->key, vs, 0);
        LeaveCriticalSection(&s_q_cs);
        fprintf(stderr, "[VSHGPU] program %016llX: draw shader from the"
                " cache file in %.1f ms\n", (unsigned long long)j->p->hash,
                now_ms() - t0);
        free(j->src);
        EnterCriticalSection(&s_q_cs);
        j->p->draw_vs = vs;
        j->p->draw_state = DRAW_READY;
        s_dcompiled++;
        WakeAllConditionVariable(&s_q_done);
        LeaveCriticalSection(&s_q_cs);
        return;
    }

    /* Optimisation levels 0, 1 and 3 took the same time (tests/nv2a_vsh_gpu,
     * 400 programs, 2026-10-18): the cost is in the source, not the level. */
    hr = D3DCompile(j->src, j->len, "nv2a_vsh_draw", NULL, NULL, s_dentry,
                    VSH_DRAW_PROFILE, VSH_DRAW_FLAGS, 0, &code, &err);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(s_dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &vs);
    if (SUCCEEDED(hr) && cache_on()) {
        cache_save(j->key, ID3D10Blob_GetBufferPointer(code),
                   ID3D10Blob_GetBufferSize(code));
        EnterCriticalSection(&s_q_cs);
        vs = disk_add(j->key, vs, 0);
        LeaveCriticalSection(&s_q_cs);
    }
    ms = now_ms() - t0;
    if (FAILED(hr)) {
        fprintf(stderr, "[VSHGPU] program %016llX: draw shader failed"
                " (0x%08lX): %s\n", (unsigned long long)j->p->hash,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        vs = NULL;
    } else {
        fprintf(stderr, "[VSHGPU] program %016llX: %u instructions, draw"
                " shader compiled in %.1f ms\n", (unsigned long long)j->p->hash,
                j->p->count, ms);
    }
    if (s_dump > 0)
        fprintf(stderr, "%s\n", j->src);
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    free(j->src);

    EnterCriticalSection(&s_q_cs);
    j->p->draw_vs = vs;
    j->p->draw_state = vs ? DRAW_READY : DRAW_FAILED;
    if (vs) s_dcompiled++; else s_dfailed++;
    s_dms += ms;
    WakeAllConditionVariable(&s_q_done);
    LeaveCriticalSection(&s_q_cs);
}

void nv2a_vsh_gpu_draw_source(const char *head, const char *tail,
                              const char *entry)
{
    s_dhead = head;
    s_dtail = tail;
    s_dentry = entry;
}

/* head, the fetch of the inputs in `used`, the program, tail: into a buffer
 * of its own, for the worker, which frees it. NULL when the program does
 * not translate. */
static char *draw_source(const Nv2aVshIns *ins, uint32_t count, uint32_t used,
                         size_t *len)
{
    size_t cap = 160 * 1024, n, m;
    char *s = (char *)malloc(cap);

    if (!s)
        return NULL;
    n = strlen(s_dhead);
    if (n >= cap) { free(s); return NULL; }
    memcpy(s, s_dhead, n);
    m = nv2a_vsh_hlsl_fetch_only(s + n, cap - n);
    if (!m) { free(s); return NULL; }
    n += m;
    m = nv2a_vsh_hlsl_inputs(used, s + n, cap - n);
    if (!m) { free(s); return NULL; }
    n += m;
    m = nv2a_vsh_hlsl(ins, count, s + n, cap - n);
    if (!m) { free(s); return NULL; }
    n += m;
    m = strlen(s_dtail);
    if (n + m >= cap) { free(s); return NULL; }
    memcpy(s + n, s_dtail, m + 1);
    *len = n + m;
    return s;
}

/* One program found in memory: its drawing shader made and kept in the disk
 * cache (and its file) unless it is there already, exactly as the drawing
 * path would make it, so that path finds it by the same key. No VshProg:
 * the program may never be drawn. */
static void precompile_job(PreJob *pj)
{
    Nv2aVshIns ins[NV2A_VSH_SLOTS];
    ID3DBlob *code = NULL, *err = NULL;
    ID3D11VertexShader *vs = NULL;
    uint32_t count, used;
    uint64_t hash, key = 0;
    size_t len = 0;
    char *src = NULL;
    double t0 = now_ms(), ms = 0.0;
    int had = 0, loaded = 0;
    HRESULT hr = E_FAIL;

    count = nv2a_vsh_decode_words(pj->words, pj->count, ins, &used, &hash);
    if (count)
        src = draw_source(ins, count, used, &len);
    free(pj->words);
    if (src) {
        key = src_key(src, len);
        EnterCriticalSection(&s_q_cs);
        had = disk_find(key) != NULL;
        LeaveCriticalSection(&s_q_cs);
        if (!had && (vs = cache_load_file(key)) != NULL)
            loaded = 1;
        if (!had && !loaded) {
            hr = D3DCompile(src, len, "nv2a_vsh_draw", NULL, NULL, s_dentry,
                            VSH_DRAW_PROFILE, VSH_DRAW_FLAGS, 0, &code, &err);
            if (SUCCEEDED(hr))
                hr = ID3D11Device_CreateVertexShader(s_dev,
                        ID3D10Blob_GetBufferPointer(code),
                        ID3D10Blob_GetBufferSize(code), NULL, &vs);
            if (SUCCEEDED(hr))
                cache_save(key, ID3D10Blob_GetBufferPointer(code),
                           ID3D10Blob_GetBufferSize(code));
            else
                vs = NULL;
            ms = now_ms() - t0;
            if (vs)
                fprintf(stderr, "[VSHPRE] program %016llX: %u instructions,"
                        " pre-compiled in %.1f ms\n", (unsigned long long)hash,
                        count, ms);
            else
                fprintf(stderr, "[VSHPRE] program %016llX: draw shader failed"
                        " (0x%08lX): %s\n", (unsigned long long)hash,
                        (unsigned long)hr,
                        err ? (const char *)ID3D10Blob_GetBufferPointer(err)
                            : "");
        }
        free(src);
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);

    EnterCriticalSection(&s_q_cs);
    if (vs)
        disk_add(key, vs, 1);
    if (had) s_pre_had++;
    else if (loaded) s_pre_loaded++;
    else if (vs) s_pre_compiled++;
    else s_pre_failed++;
    s_pre_ms += ms;
    s_pre_busy--;
    /* Once all found so far are done: one line for the batch. */
    if (s_pq_head == s_pq_tail && !s_pre_busy) {
        uint32_t all = s_pre_compiled + s_pre_loaded + s_pre_had + s_pre_failed;
        if (all != s_pre_done_said) {
            s_pre_done_said = all;
            fprintf(stderr, "[VSHPRE] queue empty: %u compiled (%.0f ms on the"
                    " threads), %u from files, %u already cached, %u failed%s;"
                    " %u in the cache\n", s_pre_compiled, s_pre_ms,
                    s_pre_loaded, s_pre_had, s_pre_failed,
                    s_pre_dropped ? " (some dropped: queue full)" : "",
                    s_ndisk);
        }
    }
    LeaveCriticalSection(&s_q_cs);
}

static DWORD WINAPI draw_worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        DrawJob j;
        PreJob pj;
        EnterCriticalSection(&s_q_cs);
        while (s_q_head == s_q_tail && s_pq_head == s_pq_tail)
            SleepConditionVariableCS(&s_q_work, &s_q_cs, INFINITE);
        if (s_q_head != s_q_tail) {
            j = s_q[s_q_head++ % VSH_CACHE];
            LeaveCriticalSection(&s_q_cs);
            compile_draw(&j);
        } else {
            pj = s_pq[s_pq_head++ % VSH_PRE_MAX];
            s_pre_busy++;
            LeaveCriticalSection(&s_q_cs);
            precompile_job(&pj);
        }
    }
}

/* The queue's lock and the disk cache's setting, once, from whichever
 * thread comes first: the executor's first drawing shader or the scan
 * thread's first program. */
static BOOL CALLBACK q_init(PINIT_ONCE once, PVOID arg, PVOID *ctx)
{
    (void)once; (void)arg; (void)ctx;
    InitializeCriticalSection(&s_q_cs);
    InitializeConditionVariable(&s_q_work);
    InitializeConditionVariable(&s_q_done);
    if (s_dump < 0)
        s_dump = getenv("RECOMP_VSH_GPU_DUMP") != NULL;
    cache_on();
    return TRUE;
}

static int start_workers(void)
{
    const char *e;
    int want, i;

    if (s_workers)
        return s_workers > 0;
    InitOnceExecuteOnce(&s_q_once, q_init, NULL, NULL);
    EnterCriticalSection(&s_q_cs);
    if (s_workers) {
        LeaveCriticalSection(&s_q_cs);
        return s_workers > 0;
    }
    /* RECOMP_VSH_GPU_THREADS=<n>: compile threads. A level start or a burst
     * of new effects brings a few dozen new programs at once, and each
     * one's batches go the CPU way until it is ready. Default: half the
     * logical processors less two, 2 to 6 (6 on 16; 2 until 2026-10-20),
     * below normal priority so a burst does not take the executor's or the
     * title's time. */
    e = getenv("RECOMP_VSH_GPU_THREADS");
    want = e ? atoi(e)
             : (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) / 2 - 2;
    if (!e && want > 6) want = 6;
    if (want < (e ? 1 : 2)) want = e ? 1 : 2;
    if (want > 8) want = 8;
    for (i = 0; i < want; i++) {
        HANDLE h = CreateThread(NULL, 0, draw_worker, NULL, 0, NULL);
        if (!h)
            break;
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
    }
    s_workers = i ? i : -1;
    fprintf(stderr, "[VSHGPU] %d draw shader compile thread%s\n", i,
            i == 1 ? "" : "s");
    if (i && cache_on()) {
        HANDLE h = CreateThread(NULL, 0, cache_loader, NULL, 0, NULL);
        if (h) {
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(h);
        }
    }
    LeaveCriticalSection(&s_q_cs);
    return i > 0;
}

int nv2a_vsh_gpu_precompile(const uint32_t *words, uint32_t count)
{
    PreJob pj;
    int queued = 0;

    if (!count || count > NV2A_VSH_SLOTS)
        return 0;
    InitOnceExecuteOnce(&s_q_once, q_init, NULL, NULL);
    if (!cache_on())
        return 0;
    pj.words = (uint32_t *)malloc((size_t)count * 16);
    if (!pj.words)
        return 0;
    memcpy(pj.words, words, (size_t)count * 16);
    pj.count = count;
    EnterCriticalSection(&s_q_cs);
    if (s_pq_tail - s_pq_head < VSH_PRE_MAX) {
        s_pq[s_pq_tail++ % VSH_PRE_MAX] = pj;
        WakeConditionVariable(&s_q_work);
        queued = 1;
    } else if (!s_pre_dropped++) {
        fprintf(stderr, "[VSHPRE] queue full (%d): programs dropped\n",
                VSH_PRE_MAX);
    }
    LeaveCriticalSection(&s_q_cs);
    if (!queued)
        free(pj.words);
    /* The workers start with the first drawing shader; once the back end
     * has given its HLSL, the first program found may start them too. */
    else if (s_ready && s_dentry)
        start_workers();
    return queued;
}

void nv2a_vsh_gpu_precompile_stats(uint32_t *compiled, uint32_t *had,
                                   uint32_t *failed, uint32_t *waiting)
{
    InitOnceExecuteOnce(&s_q_once, q_init, NULL, NULL);
    EnterCriticalSection(&s_q_cs);
    *compiled = s_pre_compiled + s_pre_loaded;
    *had = s_pre_had;
    *failed = s_pre_failed;
    *waiting = s_pq_tail - s_pq_head + s_pre_busy;
    LeaveCriticalSection(&s_q_cs);
}

struct ID3D11VertexShader *nv2a_vsh_gpu_draw_shader(int wait)
{
    VshProg *p = s_cur;

    if (!s_ready || !p || p->failed || p == &s_spare || !s_dentry)
        return NULL;
    if (p->draw_state == DRAW_READY)
        return p->draw_vs;
    if (p->draw_state == DRAW_FAILED)
        return NULL;
    if (p->draw_state == DRAW_NONE) {
        const Nv2aVshIns *ins;
        uint32_t count, used = 0;
        uint64_t hash;
        DrawJob j;
        DiskShader *d;
        if (!start_workers()) {
            p->draw_state = DRAW_FAILED;
            return NULL;
        }
        ins = nv2a_vsh_decoded(&count, &hash);
        if (!ins || hash != p->hash || count != p->count
            || !nv2a_vsh_prepare(&used))
            return NULL;
        j.p = p;
        j.src = draw_source(ins, count, used, &j.len);
        if (!j.src) {
            fprintf(stderr, "[VSHGPU] program %016llX: %u instructions do not"
                    " translate\n", (unsigned long long)p->hash, count);
            p->failed = 1;
            return NULL;
        }
        j.key = src_key(j.src, j.len);
        /* RECOMP_VSH_SOURCE_LOG=1: where the program's first instruction came
         * in (push buffer address; the recorded buffer it was in, 0 = the
         * ring), once per program: which programs a title keeps in recorded
         * buffers, for shader pre-compilation. */
        {
            static int src_log = -1;
            if (src_log < 0)
                src_log = getenv("RECOMP_VSH_SOURCE_LOG") != NULL;
            if (src_log) {
                uint32_t va, buf;
                nv2a_vsh_start_source(&va, &buf);
                fprintf(stderr, "[VSHORIGIN] program %016llX: %u instructions"
                        " from %08X, buffer %08X\n",
                        (unsigned long long)p->hash, count, va, buf);
            }
        }
        if (nv2a_vsh_gpu_new_program_hook)
            nv2a_vsh_gpu_new_program_hook(p->hash);
        EnterCriticalSection(&s_q_cs);
        if (cache_on() && (d = disk_find(j.key)) != NULL) {
            /* Loaded at startup or pre-compiled: drawn on the GPU from its
             * first batch. */
            int pre = d->pre;
            p->draw_vs = d->vs;
            p->draw_state = DRAW_READY;
            s_dcompiled++;
            LeaveCriticalSection(&s_q_cs);
            free(j.src);
            fprintf(stderr, "[VSHGPU] program %016llX: draw shader from the"
                    " cache%s\n", (unsigned long long)p->hash,
                    pre ? " (pre-compiled)" : "");
            return p->draw_vs;
        }
        p->draw_state = DRAW_QUEUED;
        s_q[s_q_tail++ % VSH_CACHE] = j;
        WakeConditionVariable(&s_q_work);
        LeaveCriticalSection(&s_q_cs);
    }
    if (wait) {
        EnterCriticalSection(&s_q_cs);
        while (p->draw_state == DRAW_QUEUED)
            SleepConditionVariableCS(&s_q_done, &s_q_cs, INFINITE);
        LeaveCriticalSection(&s_q_cs);
    }
    return p->draw_state == DRAW_READY ? p->draw_vs : NULL;
}

void nv2a_vsh_gpu_draw_stats(uint32_t *compiled, uint32_t *failed,
                             uint32_t *queued, double *ms)
{
    *compiled = *failed = *queued = 0;
    *ms = 0.0;
    if (s_workers <= 0)
        return;
    EnterCriticalSection(&s_q_cs);
    *compiled = s_dcompiled;
    *failed = s_dfailed;
    *queued = s_q_tail - s_q_head;
    *ms = s_dms;
    LeaveCriticalSection(&s_q_cs);
}

/* ---- step 2: the inputs read on the GPU -------------------------------- */

typedef struct {
    uintptr_t s0, s1;                   /* source bytes [s0, s1) */
    uint32_t at;                        /* where in this batch's copy */
} Span;

int nv2a_vsh_gpu_inputs(const Nv2aVshArray a[NV2A_VSH_INPUTS], uint32_t used,
                        uint32_t lo, uint32_t hi)
{
    uint32_t rows[NV2A_VSH_INPUTS][4];
    uintptr_t start[NV2A_VSH_INPUTS];
    Span sp[NV2A_VSH_INPUTS], t;
    D3D11_MAPPED_SUBRESOURCE ms;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    uint32_t k, j, ns = 0, total = 0;

    if (!s_ready)
        return 0;
    memset(rows, 0, sizeof rows);
    /* Each array's bytes for indices lo..hi (to its last), sorted by where
     * they start; then neighbours that overlap or nearly touch are merged,
     * so an interleaved vertex is copied once and not once per attribute. */
    for (k = 0; k < NV2A_VSH_INPUTS; k++) {
        const Nv2aVshArray *x = &a[k];
        uint32_t bytes = nv2a_attr_bytes(x->type, x->size);
        uint32_t top = hi < x->last ? hi : x->last;
        uint64_t len;
        if (!(used >> k & 1) || !x->src || !x->size || !x->stride || !bytes
            || lo > top)
            continue;
        len = (uint64_t)(top - lo) * x->stride + bytes;
        if (len > VSH_RING_SIZE)
            return 0;
        start[k] = (uintptr_t)x->src + (uintptr_t)lo * x->stride;
        rows[k][1] = x->stride;
        rows[k][2] = NV2A_VSH_HLSL_FETCH_KIND(x->type) | (x->size << 8);
        rows[k][3] = x->last;
        t.s0 = start[k];
        t.s1 = start[k] + (uintptr_t)len;
        for (j = ns++; j > 0 && sp[j - 1].s0 > t.s0; j--)
            sp[j] = sp[j - 1];
        sp[j] = t;
    }
    for (k = 0, j = 0; k < ns; k++) {
        if (j && sp[k].s0 <= sp[j - 1].s1 + VSH_MERGE_GAP) {
            if (sp[k].s1 > sp[j - 1].s1)
                sp[j - 1].s1 = sp[k].s1;
        } else {
            sp[j++] = sp[k];
        }
    }
    ns = j;
    for (k = 0; k < ns; k++) {
        uint64_t len = sp[k].s1 - sp[k].s0;
        if ((uint64_t)total + len > VSH_RING_SIZE)
            return 0;
        sp[k].at = total;
        total += ((uint32_t)len + 15u) & ~15u;
    }

    if (ns) {
        if (!s_no_overwrite || s_ring_at + total > VSH_RING_SIZE) {
            if (s_no_overwrite)
                s_in_wraps++;
            how = D3D11_MAP_WRITE_DISCARD;
            s_ring_at = 0;
        }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ring, 0,
                how, 0, &ms)))
            return 0;
        for (k = 0; k < ns; k++)
            memcpy((uint8_t *)ms.pData + s_ring_at + sp[k].at,
                   (const void *)sp[k].s0, (size_t)(sp[k].s1 - sp[k].s0));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ring, 0);
        /* Element 0's address, modulo 2^32: the shader's d.x + i * d.y
         * lands on element i for every i from lo up. */
        for (k = 0; k < NV2A_VSH_INPUTS; k++) {
            if (!rows[k][2])
                continue;
            for (j = ns - 1; sp[j].s0 > start[k]; j--)
                ;
            rows[k][0] = s_ring_at + sp[j].at
                       + (uint32_t)(start[k] - sp[j].s0)
                       - lo * a[k].stride;
        }
        s_ring_at += total;
        s_in_bytes += total;
    }
    s_in_batches++;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_acb, 0,
            D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return 0;
    memcpy(ms.pData, rows, sizeof rows);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_acb, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx,
            NV2A_VSH_HLSL_FETCH_CBUFFER, 1, &s_acb);
    ID3D11DeviceContext_VSSetShaderResources(s_ctx,
            NV2A_VSH_HLSL_FETCH_SRV, 1, &s_ring_srv);
    return 1;
}

static int make_fetch_shader(void)
{
    static char src[16 * 1024];
    D3D11_SO_DECLARATION_ENTRY so[NV2A_VSH_INPUTS];
    UINT stride = VSH_FETCH_SIZE, i;
    ID3DBlob *code = NULL, *err = NULL;
    size_t len;
    HRESULT hr = E_FAIL;

    if (s_fetch_tried)
        return s_fetch_vs != NULL;
    s_fetch_tried = 1;
    for (i = 0; i < NV2A_VSH_INPUTS; i++) {
        so[i].Stream = 0;
        so[i].SemanticName = "TEXCOORD";
        so[i].SemanticIndex = i;
        so[i].StartComponent = 0;
        so[i].ComponentCount = 4;
        so[i].OutputSlot = 0;
    }
    len = nv2a_vsh_hlsl_fetch(src, sizeof src);
    if (len)
        hr = D3DCompile(src, len, "nv2a_vsh_fetch", NULL, NULL, "vs_fetch",
                        "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3
                        | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &err);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(s_dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), NULL, &s_fetch_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(s_dev,
                ID3D10Blob_GetBufferPointer(code),
                ID3D10Blob_GetBufferSize(code), so, NV2A_VSH_INPUTS, &stride,
                1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &s_fetch_gs);
    if (FAILED(hr)) {
        fprintf(stderr, "[VSHGPU] fetch shader failed (0x%08lX): %s\n",
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (s_fetch_vs) {
            ID3D11VertexShader_Release(s_fetch_vs);
            s_fetch_vs = NULL;
        }
    }
    if (code) ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    return s_fetch_vs != NULL;
}

int nv2a_vsh_gpu_fetch(const uint32_t *idx, uint32_t count,
                       float (*out)[NV2A_VSH_INPUTS][4])
{
    ID3D11Buffer *none = NULL;
    UINT zero = 0;
    uint32_t done = 0;

    if (!s_ready || !make_fetch_shader())
        return 0;
    while (done < count) {
        uint32_t m = count - done;
        D3D11_MAPPED_SUBRESOURCE ms;
        D3D11_BOX box;
        if (m > VSH_RUN_MAX)
            m = VSH_RUN_MAX;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &ms)))
            return 0;
        memcpy(ms.pData, idx + done, (size_t)m * 4);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);

        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx,
                D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib,
                DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_fetch_vs, NULL, 0);
        ID3D11DeviceContext_GSSetShader(s_ctx, s_fetch_gs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, NULL, NULL, 0);
        ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &s_fso, &zero);
        ID3D11DeviceContext_DrawIndexed(s_ctx, m, 0, 0);
        ID3D11DeviceContext_SOSetTargets(s_ctx, 1, &none, &zero);
        ID3D11DeviceContext_GSSetShader(s_ctx, NULL, NULL, 0);

        box.left = 0;
        box.right = m * VSH_FETCH_SIZE;
        box.top = box.front = 0;
        box.bottom = box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx,
                (ID3D11Resource *)s_fstage, 0, 0, 0, 0,
                (ID3D11Resource *)s_fso, 0, &box);
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_fstage,
                0, D3D11_MAP_READ, 0, &ms)))
            return 0;
        memcpy(&out[done], ms.pData, (size_t)m * VSH_FETCH_SIZE);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_fstage, 0);
        done += m;
    }
    return 1;
}

void nv2a_vsh_gpu_input_stats(uint64_t *batches, uint64_t *bytes,
                              uint64_t *wraps)
{
    *batches = s_in_batches;
    *bytes = s_in_bytes;
    *wraps = s_in_wraps;
}

#endif /* _WIN32 */
