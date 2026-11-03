/**
 * VSync for the D3D11 back end (RECOMP_VSYNC=on|off|vrr, xbox_SetVsync).
 *
 * A title paces itself on the vblank the kernel emulates (kernel_bridge.c,
 * a 60 Hz clock of its own), so the frames it presents follow that clock,
 * not the monitor's. Unset, every frame is presented at once without
 * tearing, as before: Windows shows the newest at each refresh, and on a
 * 60 Hz monitor the two clocks drift, so now and then a frame shows twice
 * and the next is skipped. The modes:
 *
 *   on   no tearing, every frame waits for a refresh (sync interval 1), and
 *        a watcher thread here hands the monitor's real refreshes to the
 *        vblank clock (xbox_VblankFromDisplay) whenever the monitor runs at
 *        about a multiple of 60 Hz (60, 120, 180, 240, within LOCK_TOL):
 *        one emulated vblank per k refreshes. Each frame then lands on its
 *        own refresh. Other rates (144, 165) keep the kernel's own clock.
 *   off  presented at once, allowed to tear: the lowest delay.
 *   vrr  as off: Windows needs the "may tear" present for G-Sync /
 *        FreeSync in a window (borderless full screen included). Kept
 *        apart for frame rates above 60, which it is to cap under the
 *        monitor's maximum.
 *
 * The watcher waits on the output the window is on (IDXGIOutput::
 * WaitForVBlank) and measures the refresh: the median of the first
 * FIRST_N intervals, then the median of each MEASURE_N (a late wake that
 * spans r refreshes counts as r), each taken only near the rate Windows
 * gives for the display mode. The kernel falls back to its own clock when
 * the refreshes stop coming (kernel_vblank_tick).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <dxgi1_5.h>
#include "vsync_d3d11.h"

extern void xbox_VblankDisplayLock(int on);
extern void xbox_VblankFromDisplay(void);

enum { VS_UNSET = -1, VS_OFF, VS_ON, VS_VRR, VS_MODES };
static const char *const s_mode_names[VS_MODES] = { "off", "on", "vrr" };

#define VS_UNREAD  (-2)
#define LOCK_TOL   0.015       /* k refreshes within 1.5 % of 1/60 s */
#define LOCK_MAX_K 4           /* up to 240 Hz */
#define FIRST_N    15          /* intervals for the first (median) estimate */
#define MEASURE_N  60          /* intervals per later estimate */

static volatile LONG s_mode = VS_UNREAD;
static int s_tearing;          /* the swap chain may tear */
static int s_started;          /* vsync_swap_flags ran */

/* The output the window is on, for the watcher: set by vsync_present on
 * the executor thread, taken (with a reference) by the watcher. */
static CRITICAL_SECTION s_out_lock;
static IDXGIOutput *s_out;
static HMONITOR s_out_mon;
static HANDLE s_wake;          /* mode changed, or an output to wait on */
static HANDLE s_watcher;

static int vsync_mode(void)
{
    LONG m = s_mode;

    if (m == VS_UNREAD) {
        const char *e = getenv("RECOMP_VSYNC");
        LONG v = VS_UNSET;
        if (e && *e) {
            for (int i = 0; i < VS_MODES; i++)
                if (!_stricmp(e, s_mode_names[i]))
                    v = i;
            if (v == VS_UNSET)
                fprintf(stderr, "[VSYNC] RECOMP_VSYNC=%s ignored: want on,"
                        " off or vrr%c", e, 10);
        }
        /* A change made before the first read wins over the environment. */
        InterlockedCompareExchange(&s_mode, v, VS_UNREAD);
        m = s_mode;
    }
    return (int)m;
}

/* For a settings menu: 0 = off, 1 = on, 2 = vrr. Applies from the next
 * present. 1 when taken. */
int xbox_SetVsync(int mode)
{
    if (mode < 0 || mode >= VS_MODES)
        return 0;
    vsync_mode();
    if (InterlockedExchange(&s_mode, mode) != mode) {
        fprintf(stderr, "[VSYNC] %s%c", s_mode_names[mode], 10);
        if (s_wake)
            SetEvent(s_wake);
    }
    return 1;
}

