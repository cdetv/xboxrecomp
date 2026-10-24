/*
 * Shader pre-compilation: scanning guest RAM for vertex programs, counting
 * what a scan caught (nv2a_vsh_scan.h, RECOMP_VSH_SCAN) and handing each new
 * one to the compile threads (RECOMP_VSH_PRECOMPILE, nv2a_vsh_gpu_precompile).
 *
 * The scan runs on a thread of its own, below normal priority, so a request
 * from the game thread costs it nothing. Every program found goes into one
 * table by hash, with the number of the first scan that saw it; the drawing
 * path (nv2a_vsh_gpu_new_program_hook) marks programs drawn and prints
 * whether a scan had found them first. That is the coverage a pre-compiler
 * built on these scans would get.
 */
#if defined(_WIN32)
#include "nv2a_vsh_scan.h"
#include "nv2a_vsh_gpu.h"
#include "guest_vmem.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern size_t g_xbox_total_ram;

#define CONTIG_BASE 0x80000000u
#define SET_SIZE    32768u               /* a power of two */
#define WHY_MAX     8

typedef struct {
    uint64_t hash;                       /* 0 = empty */
    uint32_t first_scan, last_scan;      /* scan numbers, from 1; 0 = none */
    uint32_t va;                         /* where the first scan found it */
    uint8_t  kind, drawn;
} Entry;

static Entry s_set[SET_SIZE];            /* under s_cs */
static uint32_t s_nset;
static CRITICAL_SECTION s_cs;
static CONDITION_VARIABLE s_cv;
static char s_why[WHY_MAX][64];          /* requests waiting, under s_cs */
static uint32_t s_why_head, s_why_tail;
static uint32_t s_scans;                 /* scans done */
static char s_last_why[WHY_MAX * 8][64]; /* why of scan n, n % size */
static int s_on = -1;
static int s_count, s_pre;               /* RECOMP_VSH_SCAN, _PRECOMPILE */

/* The ranges scanned, counted apart to see what each adds: the contiguous
 * window (all of RAM, as the GPU sees it), the low view of RAM from
 * 0x10000 (the same pages through the title's own addresses), and what the
 * extended-VMA tracker has committed above the mirrors (a CRT heap grown
 * there), copied under its lock (guest_vmem_read) since a free on another
 * thread may decommit it. */
enum { R_CONTIG, R_LOW, R_EXT, R_COUNT };
static const char *const k_range[R_COUNT] = { "contig", "low", "ext" };

typedef struct {
    uint32_t scan, found, pb, bin, distinct, fresh, drawn, queued;
    uint32_t range, limit;               /* programs starting at limit+ are the next chunk's */
    uint32_t r_found[R_COUNT], r_fresh[R_COUNT], r_mb[R_COUNT];
} ScanCtx;

/* Callers hold s_cs. The entry for hash, made empty if absent; NULL when
 * the table is full. */
static Entry *lookup(uint64_t hash)
{
    uint32_t i = (uint32_t)(hash ^ (hash >> 32)) & (SET_SIZE - 1);
    while (s_set[i].hash && s_set[i].hash != hash)
        i = (i + 1) & (SET_SIZE - 1);
    if (!s_set[i].hash) {
        if (s_nset >= SET_SIZE * 3 / 4)
            return NULL;
        s_set[i].hash = hash;
        s_nset++;
    }
    return &s_set[i];
}

static void on_found(void *ctx, uint32_t va, int kind, const uint32_t *ins,
                     uint32_t count)
{
    ScanCtx *c = (ScanCtx *)ctx;
    uint64_t h;
    Entry *e;
    int fresh = 0;

    if (va >= c->limit)
        return;
    h = nv2a_vsh_scan_hash(ins, count);
    c->found++;
    c->r_found[c->range]++;
    if (kind == NV2A_VSH_SCAN_PB) c->pb++; else c->bin++;
    EnterCriticalSection(&s_cs);
    e = lookup(h);
    if (e && e->last_scan != c->scan) {
        c->distinct++;
        if (!e->first_scan) {
            e->first_scan = c->scan;
            e->va = va;
            e->kind = (uint8_t)kind;
            c->fresh++;
            c->r_fresh[c->range]++;
            fresh = 1;
        }
        if (e->drawn)
            c->drawn++;
        e->last_scan = c->scan;
    }
    LeaveCriticalSection(&s_cs);
    /* The first time any scan sees a program, drawn already or not: one
     * drawn before the scan is in the cache and skipped there. */
    if (fresh && s_pre)
        c->queued += (uint32_t)nv2a_vsh_gpu_precompile(ins, count);
}

/* [lo, hi) read in place, run by run of committed readable host pages (the
 * RAM views are mapped for good; a hole is skipped, never read: the game's
 * exception handler takes any fault for a crash). */
static void scan_direct(ScanCtx *c, int range, uint32_t lo, uint32_t hi)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t at = lo, bytes = 0;

    c->range = (uint32_t)range;
    while (at < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        uint64_t end;
        DWORD p;
        if (!VirtualQuery(mem + at, &mbi, sizeof mbi))
            break;
        end = (uint64_t)((const uint8_t *)mbi.BaseAddress + mbi.RegionSize - mem);
        if (end > hi)
            end = hi;
        p = mbi.Protect & 0xFFu;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            && (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY
                || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE
                || p == PAGE_EXECUTE_WRITECOPY)) {
            c->limit = (uint32_t)end;
            nv2a_vsh_scan(mem, (uint32_t)at, (uint32_t)end, on_found, c);
            bytes += end - at;
        }
        at = end;
    }
    c->r_mb[range] += (uint32_t)(bytes >> 20);
}

