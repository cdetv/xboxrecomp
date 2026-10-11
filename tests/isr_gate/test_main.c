/* xbox_IsrGateTryEnter: while a guest thread holds the dispatch boundary
 * (raised to DISPATCH_LEVEL), the try reports busy at once instead of
 * waiting; once the holder lowers, it succeeds. The APU frame thread uses
 * it so a descheduled raised thread cannot stop audio output (it waited up
 * to 0.5 s in xbox_IsrGateEnter on an overloaded host). */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

extern ptrdiff_t g_xbox_mem_offset;

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static HANDLE raised, release;

static DWORD WINAPI holder(LPVOID arg)
{
    KIRQL old;
    (void)arg;
    old = xbox_KfRaiseIrql(DISPATCH_LEVEL);   /* takes the gate */
    SetEvent(raised);
    WaitForSingleObject(release, INFINITE);
    xbox_KfLowerIrql(old);                     /* releases it */
    return 0;
}

int main(void)
{
    const char *off = getenv("ISR_GATE_EXPECT_OFF");
    int expect_off = off && atoi(off);
    int fails = 0, got;
    DWORD t0, ms;
    HANDLE th;

    /* irql_publish writes the level into the guest's KPCR, so the IRQL calls
     * need some guest memory behind g_xbox_mem_offset. */
    void *memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE);
    if (!memory) return 10;
    g_xbox_mem_offset = (ptrdiff_t)memory;

    raised = CreateEventA(NULL, TRUE, FALSE, NULL);
    release = CreateEventA(NULL, TRUE, FALSE, NULL);
    th = CreateThread(NULL, 0, holder, NULL, 0, NULL);
    WaitForSingleObject(raised, INFINITE);

    t0 = GetTickCount();
    got = xbox_IsrGateTryEnter();
    ms = GetTickCount() - t0;
    if (got)
        xbox_IsrGateLeave();
    if (expect_off) {
        printf("%s: gate off, try while held returns %d\n", got ? "PASS" : "FAIL", got);
        fails += !got;
    } else {
        printf("%s: try while a raised thread holds the gate returns %d in %lu ms\n",
               (!got && ms < 100) ? "PASS" : "FAIL", got, (unsigned long)ms);
        fails += got || ms >= 100;
    }

    SetEvent(release);
    WaitForSingleObject(th, INFINITE);

    got = xbox_IsrGateTryEnter();
    printf("%s: try after the holder lowered returns %d\n", got ? "PASS" : "FAIL", got);
    fails += !got;
    if (got)
        xbox_IsrGateLeave();
    return fails ? 1 : 0;
}
