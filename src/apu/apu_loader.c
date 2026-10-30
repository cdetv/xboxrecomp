/*
 * apu.h for the .exe: on Windows every call goes to xemu_apu.dll, loaded
 * from the .exe's own folder on first use (apu_host.h says why it's a DLL).
 * Without the DLL, or with one for another version, the game runs silent:
 * init fails and the rest does nothing.
 *
 * Elsewhere the APU objects are linked in (CMakeLists.txt) and only init
 * needs the host table handed over.
 *
 * The toolkit's own file (MIT).
 */
#include "apu_host.h"

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>

typedef int (*apu_connect_fn)(const ApuHost *host, ApuApi *api);

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static ApuApi s_api;
static const ApuApi *s_apu;   /* NULL: no usable DLL */

static BOOL CALLBACK apu_load(PINIT_ONCE once, void *param, void **ctx)
{
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW(NULL, path, 1024);
    wchar_t *slash = n && n < 1024 ? wcsrchr(path, L'\\') : NULL;
    HMODULE dll;
    apu_connect_fn connect;

    (void)once; (void)param; (void)ctx;
    if (!slash || (size_t)(slash + 1 - path) + 13 > 1024) {
        fprintf(stderr, "[APU] can't find the .exe's folder: no emulated audio\n");
        return TRUE;
    }
    wcscpy(slash + 1, L"xemu_apu.dll");
    /* The full path, so no other folder's xemu_apu.dll is picked up. */
    dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "[APU] xemu_apu.dll not loaded from the .exe's folder"
                " (error %lu): no emulated audio\n", GetLastError());
        return TRUE;
    }
    connect = (apu_connect_fn)(void (*)(void))GetProcAddress(dll, "xemu_apu_connect");
    s_api.size = sizeof(ApuApi);
    if (!connect || !connect(apu_host_table(), &s_api)) {
        fprintf(stderr, "[APU] xemu_apu.dll is not for this .exe (host table"
                " version %d): no emulated audio\n", APU_HOST_VERSION);
        FreeLibrary(dll);
        return TRUE;
    }
    s_apu = &s_api;
    fprintf(stderr, "[APU] xemu_apu.dll loaded\n");
    return TRUE;
}

static const ApuApi *apu(void)
{
    InitOnceExecuteOnce(&s_once, apu_load, NULL, NULL);
    return s_apu;
}

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    return apu() ? s_apu->init(ram_ptr) : NULL;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (apu()) s_apu->shutdown(d);
}

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    return apu() ? s_apu->mmio_read(d, addr, size) : 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (apu()) s_apu->mmio_write(d, addr, val, size);
}

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (apu()) s_apu->play_test_tone(d);
}

void xbox_ApuSetPaused(int on)
{
    if (apu()) s_apu->set_paused(on);
}

int apu_mixer_alloc_voice(void)
{
    return apu() ? s_apu->mixer_alloc_voice() : -1;
}

void apu_mixer_free_voice(int slot)
{
    if (apu()) s_apu->mixer_free_voice(slot);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    return apu() ? s_apu->mixer_get_voice(slot) : NULL;
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    return apu() ? s_apu->mixer_set_position(slot, byte_offset) : 0;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (apu()) {
        s_apu->mixer_get_state(slot, byte_offset, active, looping);
        return;
    }
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
}

void apu_mixer_play(int slot, int looping)
{
    if (apu()) s_apu->mixer_play(slot, looping);
}

void apu_mixer_stop(int slot)
{
    if (apu()) s_apu->mixer_stop(slot);
}

#else

MCPXAPUState *mcpx_apu_init_core(uint8_t *ram_ptr);

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    static ApuApi api = { sizeof(ApuApi) };
    if (!xemu_apu_connect(apu_host_table(), &api))
        return NULL;
    return mcpx_apu_init_core(ram_ptr);
}

#endif
