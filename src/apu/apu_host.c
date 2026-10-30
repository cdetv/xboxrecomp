/*
 * The .exe's side of the boundary (apu_host.h): the runtime functions
 * xemu_apu.dll may call, and the guest interrupt call it can't make itself.
 *
 * The toolkit's own file (MIT).
 */
#include "apu_host.h"
#include "../kernel/xbox_memory_layout.h"   /* XBOX_WORKER_STACK_TOP */
#include "../kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>

extern ptrdiff_t g_xbox_mem_offset;

/* Calling the title's interrupt service routine, as ohci_call_isr does: a
 * recompiled function reads its arguments off the guest stack and keeps its
 * registers in thread-local storage, so it runs on a worker stack slice with
 * this thread's own registers and TIB. */
typedef void (*apu_guest_fn)(void);
extern apu_guest_fn recomp_lookup(uint32_t xbox_va);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern uint32_t xbox_AllocThreadTib(void);
#if defined(_MSC_VER)
#  define APU_TLS __declspec(thread)
#else
#  define APU_TLS __thread
#endif
extern APU_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
extern APU_TLS uint32_t g_fs_base;

static const char *host_getenv(const char *name)
{
    return getenv(name);
}

static void host_log(int to_stderr, const char *text)
{
    fputs(text, to_stderr ? stderr : stdout);
}

/* Only the APU's frame thread delivers, so one TIB, set on its first call. */
static int isr_prepare(uint32_t vector, ApuIsrCall *c)
{
    static int tib_ready;
    uint8_t *ram = (uint8_t *)xbox_GetMemoryBase();

    c->kint = xbox_GetConnectedInterrupt(vector);
    if (!c->kint)
        return 0;
    c->routine = *(uint32_t *)(ram + c->kint + 0);
    c->context = *(uint32_t *)(ram + c->kint + 4);
    c->fn = c->routine ? (void *)recomp_lookup(c->routine) : NULL;
    if (!c->fn)
        return 0;
    if (!tib_ready) {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib)
            return 0;
        g_fs_base = tib;
        tib_ready = 1;
    }
    c->slot = xbox_worker_stack_alloc();
    return c->slot >= 0;
}

static uint32_t isr_call(ApuIsrCall *c)
{
    uint8_t *ram = (uint8_t *)xbox_GetMemoryBase();

    g_esp = XBOX_WORKER_STACK_TOP(c->slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_esp -= 4; *(uint32_t *)(ram + g_esp) = c->context;
    g_esp -= 4; *(uint32_t *)(ram + g_esp) = c->kint;
    g_esp -= 4; *(uint32_t *)(ram + g_esp) = 0xDEADBEEFu;
    { int _irql = xbox_IrqlEnterInterrupt(16); ((apu_guest_fn)c->fn)(); xbox_IrqlLeaveInterrupt(_irql); }
    return g_eax;
}

static void isr_release(ApuIsrCall *c)
{
    xbox_worker_stack_free(c->slot);
}

static const ApuHost s_host = {
    sizeof(ApuHost),
    APU_HOST_VERSION,
    host_getenv,
    host_log,
    &g_xbox_mem_offset,
    &g_xbox_image_lo,
    &g_xbox_image_hi,
    xbox_PhysAliasToVa,
    xbox_ContiguousAllocatedBytes,
    xbox_IrqlBlocksInterrupts,
    isr_prepare,
    xbox_IsrGateTryEnter,
    xbox_IsrGateEnter,
    xbox_IsrGateLeave,
    isr_call,
    isr_release,
};

const ApuHost *apu_host_table(void)
{
    return &s_host;
}
