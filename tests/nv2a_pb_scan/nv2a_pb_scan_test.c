/* Where the pushbuffer walk resumes after a JUMP to a recorded pushbuffer.
 *
 * D3D's RunPushBuffer jumps from the ring to a recorded buffer and patches
 * the buffer's last dword into a JUMP back. The walk runs after the title
 * moved PUT, so a buffer run twice already holds the second run's return:
 * trusting it skipped the commands between the two runs, or -- when that
 * return lay outside the segment -- ended the walk and lost the rest of it
 * (Conker's front end: black wedges). The return is now taken from the ring.
 * A buffer with no jump back at all must not walk off the end of RAM. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define RAM      0x100000u
#define CONTIG   0x80000000u
#define SPAN     0x7E000000u                /* CONTIG up to the end of PGRAPH */

/* Guest VAs from CONTIG on. The walk also writes two GPU registers through
 * the same memory offset (PFIFO DMA_GET at 0xFD800044, PGRAPH 0xFD400B10),
 * so their pages are backed too, not just the RAM. */
static uint8_t *s_ram;
size_t g_xbox_total_ram = RAM;

static void map_guest(void)
{
#ifdef _WIN32
    s_ram = (uint8_t *)VirtualAlloc(NULL, SPAN, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc(s_ram, RAM, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(s_ram + (0xFD400000u - CONTIG), 0x1000, MEM_COMMIT,
                 PAGE_READWRITE);
    VirtualAlloc(s_ram + (0xFD800000u - CONTIG), 0x1000, MEM_COMMIT,
                 PAGE_READWRITE);
#else
    s_ram = (uint8_t *)mmap(NULL, SPAN, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)((uintptr_t)s_ram - CONTIG);
}

static uint32_t s_log[64];
static int s_nlog;

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    (void)subch; (void)param;
    if (s_nlog < 64)
        s_log[s_nlog++] = method;
}

void nv2a_pb_exec_report(void) {}
void xbox_Nv2aSoftwareMethod(uint32_t subch, uint32_t param)
{
    (void)subch; (void)param;
}

extern void nv2a_pb_scan(uint32_t start_va, uint32_t end_va);
extern void nv2a_pb_scan_ring(uint32_t lo_va, uint32_t hi_va);

static void put(uint32_t va, uint32_t w)
{
    memcpy(s_ram + (va - CONTIG), &w, 4);
}

#define M(m)   ((1u << 18) | (m))           /* one parameter, subchannel 0 */
#define JMP(p) ((p) | 1u)                   /* jump to physical p */

static int expect(const char *name, const uint32_t *want, int n)
{
    int i, ok = s_nlog == n;
    for (i = 0; ok && i < n; i++)
        ok = s_log[i] == want[i];
    printf("%s: %s (", name, ok ? "ok" : "FAIL");
    for (i = 0; i < s_nlog; i++)
        printf("%s%03X", i ? " " : "", s_log[i]);
    printf(")\n");
    s_nlog = 0;
    return ok;
}

int main(void)
{
    const uint32_t buf = CONTIG + 0x40000;  /* the recorded pushbuffer */
    int ok = 1;

    map_guest();
#ifdef _WIN32
    _putenv("RECOMP_PB_EXEC=1");
#else
    setenv("RECOMP_PB_EXEC", "1", 1);
#endif
    nv2a_pb_scan_ring(CONTIG + 0x1000, CONTIG + 0x3000);

    /* Run twice in one segment; the buffer returns after the second run. */
    put(buf + 0, M(0x30C)); put(buf + 4, 9); put(buf + 8, JMP(0x1018));
    put(CONTIG + 0x1000, M(0x300)); put(CONTIG + 0x1004, 1);
    put(CONTIG + 0x1008, JMP(0x40000));
    put(CONTIG + 0x100C, M(0x304)); put(CONTIG + 0x1010, 2);
    put(CONTIG + 0x1014, JMP(0x40000));
    put(CONTIG + 0x1018, M(0x308)); put(CONTIG + 0x101C, 3);
    nv2a_pb_scan(CONTIG + 0x1000, CONTIG + 0x1020);
    {
        static const uint32_t w[] = { 0x300, 0x30C, 0x304, 0x30C, 0x308 };
        ok &= expect("run twice", w, 5);
    }

    /* NOP 0xC: the return is in a record after the JUMP; the buffer's own
     * return points outside the segment. */
    put(buf + 8, JMP(0x2000));
    put(CONTIG + 0x1100, M(0x100));
    put(CONTIG + 0x1104, ((CONTIG + 0x110C) << 5) | 0xC);
    put(CONTIG + 0x1108, JMP(0x40000));
    put(CONTIG + 0x110C, 0x1120); put(CONTIG + 0x1110, 0);
    put(CONTIG + 0x1114, buf + 8); put(CONTIG + 0x1118, 0);
    put(CONTIG + 0x111C, buf);
    put(CONTIG + 0x1120, M(0x310)); put(CONTIG + 0x1124, 4);
    nv2a_pb_scan(CONTIG + 0x1100, CONTIG + 0x1128);
    {
        static const uint32_t w[] = { 0x100, 0x30C, 0x310 };
        ok &= expect("NOP 0xC record", w, 3);
    }

    /* A buffer at the very end of RAM with no jump back. */
    put(CONTIG + RAM - 8, M(0x314)); put(CONTIG + RAM - 4, 5);
    put(CONTIG + 0x1200, JMP(RAM - 8));
    put(CONTIG + 0x1204, M(0x31C)); put(CONTIG + 0x1208, 6);
    nv2a_pb_scan(CONTIG + 0x1200, CONTIG + 0x120C);
    {
        static const uint32_t w[] = { 0x314, 0x31C };
        ok &= expect("no jump back at end of RAM", w, 2);
    }
    return ok ? 0 : 1;
}
