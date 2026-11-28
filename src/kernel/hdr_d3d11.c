/**
 * HDR10 output for the D3D11 back end: the settings, and whether Windows
 * HDR is on for the monitor the window is on.
 *
 * The title's picture stays SDR all the way through the filter chain; with
 * HDR on, the chain puts it out into a 16-bit float texture instead of the
 * back buffer, the host overlay draws into an SDR texture of its own
 * (xbox_GpuOverlayTarget), and one last step (postfx_hdr) maps both onto an
 * HDR10 back buffer: SDR white at the paper white, the 3D picture's
 * brightest parts stretched towards the monitor's peak, and the title's 2D
 * layer (where the frame differs from its copy at NV_HOST_FX_SCENE_END) and
 * the overlay at the paper white only.
 *
 * HDR is offered only where Windows HDR is on for the monitor (its colour
 * space reads PQ, IDXGIOutput6::GetDesc1): a window moved to an SDR monitor,
 * or Windows HDR turned off, drops to SDR at the next present, and comes
 * back the same way. The setting is kept either way.
 *
 *   RECOMP_HDR=off|on|mask     (or 0|1|2) off, on, on with the HUD mask
 *                              shown in red (debug; on an SDR monitor too,
 *                              as an SDR picture); xbox_SetHdr
 *   RECOMP_HDR_PAPER_WHITE=n   nits, 80-1000; 0 / unset = Windows' "SDR
 *                              content brightness" for that monitor
 *   RECOMP_HDR_PEAK=n          nits, 100-10000; 0 / unset = the monitor's
 *   RECOMP_HDR_STRETCH=n       percent, 0-100 (default 50); xbox_SetHdrLevels
 *   RECOMP_HDR_FORMAT=pq|scrgb the swap chain: HDR10 (R10G10B10A2, PQ,
 *                              BT.2020; default) or scRGB (16-bit float,
 *                              linear BT.709)
 *
 * [HDR] lines: each monitor probed (Windows HDR on or off, its peak, full
 * frame and black level, Windows' SDR white) and each output switch.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include "hdr_d3d11.h"
#include <dxgi1_6.h>

/* ---- settings ------------------------------------------------------------- */

#define PAPER_LO   80.0f
#define PAPER_HI   1000.0f
#define PAPER_DEF  200.0f      /* when Windows' SDR white cannot be read */
#define PEAK_LO    100.0f
#define PEAK_HI    10000.0f
#define PEAK_DEF   400.0f      /* when the monitor gives none */
#define STRETCH_DEF 50.0f

static volatile LONG s_mode = -1;      /* 0..2; -1 = not read yet */
static volatile LONG s_paper;          /* float bits; 0 = Windows' */
static volatile LONG s_peak;           /* float bits; 0 = the monitor's */
static volatile LONG s_stretch;        /* float bits, percent */
static int s_scrgb;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static volatile LONG s_active;         /* HDR_OUT_* now */
static volatile LONG s_mon_hdr;        /* the last probe's monitor: HDR on */
static volatile LONG s_mon_peak;       /* float bits */
static volatile LONG s_mon_white;      /* float bits */

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

/* def when unset or out of range (said); 0 is always taken when zero_ok. */
static float env_float(const char *name, float lo, float hi, float def,
                       int zero_ok)
{
    const char *v = getenv(name);
    char *end;
    float f;

    if (!v || !*v)
        return def;
    f = strtof(v, &end);
    if (*end || !((zero_ok && f == 0.0f) || (f >= lo && f <= hi))) {
        fprintf(stderr, "[HDR] %s=%s not %sa number from %g to %g; %g%c",
                name, v, zero_ok ? "0 or " : "", lo, hi, def, 10);
        return def;
    }
    return f;
}

