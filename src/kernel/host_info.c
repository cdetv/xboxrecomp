/**
 * Host detection: Windows or Wine / Proton, Steam Deck (host_info.h).
 */
#include "host_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
typedef const char *(__cdecl *WineVersionFn)(void);
typedef void (__cdecl *WineHostFn)(const char **sysname, const char **release);

static volatile LONG s_done;
static int s_wine;
static char s_wine_ver[64];
static char s_host_sys[96];

static void detect(void)
{
    HMODULE nt;
    WineVersionFn ver;
    WineHostFn host;
    const char *fake;

    if (s_done)
        return;
    nt = GetModuleHandleA("ntdll.dll");
    ver = nt ? (WineVersionFn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    host = nt ? (WineHostFn)(void *)GetProcAddress(nt, "wine_get_host_version") : NULL;
    if (ver) {
        const char *v = ver();
        s_wine = 1;
        snprintf(s_wine_ver, sizeof s_wine_ver, "%s", v ? v : "?");
        if (host) {
            const char *sys = NULL, *rel = NULL;
            host(&sys, &rel);
            snprintf(s_host_sys, sizeof s_host_sys, "%s %s",
                     sys ? sys : "?", rel ? rel : "");
        }
    } else {
        fake = getenv("RECOMP_FAKE_WINE");
        if (fake && fake[0] && strcmp(fake, "0") != 0) {
            s_wine = 1;
            snprintf(s_wine_ver, sizeof s_wine_ver, "fake (RECOMP_FAKE_WINE)");
        }
    }
    InterlockedExchange(&s_done, 1);
}

int xbox_HostIsWine(void)
{
    detect();
    return s_wine;
}

const char *xbox_HostWineVersion(void)
{
    detect();
    return s_wine_ver;
}

int xbox_HostIsSteamDeck(void)
{
    const char *d = getenv("SteamDeck");
    return d && strcmp(d, "1") == 0;
}

void xbox_HostLogLine(void)
{
    RtlGetVersionFn rtl;
    OSVERSIONINFOW vi;
    SYSTEM_INFO si;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    char win[64];

    detect();
    memset(&vi, 0, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    rtl = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (rtl && rtl(&vi) == 0)
        snprintf(win, sizeof win, "Windows %lu.%lu.%lu", vi.dwMajorVersion,
                 vi.dwMinorVersion, vi.dwBuildNumber);
    else
        snprintf(win, sizeof win, "Windows (version unknown)");
    GetNativeSystemInfo(&si);
    if (s_wine)
        printf("[HOST] %s on Wine %s%s%s%s, %lu logical processors\n", win,
               s_wine_ver, s_host_sys[0] ? " (host " : "", s_host_sys,
               s_host_sys[0] ? ")" : "", si.dwNumberOfProcessors);
    else
        printf("[HOST] %s, %lu logical processors\n", win,
               si.dwNumberOfProcessors);
    if (xbox_HostIsSteamDeck())
        printf("[HOST] SteamDeck=1\n");
}

#else  /* POSIX: a native build is never under Wine. */

#include <sys/utsname.h>
#include <unistd.h>

int xbox_HostIsWine(void) { return 0; }
const char *xbox_HostWineVersion(void) { return ""; }

int xbox_HostIsSteamDeck(void)
{
    const char *d = getenv("SteamDeck");
    return d && strcmp(d, "1") == 0;
}

void xbox_HostLogLine(void)
{
    struct utsname u;
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    if (uname(&u) == 0)
        printf("[HOST] %s %s (native), %ld logical processors\n", u.sysname,
               u.release, n);
    else
        printf("[HOST] native POSIX build, %ld logical processors\n", n);
    if (xbox_HostIsSteamDeck())
        printf("[HOST] SteamDeck=1\n");
}

#endif
