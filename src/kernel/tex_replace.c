/**
 * Replacement textures: the PNG folder and the hash dump (tex_replace.h).
 *
 * The folder is listed once, on first use, and each PNG is decoded (WIC) the
 * first time its hash comes up, then kept: a texture the title evicts and
 * loads again goes back up without touching the file. Files added while the
 * game runs are not seen until the next start.
 * ponytail: decoded pixels are never freed; a pack bigger than memory would
 * need an LRU here.
 */
#include "tex_replace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <wincodec.h>

typedef struct {
    uint64_t hash;
    WCHAR    path[MAX_PATH];
    uint32_t *px;                       /* decoded on first find */
    uint32_t w, h;
    int      failed;                    /* could not be read: said once */
} Replacement;

static int          s_init;
static int          s_dump;             /* RECOMP_TEX_HASH_DUMP set */
static WCHAR        s_dump_dir[MAX_PATH];
static Replacement *s_rep;
static int          s_nrep;

#define DUMPED_SLOTS 65536              /* hashes dumped so far (0 = empty) */
static uint64_t    *s_dumped;
static int          s_ndumped;

static __declspec(thread) int t_com;
static IWICImagingFactory *s_wic;

static IWICImagingFactory *wic(void)
{
    if (!t_com) {
        /* S_FALSE or RPC_E_CHANGED_MODE: COM is already up on this thread,
         * which is all WIC needs. */
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        t_com = 1;
    }
    if (!s_wic && FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                          CLSCTX_INPROC_SERVER,
                                          &IID_IWICImagingFactory,
                                          (void **)&s_wic)))
        s_wic = NULL;
    return s_wic;
}

static void to_wide_dir(const char *s, WCHAR *out)
{
    size_t n;
    WCHAR *q;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, MAX_PATH);
    for (q = out; *q; q++)
        if (*q == L'/')
            *q = L'\\';
    n = wcslen(out);
    while (n > 1 && out[n - 1] == L'\\')
        out[--n] = L'\0';
}

/* The hash a file name starts with: 16 hex digits. 0 when it does not. */
static uint64_t name_hash(const WCHAR *name)
{
    uint64_t h = 0;
    int i;
    for (i = 0; i < 16; i++) {
        WCHAR c = name[i];
        int v = c >= L'0' && c <= L'9' ? c - L'0'
              : c >= L'a' && c <= L'f' ? c - L'a' + 10
              : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (v < 0)
            return 0;
        h = h << 4 | (uint64_t)v;
    }
    return h;
}

static int cmp_rep(const void *a, const void *b)
{
    uint64_t x = ((const Replacement *)a)->hash;
    uint64_t y = ((const Replacement *)b)->hash;
    return x < y ? -1 : x > y;
}

static void scan(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int cap = 0;

    swprintf_s(pat, MAX_PATH, L"%s\\*.png", dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[TEXREP] no replacement textures in %S\n", dir);
        return;
    }
    do {
        uint64_t h = name_hash(fd.cFileName);
        Replacement *r;
        if (!h || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (s_nrep == cap) {
            Replacement *n;
            cap = cap ? cap * 2 : 16;
            n = (Replacement *)realloc(s_rep, (size_t)cap * sizeof *n);
            if (!n)
                break;
            s_rep = n;
        }
        r = &s_rep[s_nrep++];
        memset(r, 0, sizeof *r);
        r->hash = h;
        swprintf_s(r->path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    qsort(s_rep, (size_t)s_nrep, sizeof *s_rep, cmp_rep);
    fprintf(stderr, "[TEXREP] %d replacement texture(s) in %S\n", s_nrep, dir);
}

static void init(void)
{
    const char *e;
    if (s_init)
        return;
    s_init = 1;
    e = getenv("RECOMP_TEX_REPLACE");
    if (e && *e) {
        WCHAR dir[MAX_PATH];
        to_wide_dir(e, dir);
        scan(dir);
    }
    e = getenv("RECOMP_TEX_HASH_DUMP");
    if (e && *e) {
        to_wide_dir(e, s_dump_dir);
        CreateDirectoryW(s_dump_dir, NULL);
        s_dumped = (uint64_t *)calloc(DUMPED_SLOTS, sizeof *s_dumped);
        s_dump = s_dumped != NULL;
        fprintf(stderr, "[TEXREP] dumping textures to %S\n", s_dump_dir);
    }
}

int tex_replace_active(void)
{
    init();
    return s_nrep > 0 || s_dump;
}

/* Decode r's PNG into B8G8R8A8 rows. 1 on success. */
static int decode(Replacement *r)
{
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapSource *conv = NULL;
    UINT w = 0, h = 0;
    HRESULT hr;

    if (!f)
        return 0;
    hr = IWICImagingFactory_CreateDecoderFromFilename(f, r->path, NULL,
            GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr))
        hr = IWICBitmapDecoder_GetFrame(dec, 0, &fr);
    if (SUCCEEDED(hr))
        hr = WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA,
                                    (IWICBitmapSource *)fr, &conv);
    if (SUCCEEDED(hr))
        hr = IWICBitmapSource_GetSize(conv, &w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > 16384 || h > 16384))
        hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        r->px = (uint32_t *)malloc((size_t)w * h * 4);
        hr = r->px ? IWICBitmapSource_CopyPixels(conv, NULL, w * 4,
                                                 w * h * 4, (BYTE *)r->px)
                   : E_OUTOFMEMORY;
    }
    if (conv) IWICBitmapSource_Release(conv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (FAILED(hr)) {
        free(r->px);
        r->px = NULL;
        return 0;
    }
    r->w = w;
    r->h = h;
    return 1;
}