static BOOL CALLBACK params_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    const char *m = getenv("RECOMP_HDR");
    const char *f = getenv("RECOMP_HDR_FORMAT");

    (void)once; (void)p; (void)ctx;
    if (s_mode < 0) {
        static const char *const word[] = { "off", "on", "mask" };
        int i, v = 0;
        for (i = 0; m && *m && i < 3; i++)
            if (!_stricmp(m, word[i]) || (m[0] == '0' + i && !m[1]))
                v = i;
        if (m && *m && !v && _stricmp(m, "off") && strcmp(m, "0"))
            fprintf(stderr, "[HDR] RECOMP_HDR=%s not off, on or mask (0, 1,"
                    " 2); off%c", m, 10);
        InterlockedExchange(&s_mode, v);
    }
    InterlockedExchange(&s_paper, fbits(env_float("RECOMP_HDR_PAPER_WHITE",
                        PAPER_LO, PAPER_HI, 0.0f, 1)));
    InterlockedExchange(&s_peak, fbits(env_float("RECOMP_HDR_PEAK", PEAK_LO,
                        PEAK_HI, 0.0f, 1)));
    InterlockedExchange(&s_stretch, fbits(env_float("RECOMP_HDR_STRETCH",
                        0.0f, 100.0f, STRETCH_DEF, 0)));
    s_scrgb = f && !_stricmp(f, "scrgb");
    if (f && *f && !s_scrgb && _stricmp(f, "pq"))
        fprintf(stderr, "[HDR] RECOMP_HDR_FORMAT=%s not pq or scrgb; pq%c", f,
                10);
    return TRUE;
}

static void params(void)
{
    InitOnceExecuteOnce(&s_once, params_init, NULL, NULL);
}

/* For a settings menu: 0 = off, 1 = on, 2 = on with the HUD mask shown in
 * red (debug). On has an effect only where Windows HDR is on for the
 * monitor. Applies from the next present. 1 when taken. */
int xbox_SetHdr(int mode)
{
    params();
    if (mode < 0 || mode > 2)
        return 0;
    InterlockedExchange(&s_mode, mode);
    return 1;
}

/* For a settings menu: paper white (SDR white, the HUD's brightness) and
 * the peak in nits, 0 = automatic (Windows' SDR content brightness, the
 * monitor's peak); the highlight stretch in percent (0 = the picture as in
 * SDR, at the paper white). A negative value keeps that one as it is.
 * Applies from the next present. 1 when every value given was taken; one
 * out of range is refused and the others still taken. */
int xbox_SetHdrLevels(float paper, float peak, float stretch)
{
    int ok = 1;

    params();
    if (paper >= 0.0f) {
        if (paper == 0.0f || (paper >= PAPER_LO && paper <= PAPER_HI))
            InterlockedExchange(&s_paper, fbits(paper));
        else
            ok = 0;
    }
    if (peak >= 0.0f) {
        if (peak == 0.0f || (peak >= PEAK_LO && peak <= PEAK_HI))
            InterlockedExchange(&s_peak, fbits(peak));
        else
            ok = 0;
    }
    if (stretch >= 0.0f) {
        if (stretch <= 100.0f)
            InterlockedExchange(&s_stretch, fbits(stretch));
        else
            ok = 0;
    }
    return ok;
}

/* For a settings menu: 1 when Windows HDR is on for the monitor the window
 * is on (as last probed, at a present), with that monitor's peak and
 * Windows' SDR content brightness there in nits (0 when not given). */
int xbox_HdrDisplay(float *peak, float *sdr_white)
{
    if (peak)
        *peak = fval(&s_mon_peak);
    if (sdr_white)
        *sdr_white = fval(&s_mon_white);
    return (int)s_mon_hdr;
}

/* 0 = the swap chain is SDR now, 1 = HDR10 (PQ), 2 = scRGB. */
int xbox_HdrActive(void)
{
    return (int)s_active;
}

int hdr_mode(void)
{
    params();
    return (int)s_mode;
}

int hdr_wanted_out(const HdrMonitor *m)
{
    params();
    if (!s_mode || !m->hdr)
        return HDR_OUT_SDR;
    return s_scrgb ? HDR_OUT_SCRGB : HDR_OUT_PQ;
}

void hdr_levels(const HdrMonitor *m, float *paper, float *peak,
                float *stretch)
{
    float p, k;

    params();
    p = fval(&s_paper);
    if (p == 0.0f)
        p = m->sdr_white >= PAPER_LO ? m->sdr_white : PAPER_DEF;
    k = fval(&s_peak);
    if (k == 0.0f)
        k = m->peak >= PEAK_LO ? m->peak : PEAK_DEF;
    *paper = p;
    *peak = k;
    *stretch = fval(&s_stretch) / 100.0f;
}

void hdr_set_active(int out)
{
    InterlockedExchange(&s_active, out);
}

/* ---- the monitor ---------------------------------------------------------- */

static IDXGIFactory1 *s_fac;            /* our own, made again when stale */
static HMONITOR s_mon;
static HdrMonitor s_cur;
static int s_probed;

/* Windows' SDR content brightness for the monitor whose GDI name is gdi
 * (\\.\DISPLAYn), in nits; 0 when it cannot be read (before Windows 10
 * 1709, or under Wine). */