static void lock_say(int locked, double ms, int k)
{
    if (locked)
        fprintf(stderr, "[VSYNC] monitor %.2f Hz: game clock follows it"
                " (one vblank every %d refresh%s)%c", 1000.0 / ms, k,
                k == 1 ? "" : "es", 10);
    else if (ms > 0)
        fprintf(stderr, "[VSYNC] monitor %.2f Hz: not a multiple of 60, game"
                " clock stays on its own 60 Hz%c", 1000.0 / ms, 10);
    else
        fprintf(stderr, "[VSYNC] game clock back on its own 60 Hz%c", 10);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The refresh rate Windows says the output's display mode has, in Hz; 0
 * when it does not say. Whole numbers only: 59 for a 59.94 Hz mode. */
static double mode_hz(IDXGIOutput *out)
{
    DXGI_OUTPUT_DESC d;
    DEVMODEW dm;

    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (FAILED(IDXGIOutput_GetDesc(out, &d))
            || !EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &dm)
            || dm.dmDisplayFrequency <= 1)
        return 0;
    return (double)dm.dmDisplayFrequency;
}

/* A measured period (ms) is taken only near the mode's rate: a loading
 * screen that keeps the watcher from waking on time skews a whole window
 * of intervals, which once read as 48 Hz on a 60 Hz monitor. */
static int period_plausible(double ms, double hz)
{
    return !hz || fabs(1000.0 / ms - hz) / hz < 0.03;
}

static DWORD WINAPI watcher_main(void *arg)
{
    LARGE_INTEGER f, now;
    long long last = 0;
    double first[FIRST_N], window[MEASURE_N], period = 0, hz = 0;
    int nfirst = 0, nsum = 0, locked = 0, k = 1, phase = 0, said_k = -1;
    IDXGIOutput *seen = NULL;      /* the output measured; compared only */

    (void)arg;
    QueryPerformanceFrequency(&f);
    for (;;) {
        IDXGIOutput *out;
        HRESULT hr;

        if (vsync_mode() != VS_ON) {
            if (locked) {
                xbox_VblankDisplayLock(0);
                lock_say(0, 0, 0);
            }
            locked = 0;
            said_k = -1;
            last = 0;
            period = 0;
            nfirst = nsum = 0;
            seen = NULL;
            WaitForSingleObject(s_wake, INFINITE);
            continue;
        }
        EnterCriticalSection(&s_out_lock);
        out = s_out;
        if (out)
            IDXGIOutput_AddRef(out);
        LeaveCriticalSection(&s_out_lock);
        if (!out) {
            WaitForSingleObject(s_wake, 100);
            continue;
        }
        if (out != seen) {
            /* Another monitor: measure afresh. The lock stays until the
             * new one is known; one wrong frame at most. */
            seen = out;
            hz = mode_hz(out);
            last = 0;
            period = 0;
            nfirst = nsum = 0;
        }
        hr = IDXGIOutput_WaitForVBlank(out);
        IDXGIOutput_Release(out);
        QueryPerformanceCounter(&now);
        if (FAILED(hr)) {
            /* The kernel's own clock carries on meanwhile. */
            last = 0;
            Sleep(16);
            continue;
        }
        if (last) {
            double ms = (double)(now.QuadPart - last) * 1000.0
                      / (double)f.QuadPart;
            int r = 1;

            if (period <= 0) {
                if (ms >= 2.0 && ms <= 40.0)
                    first[nfirst++] = ms;
                if (nfirst == FIRST_N) {
                    qsort(first, FIRST_N, sizeof first[0], cmp_double);
                    if (period_plausible(first[FIRST_N / 2], hz))
                        period = first[FIRST_N / 2];
                    nfirst = 0;
                }
            } else {
                r = (int)floor(ms / period + 0.5);
                if (r < 1)
                    r = 1;
                if (r <= 8)
                    window[nsum++] = ms / r;
                if (nsum == MEASURE_N) {
                    qsort(window, MEASURE_N, sizeof window[0], cmp_double);
                    if (period_plausible(window[MEASURE_N / 2], hz))
                        period = window[MEASURE_N / 2];
                    nsum = 0;
                }
            }
            if (period > 0) {
                int nk = (int)floor(1000.0 / 60.0 / period + 0.5);
                int ok = nk >= 1 && nk <= LOCK_MAX_K
                      && fabs(nk * period - 1000.0 / 60.0) / (1000.0 / 60.0)
                         < LOCK_TOL;
                if (ok != locked || (ok && nk != k)) {
                    xbox_VblankDisplayLock(ok);
                    locked = ok;
                    k = ok ? nk : 1;
                    phase = 0;
                }
                if (said_k != (locked ? k : 0)) {
                    said_k = locked ? k : 0;
                    lock_say(locked, period, k);
                }
            }
            if (locked) {
                for (phase += r; phase >= k; phase -= k)
                    xbox_VblankFromDisplay();
            }
        }
        last = now.QuadPart;
    }
}

