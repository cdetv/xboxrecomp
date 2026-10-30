/*
 * The boundary between a game's .exe and xemu_apu.dll.
 *
 * The emulated APU (apu_core.c, apu_vp.c, apu_dsp.c) is extracted from xemu
 * and LGPL; the rest of the runtime is MIT. On Windows the APU is built as
 * its own xemu_apu.dll, which a player can replace with one built from
 * modified sources, as the LGPL asks. The .exe loads it at start-up
 * (apu_loader.c) and the two talk only through the two tables below:
 *
 *   ApuHost  what the DLL needs from the runtime (filled by apu_host.c)
 *   ApuApi   the APU entry points apu.h promises (filled by the DLL)
 *
 * Why a table and not plain imports, both ways:
 *   - the guest's interrupt routine runs on the runtime's thread-local guest
 *     registers (g_eax.., g_esp, g_fs_base). Another module can't reach
 *     them, so the call itself stays in the .exe (isr_*);
 *   - each module has its own static C runtime: its own environment copy
 *     (the runtime sets options with _putenv_s, also live from a settings
 *     menu) and its own stdout/stderr (the runtime redirects them into its
 *     log). So the DLL asks the .exe for getenv and writes its log lines
 *     through it (apu_dll_io.h);
 *   - each module has its own heap: whatever one side allocates, it frees.
 *
 * This file is the toolkit's own (MIT), shared by both sides.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "apu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raised whenever either table changes shape; a DLL built for another
 * version refuses to connect rather than call through a wrong table. */
#define APU_HOST_VERSION 1

/* One delivery of the APU interrupt to the guest's service routine:
 * isr_prepare fills it, isr_call runs it, isr_release gives back what
 * prepare took. Opaque to the DLL apart from routine (for its log). */
typedef struct ApuIsrCall {
    uint32_t kint;      /* guest KINTERRUPT */
    uint32_t routine;   /* guest service routine */
    uint32_t context;
    void    *fn;        /* the recompiled routine */
    int      slot;      /* worker stack */
} ApuIsrCall;

typedef struct ApuHost {
    uint32_t size;       /* sizeof(ApuHost) */
    uint32_t version;    /* APU_HOST_VERSION */

    /* The .exe's environment and log. */
    const char *(*get_env)(const char *name);   /* not "getenv": apu_dll_io.h renames that */
    void (*log)(int to_stderr, const char *text);

    /* Guest memory, for resolving the physical addresses DirectSound hands
     * the APU (mcpx_apu_phys) and the DSP scratch window. */
    const ptrdiff_t *mem_offset;          /* g_xbox_mem_offset */
    const uint32_t  *image_lo, *image_hi; /* the XBE image's span */
    int      (*phys_alias_to_va)(uint32_t pa, uint32_t *va);
    uint32_t (*contiguous_allocated_bytes)(void);

    /* The interrupt, in the order apu_deliver_irq uses them. */
    int      (*irql_blocks_interrupts)(void);
    int      (*isr_prepare)(uint32_t vector, ApuIsrCall *c); /* 0: nothing to call */
    int      (*isr_gate_try_enter)(void);
    void     (*isr_gate_enter)(void);
    void     (*isr_gate_leave)(void);
    uint32_t (*isr_call)(ApuIsrCall *c);  /* runs it; returns the guest's eax */
    void     (*isr_release)(ApuIsrCall *c);
} ApuHost;

typedef struct ApuApi {
    uint32_t size;       /* sizeof(ApuApi) */
    uint32_t version;    /* APU_HOST_VERSION */

    MCPXAPUState *(*init)(uint8_t *ram_ptr);
    void     (*shutdown)(MCPXAPUState *d);
    uint64_t (*mmio_read)(MCPXAPUState *d, uint64_t addr, unsigned int size);
    void     (*mmio_write)(MCPXAPUState *d, uint64_t addr, uint64_t val,
                           unsigned int size);
    void     (*play_test_tone)(MCPXAPUState *d);
    void     (*set_paused)(int on);

    int      (*mixer_alloc_voice)(void);
    void     (*mixer_free_voice)(int slot);
    APUMixerVoice *(*mixer_get_voice)(int slot);
    int      (*mixer_set_position)(int slot, uint32_t byte_offset);
    void     (*mixer_get_state)(int slot, uint32_t *byte_offset, int *active,
                                int *looping);
    void     (*mixer_play)(int slot, int looping);
    void     (*mixer_stop)(int slot);
} ApuApi;

/* The DLL's one export. Keeps host (it must outlive the APU) and fills api.
 * Returns 0 and leaves api untouched when the versions differ. */
#if defined(_WIN32) && defined(XEMU_APU_BUILD)
#  define XEMU_APU_EXPORT __declspec(dllexport)
#else
#  define XEMU_APU_EXPORT
#endif
XEMU_APU_EXPORT int xemu_apu_connect(const ApuHost *host, ApuApi *api);

/* The .exe side's table (apu_host.c). */
const ApuHost *apu_host_table(void);

#ifdef __cplusplus
}
#endif
