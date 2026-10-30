/*
 * xemu_apu.dll's side of the boundary (apu_host.h): its one export, and the
 * getenv / log redirection apu_dll_io.h renames the calls to.
 *
 * Without a host table (a test linking the APU objects statically) both fall
 * back to this module's own C runtime.
 *
 * The toolkit's own file (MIT).
 */
#include "apu_host.h"

#include <string.h>

#undef getenv
#undef fprintf
#undef fflush

const ApuHost *g_apu_host;

/* apu_core.c */
MCPXAPUState *mcpx_apu_init_core(uint8_t *ram_ptr);

char *apu_dll_getenv(const char *name)
{
    if (g_apu_host)
        return (char *)g_apu_host->get_env(name);
    return getenv(name);
}

int apu_dll_fprintf(FILE *f, const char *fmt, ...)
{
    char buf[2048], *text = buf;
    va_list ap;
    int n;

    if (!g_apu_host || (f != stdout && f != stderr)) {
        va_start(ap, fmt);
        n = vfprintf(f, fmt, ap);
        va_end(ap);
        return n;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    if ((size_t)n >= sizeof buf) {
        text = (char *)malloc((size_t)n + 1);
        if (!text)
            return -1;
        va_start(ap, fmt);
        vsnprintf(text, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    g_apu_host->log(f == stderr, text);
    if (text != buf)
        free(text);
    return n;
}

/* The .exe's log is unbuffered: nothing of ours to flush. */
int apu_dll_fflush(FILE *f)
{
    if (g_apu_host && (f == stdout || f == stderr))
        return 0;
    return fflush(f);
}

int xemu_apu_connect(const ApuHost *host, ApuApi *api)
{
    if (!host || host->version != APU_HOST_VERSION ||
        host->size != sizeof(ApuHost) || !api ||
        api->size != sizeof(ApuApi))
        return 0;
    g_apu_host = host;

    api->version            = APU_HOST_VERSION;
    api->init               = mcpx_apu_init_core;
    api->shutdown           = mcpx_apu_shutdown;
    api->mmio_read          = mcpx_apu_mmio_read;
    api->mmio_write         = mcpx_apu_mmio_write;
    api->play_test_tone     = mcpx_apu_play_test_tone;
    api->set_paused         = xbox_ApuSetPaused;
    api->mixer_alloc_voice  = apu_mixer_alloc_voice;
    api->mixer_free_voice   = apu_mixer_free_voice;
    api->mixer_get_voice    = apu_mixer_get_voice;
    api->mixer_set_position = apu_mixer_set_position;
    api->mixer_get_state    = apu_mixer_get_state;
    api->mixer_play         = apu_mixer_play;
    api->mixer_stop         = apu_mixer_stop;
    return 1;
}