const uint32_t *tex_replace_find(uint64_t hash, uint32_t *w, uint32_t *h)
{
    int lo = 0, hi;
    Replacement *r = NULL;

    init();
    hi = s_nrep - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_rep[mid].hash == hash) {
            r = &s_rep[mid];
            break;
        }
        if (s_rep[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (!r || r->failed)
        return NULL;
    if (!r->px) {
        if (!decode(r)) {
            r->failed = 1;
            fprintf(stderr, "[TEXREP] could not read %S\n", r->path);
            return NULL;
        }
        fprintf(stderr, "[TEXREP] %016llX -> %S (%ux%u)\n",
                (unsigned long long)hash, r->path, r->w, r->h);
    }
    *w = r->w;
    *h = r->h;
    return r->px;
}

int tex_replace_dumping(void)
{
    init();
    return s_dump;
}

int tex_replace_dump_new(uint64_t hash)
{
    uint32_t i;
    if (!s_dump || !hash || s_ndumped >= DUMPED_SLOTS / 2)
        return 0;
    for (i = (uint32_t)(hash * 0x9E3779B97F4A7C15ull >> 48);
         s_dumped[i]; i = (i + 1) & (DUMPED_SLOTS - 1))
        if (s_dumped[i] == hash)
            return 0;
    s_dumped[i] = hash;
    s_ndumped++;
    return 1;
}

void tex_replace_dump(uint64_t hash, const uint32_t *px, uint32_t w,
                      uint32_t h, uint32_t fmt)
{
    IWICImagingFactory *f = wic();
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    WCHAR path[MAX_PATH];
    HRESULT hr;

    if (!f)
        return;
    swprintf_s(path, MAX_PATH, L"%s\\%016llX_%ux%u_f%02X.png", s_dump_dir,
               (unsigned long long)hash, w, h, fmt);
    hr = IWICImagingFactory_CreateStream(f, &st);
    if (SUCCEEDED(hr))
        hr = IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng,
                                              NULL, &enc);
    if (SUCCEEDED(hr))
        hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st,
                                          WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_Initialize(fr, NULL);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_SetSize(fr, w, h);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
    if (SUCCEEDED(hr) && memcmp(&pf, &GUID_WICPixelFormat32bppBGRA,
                                sizeof pf))
        hr = E_FAIL;                    /* PNG always takes 32bppBGRA */
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_WritePixels(fr, h, w * 4, w * h * 4,
                                               (BYTE *)px);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr))
        hr = IWICBitmapEncoder_Commit(enc);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (FAILED(hr))
        fprintf(stderr, "[TEXREP] could not write %S (0x%08lX)\n", path,
                (unsigned long)hr);
}

#else

int tex_replace_active(void) { return 0; }
const uint32_t *tex_replace_find(uint64_t hash, uint32_t *w, uint32_t *h)
{
    (void)hash; (void)w; (void)h;
    return NULL;
}
int tex_replace_dumping(void) { return 0; }
int tex_replace_dump_new(uint64_t hash) { (void)hash; return 0; }
void tex_replace_dump(uint64_t hash, const uint32_t *px, uint32_t w,
                      uint32_t h, uint32_t fmt)
{
    (void)hash; (void)px; (void)w; (void)h; (void)fmt;
}

#endif