UINT vsync_swap_flags(IDXGIFactory2 *factory)
{
    IDXGIFactory5 *f5 = NULL;
    BOOL ok = FALSE;
    int m;

    if (!s_started) {
        InitializeCriticalSection(&s_out_lock);
        s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_started = 1;
    }
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5,
                                               (void **)&f5))) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5,
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &ok, sizeof ok)))
            ok = FALSE;
        IDXGIFactory5_Release(f5);
    }
    s_tearing = ok != FALSE;
    m = vsync_mode();
    fprintf(stderr, "[VSYNC] %s; tearing (off, vrr) %s%c",
            m == VS_UNSET ? "unset (no wait, no tearing)" : s_mode_names[m],
            s_tearing ? "supported" : "not supported: they present without",
            10);
    return s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
}

/* VSync on: keep the watcher's output the one the window is on (checked
 * every present: MonitorFromWindow is cheap, GetContainingOutput only when
 * the monitor changed), and start the watcher the first time. */
static void follow_output(IDXGISwapChain1 *swap)
{
    HWND hwnd = NULL;
    HMONITOR mon;
    IDXGIOutput *out = NULL, *old;

    if (FAILED(IDXGISwapChain1_GetHwnd(swap, &hwnd)) || !hwnd)
        return;
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon != s_out_mon) {
        if (FAILED(IDXGISwapChain1_GetContainingOutput(swap, &out)))
            out = NULL;
        EnterCriticalSection(&s_out_lock);
        old = s_out;
        s_out = out;
        s_out_mon = out ? mon : NULL;
        LeaveCriticalSection(&s_out_lock);
        if (old)
            IDXGIOutput_Release(old);
        SetEvent(s_wake);
    }
    if (!s_watcher) {
        s_watcher = CreateThread(NULL, 0, watcher_main, NULL, 0, NULL);
        if (s_watcher)
            SetThreadPriority(s_watcher, THREAD_PRIORITY_HIGHEST);
    }
}

/* RECOMP_VSYNC_LOG=1: what the monitor showed, from DXGI's frame
 * statistics, every 10 s: of the frames presented, how many were shown,
 * and how many refreshes each shown frame stayed up (the step from the one
 * before). A steady 60 fps on a 60 Hz monitor is all 1s, 30 fps all 2s; a
 * frame shown twice and the next skipped is a 2 and a dropped frame. */
static void stats_note(IDXGISwapChain1 *swap)
{
    static int on = -1;
    static UINT last_count, last_refresh, presents, shown, dropped, steps[4];
    static ULONGLONG next_ms;
    DXGI_FRAME_STATISTICS st;
    ULONGLONG now;

    if (on < 0)
        on = getenv("RECOMP_VSYNC_LOG") != NULL;
    if (!on)
        return;
    presents++;
    if (SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(swap, &st))
            && st.PresentCount != last_count) {
        if (last_count) {
            UINT n = st.PresentCount - last_count;
            UINT d = st.PresentRefreshCount - last_refresh;
            shown++;
            dropped += n - 1;
            steps[d < 3 ? d : 3]++;
        }
        last_count = st.PresentCount;
        last_refresh = st.PresentRefreshCount;
    }
    now = GetTickCount64();
    if (!next_ms)
        next_ms = now + 10000;
    if (now >= next_ms) {
        fprintf(stderr, "[VSYNC] 10 s: %u presented, %u shown, %u never"
                " shown; refreshes per shown frame 0:%u 1:%u 2:%u 3+:%u%c",
                presents, shown, dropped, steps[0], steps[1], steps[2],
                steps[3], 10);
        presents = shown = dropped = 0;
        memset(steps, 0, sizeof steps);
        next_ms = now + 10000;
    }
}

HRESULT vsync_present(IDXGISwapChain1 *swap)
{
    int m = vsync_mode();
    UINT flags = 0;
    HRESULT hr;

    if (m == VS_ON)
        follow_output(swap);
    else if ((m == VS_OFF || m == VS_VRR) && s_tearing)
        flags = DXGI_PRESENT_ALLOW_TEARING;
    hr = IDXGISwapChain1_Present(swap, m == VS_ON ? 1 : 0, flags);
    stats_note(swap);
    return hr;
}

#else

int xbox_SetVsync(int mode)
{
    (void)mode;
    return 0;
}

#endif