/* In 1 MB copies with room for a whole program past each, so one that
 * starts near a copy's end is still read whole (and counted once). */
#define EXT_CHUNK   (1u << 20)
#define EXT_OVER    0x1000u                  /* > 136 instructions + headers */
static uint8_t s_ext_buf[EXT_CHUNK + EXT_OVER];

static void scan_ext(ScanCtx *c)
{
    uint32_t from = 0, base, size, at, got;
    uint64_t bytes = 0;

    c->range = R_EXT;
    while (guest_vmem_next_committed(from, &base, &size)) {
        for (at = base; at - base < size; at += EXT_CHUNK) {
            uint32_t want = size - (at - base);
            want = want > EXT_CHUNK ? EXT_CHUNK + EXT_OVER : want;
            got = guest_vmem_read(at, s_ext_buf, want);
            if (!got)
                break;
            c->limit = at + (got < EXT_CHUNK ? got : EXT_CHUNK);
            nv2a_vsh_scan(s_ext_buf - at, at, at + got, on_found, c);
            bytes += got < EXT_CHUNK ? got : EXT_CHUNK;
        }
        if ((uint64_t)base + size >= 0xFFFFFFFFull)
            break;
        from = base + size;
    }
    c->r_mb[R_EXT] = (uint32_t)(bytes >> 20);
}

static DWORD WINAPI scan_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        char why[64];
        ScanCtx c;
        LARGE_INTEGER f, t0, t1;

        EnterCriticalSection(&s_cs);
        while (s_why_head == s_why_tail)
            SleepConditionVariableCS(&s_cv, &s_cs, INFINITE);
        memcpy(why, s_why[s_why_head++ % WHY_MAX], sizeof why);
        c.scan = ++s_scans;
        memcpy(s_last_why[c.scan % (WHY_MAX * 8)], why, sizeof why);
        LeaveCriticalSection(&s_cs);

        memset(&c.found, 0, sizeof c - offsetof(ScanCtx, found));
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        scan_direct(&c, R_CONTIG, CONTIG_BASE,
                    CONTIG_BASE + (uint32_t)g_xbox_total_ram);
        scan_direct(&c, R_LOW, 0x00010000u, (uint32_t)g_xbox_total_ram);
        scan_ext(&c);
        QueryPerformanceCounter(&t1);
        fprintf(stderr, "[VSHSCAN] scan #%u (%s): %u programs (%u in push"
                " buffers, %u binaries), %u distinct, %u new (%u queued to"
                " compile), %u already drawn;"
                " %.0f ms; %u known | %s %u/%u new (%u MB), %s %u/%u (%u MB),"
                " %s %u/%u (%u MB)\n", c.scan, why, c.found, c.pb, c.bin,
                c.distinct, c.fresh, c.queued, c.drawn,
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
                s_nset, k_range[0], c.r_found[0], c.r_fresh[0], c.r_mb[0],
                k_range[1], c.r_found[1], c.r_fresh[1], c.r_mb[1],
                k_range[2], c.r_found[2], c.r_fresh[2], c.r_mb[2]);
    }
}

static void on_new_program(uint64_t hash)
{
    Entry *e;
    char why[64] = "";
    uint32_t first = 0, va = 0, kind = 0;

    EnterCriticalSection(&s_cs);
    e = lookup(hash);
    if (e && !e->drawn) {
        e->drawn = 1;
        first = e->first_scan;
        va = e->va;
        kind = e->kind;
        if (first)
            memcpy(why, s_last_why[first % (WHY_MAX * 8)], sizeof why);
    } else
        e = NULL;
    LeaveCriticalSection(&s_cs);
    if (!e)
        return;
    if (first)
        fprintf(stderr, "[VSHSCAN] program %016llX drawn: found by scan #%u"
                " (%s) at %08X, %s\n", (unsigned long long)hash, first, why, va,
                kind == NV2A_VSH_SCAN_PB ? "push buffer" : "binary");
    else
        fprintf(stderr, "[VSHSCAN] program %016llX drawn: not found by any"
                " scan\n", (unsigned long long)hash);
}

void nv2a_vsh_prescan_request(const char *why)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_VSH_SCAN");
        const char *p = getenv("RECOMP_VSH_PRECOMPILE");
        HANDLE h;
        s_count = e && e[0] && strcmp(e, "0") != 0;
        s_pre = p && p[0] && strcmp(p, "0") != 0;
        s_on = s_count || s_pre;
        if (!s_on)
            return;
        InitializeCriticalSection(&s_cs);
        InitializeConditionVariable(&s_cv);
        if (s_count)
            nv2a_vsh_gpu_new_program_hook = on_new_program;
        h = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
        if (!h) {
            s_on = 0;
            return;
        }
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
        fprintf(stderr, "[VSHSCAN] scanning %u MB of guest RAM per request"
                " (%s)\n", (unsigned)(g_xbox_total_ram >> 20),
                s_pre ? "programs found are pre-compiled" : "count only");
    }
    if (!s_on)
        return;
    EnterCriticalSection(&s_cs);
    if (s_why_tail - s_why_head < WHY_MAX) {
        snprintf(s_why[s_why_tail % WHY_MAX], sizeof s_why[0], "%s",
                 why ? why : "");
        s_why_tail++;
        WakeConditionVariable(&s_cv);
    }
    LeaveCriticalSection(&s_cs);
}

#endif /* _WIN32 */
