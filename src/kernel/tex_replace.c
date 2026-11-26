/**
 * Replacement textures: the PNG folder, the pack folders and the hash dump
 * (tex_replace.h).
 *
 * The folders are listed once, on first use: the packs (each with its
 * subfolders, named by the title's namer or by hash) and the base folder
 * (its PNGs right in it, by hash), every hash kept from the highest one
 * (the first pack; the base folder last). Each PNG is decoded (WIC) the
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
    int      prio;                      /* 0 = the first pack; the base folder last */
    int      seq;                       /* found order: the first wins in one folder */
    WCHAR    path[MAX_PATH];
    uint32_t *px;                       /* decoded on first find */
    uint32_t w, h;
    int      failed;                    /* could not be read: said once */
} Replacement;

static int          s_init;
static int          s_dump;             /* RECOMP_TEX_HASH_DUMP set */
static WCHAR        s_dump_dir[MAX_PATH];
static Replacement *s_rep;
static int          s_nrep, s_caprep;

#define MAX_PACKS 64
static WCHAR          *s_pack[MAX_PACKS];   /* tex_replace_set_packs, in order */
static int             s_npack;
static TexReplaceNamer s_namer;
static void           *s_namer_ctx;

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

/* By hash, then the highest folder, then found order. */
static int cmp_rep(const void *a, const void *b)
{
    const Replacement *x = (const Replacement *)a, *y = (const Replacement *)b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    if (x->prio != y->prio)
        return x->prio < y->prio ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static Replacement *add_rep(uint64_t hash, int prio, const WCHAR *path)
{
    Replacement *r;
    if (s_nrep == s_caprep) {
        int cap = s_caprep ? s_caprep * 2 : 64;
        Replacement *n = (Replacement *)realloc(s_rep, (size_t)cap * sizeof *n);
        if (!n)
            return NULL;
        s_rep = n;
        s_caprep = cap;
    }
    r = &s_rep[s_nrep];
    memset(r, 0, sizeof *r);
    r->hash = hash;
    r->prio = prio;
    r->seq = s_nrep++;
    wcscpy_s(r->path, MAX_PATH, path);
    return r;
}

/* The base folder: its PNGs right in it, named by hash. */
static void scan(const WCHAR *dir, int prio)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0;

    swprintf_s(pat, MAX_PATH, L"%s\\*.png", dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[TEXREP] no replacement textures in %S\n", dir);
        return;
    }
    do {
        uint64_t h = name_hash(fd.cFileName);
        if (!h || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName) < 0)
            continue;
        if (!add_rep(h, prio, path))
            break;
        n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    fprintf(stderr, "[TEXREP] %d replacement texture(s) in %S\n", n, dir);
}

typedef struct {
    int   found, unmatched, too_long;
    WCHAR first_unmatched[MAX_PATH];
} PackCount;

static int is_png(const WCHAR *name)
{
    size_t n = wcslen(name);
    return n > 4 && !_wcsicmp(name + n - 4, L".png");
}

/* One pack: every PNG below root (rel = the folder inside it, "" at the
 * top), by the namer, else by the hash its name starts with. */
static void scan_pack(const WCHAR *root, const WCHAR *rel, int prio, int depth,
                      PackCount *c)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;

    if (depth > 16)                     /* a folder link round in a loop */
        return;
    if (_snwprintf_s(pat, MAX_PATH, _TRUNCATE, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*",
                     root, rel) < 0)
        return;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        uint64_t h = 0;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (_snwprintf_s(sub, MAX_PATH, _TRUNCATE, rel[0] ? L"%s\\%s" : L"%s%s",
                         rel, fd.cFileName) < 0
            || _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", root, sub) < 0) {
            c->too_long++;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_pack(root, sub, prio, depth + 1, c);
            continue;
        }
        if (!is_png(fd.cFileName))
            continue;
        if (s_namer) {
            char u[MAX_PATH * 3];
            if (WideCharToMultiByte(CP_UTF8, 0, sub, -1, u, (int)sizeof u, NULL, NULL))
                h = s_namer(u, s_namer_ctx);
        }
        if (!h)
            h = name_hash(fd.cFileName);
        if (!h) {
            if (!c->unmatched++)
                wcscpy_s(c->first_unmatched, MAX_PATH, sub);
            continue;
        }
        if (add_rep(h, prio, full))
            c->found++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

/* Sorted for tex_replace_find, each hash once: the highest folder's. */
static void finish(void)
{
    int i, j, hidden = 0;
    if (!s_nrep)
        return;
    qsort(s_rep, (size_t)s_nrep, sizeof *s_rep, cmp_rep);
    for (i = j = 0; i < s_nrep; i++) {
        if (j && s_rep[j - 1].hash == s_rep[i].hash) {
            hidden++;
            continue;
        }
        if (i != j)
            s_rep[j] = s_rep[i];
        j++;
    }
    s_nrep = j;
    if (s_npack)
        fprintf(stderr, "[TEXREP] %d texture(s) replaced; %d PNG(s) left out for one "
                "higher up that replaces the same texture\n", s_nrep, hidden);
}

void tex_replace_set_namer(TexReplaceNamer namer, void *ctx)
{
    s_namer = namer;
    s_namer_ctx = ctx;
}

int tex_replace_set_packs(const char *const *dirs, int n)
{
    int i;
    if (s_init)
        return 0;
    for (i = 0; i < s_npack; i++)
        free(s_pack[i]);
    s_npack = 0;
    for (i = 0; i < n && s_npack < MAX_PACKS; i++) {
        WCHAR *w = (WCHAR *)malloc(MAX_PATH * sizeof *w);
        if (!w)
            break;
        to_wide_dir(dirs[i], w);
        s_pack[s_npack++] = w;
    }
    return 1;
}

static void init(void)
{
    const char *e;
    int i;
    if (s_init)
        return;
    s_init = 1;
    for (i = 0; i < s_npack; i++) {
        DWORD a = GetFileAttributesW(s_pack[i]);
        PackCount c;
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            fprintf(stderr, "[TEXREP] pack %S: no such folder\n", s_pack[i]);
            continue;
        }
        memset(&c, 0, sizeof c);
        scan_pack(s_pack[i], L"", i, 0, &c);
        fprintf(stderr, "[TEXREP] pack %S: %d replacement texture(s)", s_pack[i], c.found);
        if (c.unmatched)
            fprintf(stderr, "; %d PNG(s) named after no texture (the first: %S)",
                    c.unmatched, c.first_unmatched);
        if (c.too_long)
            fprintf(stderr, "; %d path(s) too long", c.too_long);
        fputc('\n', stderr);
    }
    e = getenv("RECOMP_TEX_REPLACE");
    if (e && *e) {
        WCHAR dir[MAX_PATH];
        to_wide_dir(e, dir);
        scan(dir, s_npack);             /* underneath every pack */
    }
    finish();
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

void tex_replace_set_namer(TexReplaceNamer namer, void *ctx)
{
    (void)namer; (void)ctx;
}
int tex_replace_set_packs(const char *const *dirs, int n)
{
    (void)dirs; (void)n;
    return 1;
}
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
