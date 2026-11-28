/**
 * HDR10 output for the D3D11 back end (hdr_d3d11.c): the settings, and
 * whether Windows HDR is on for the monitor the window is on. The swap
 * chain switch is nv2a_draw_d3d11.c's (hdr_update), the HDR step
 * postfx_d3d11.c's (postfx_hdr).
 */
#ifndef HDR_D3D11_H
#define HDR_D3D11_H

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>

/* What the swap chain puts out. */
enum { HDR_OUT_SDR, HDR_OUT_PQ, HDR_OUT_SCRGB };

/* The monitor holding most of the window, as Windows describes it. */
typedef struct {
    HMONITOR mon;
    int      hdr;           /* Windows HDR is on for it (colour space PQ) */
    float    peak;          /* nits, the monitor's own figures; 0 = none */
    float    full_frame;
    float    black;
    float    sdr_white;     /* Windows' SDR content brightness; 0 = unread */
    char     name[40];      /* \\.\DISPLAYn */
} HdrMonitor;

/* Before each present, on the executor thread: m filled for the monitor
 * holding most of hwnd, probed again when that monitor changes or Windows'
 * display setup does (Windows HDR turned on or off, a monitor plugged in).
 * 1 when anything in it changed since the last call (said in the log). */
int hdr_follow(HWND hwnd, HdrMonitor *m);

/* The setting: 0 off, 1 on, 2 on with the HUD mask shown (debug). */
int hdr_mode(void);

/* The output wanted for monitor m: HDR_OUT_SDR when the setting is off or
 * Windows HDR is off there; else PQ, or scRGB with RECOMP_HDR_FORMAT=scrgb. */
int hdr_wanted_out(const HdrMonitor *m);

/* The HDR step's levels for monitor m: paper white and peak in nits (each
 * the setting's, else Windows' SDR brightness / the monitor's peak), the
 * highlight stretch 0..1. */
void hdr_levels(const HdrMonitor *m, float *paper, float *peak,
                float *stretch);

/* The swap chain's output now, for xbox_HdrActive. */
void hdr_set_active(int out);

#endif

#endif