static float sdr_white(const WCHAR *gdi)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *p = NULL;
    DISPLAYCONFIG_MODE_INFO *mi = NULL;
    float nits = 0.0f;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm)
        != ERROR_SUCCESS || !np)
        return 0.0f;
    p = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof *p);
    mi = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof *mi);
    if (p && mi && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p, &nm, mi,
                                      NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            DISPLAYCONFIG_SDR_WHITE_LEVEL w;
            memset(&src, 0, sizeof src);
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof src;
            src.header.adapterId = p[i].sourceInfo.adapterId;
            src.header.id = p[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS
                || wcscmp(src.viewGdiDeviceName, gdi))
                continue;
            memset(&w, 0, sizeof w);
            w.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            w.header.size = sizeof w;
            w.header.adapterId = p[i].targetInfo.adapterId;
            w.header.id = p[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&w.header) == ERROR_SUCCESS)
                nits = (float)w.SDRWhiteLevel / 1000.0f * 80.0f;
            break;
        }
    }
    free(p);
    free(mi);
    return nits;
}

/* Monitor mon as DXGI describes it, through a factory made now: one made
 * before Windows HDR was turned on or off still describes the old state. */
static void probe(HMONITOR mon, HdrMonitor *m)
{
    IDXGIAdapter1 *a = NULL;
    UINT ai, oi;
    int found = 0;

    memset(m, 0, sizeof *m);
    m->mon = mon;
    if (s_fac && !IDXGIFactory1_IsCurrent(s_fac)) {
        IDXGIFactory1_Release(s_fac);
        s_fac = NULL;
    }
    if (!s_fac && FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1,
                                            (void **)&s_fac))) {
        s_fac = NULL;
        return;
    }
    for (ai = 0; !found && SUCCEEDED(IDXGIFactory1_EnumAdapters1(s_fac, ai, &a));
         ai++) {
        IDXGIOutput *o = NULL;
        for (oi = 0; !found
                     && SUCCEEDED(IDXGIAdapter1_EnumOutputs(a, oi, &o)); oi++) {
            DXGI_OUTPUT_DESC d;
            IDXGIOutput6 *o6 = NULL;
            if (SUCCEEDED(IDXGIOutput_GetDesc(o, &d)) && d.Monitor == mon) {
                found = 1;
                WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, m->name,
                                    (int)sizeof m->name, NULL, NULL);
                if (SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput6,
                                                         (void **)&o6))) {
                    DXGI_OUTPUT_DESC1 d1;
                    if (SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d1))) {
                        m->hdr = d1.ColorSpace
                                 == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        m->peak = d1.MaxLuminance;
                        m->full_frame = d1.MaxFullFrameLuminance;
                        m->black = d1.MinLuminance;
                    }
                    IDXGIOutput6_Release(o6);
                }
                m->sdr_white = sdr_white(d.DeviceName);
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
}

int hdr_follow(HWND hwnd, HdrMonitor *m)
{
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : NULL;
    HdrMonitor now;

    if (s_probed && mon == s_mon && s_fac && IDXGIFactory1_IsCurrent(s_fac)) {
        *m = s_cur;
        return 0;
    }
    probe(mon, &now);
    s_mon = mon;
    if (s_probed && !memcmp(&now, &s_cur, sizeof now)) {
        *m = s_cur;
        return 0;
    }
    s_probed = 1;
    s_cur = now;
    *m = now;
    InterlockedExchange(&s_mon_hdr, now.hdr);
    InterlockedExchange(&s_mon_peak, fbits(now.peak));
    InterlockedExchange(&s_mon_white, fbits(now.sdr_white));
    fprintf(stderr, "[HDR] monitor %s: Windows HDR %s; peak %.0f nits, full"
            " frame %.0f, black %.3f; Windows SDR white %.0f nits%c",
            now.name[0] ? now.name : "?", now.hdr ? "on" : "off", now.peak,
            now.full_frame, now.black, now.sdr_white, 10);
    return 1;
}

#else

int xbox_SetHdr(int mode)
{
    (void)mode;
    return 0;
}

int xbox_SetHdrLevels(float paper, float peak, float stretch)
{
    (void)paper; (void)peak; (void)stretch;
    return 0;
}

int xbox_HdrDisplay(float *peak, float *sdr_white)
{
    if (peak) *peak = 0.0f;
    if (sdr_white) *sdr_white = 0.0f;
    return 0;
}

int xbox_HdrActive(void)
{
    return 0;
}

#endif
