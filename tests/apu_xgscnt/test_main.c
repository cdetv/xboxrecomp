/* NV_PAPU_XGSCNT must count output samples at 48 kHz from whatever the title
 * wrote: XACT turns it into its clock as count * 10^7 / 48000, and a 10 MHz
 * tick (the old ns / 100 read) ran that clock ~208 times fast. See README.md.
 *
 * Run with: ctest --test-dir <build>   (arms in CMakeLists.txt) */
#include "apu_state.h"
#include "apu.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static MCPXAPUState *d;
static int fails;

static uint32_t rd(void) { return (uint32_t)mcpx_apu_mmio_read(d, NV_PAPU_XGSCNT, 4); }

static void check(int ok, const char *what, uint32_t got, uint32_t want)
{
    printf("%s: %s (got %u, want %u)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) fails++;
}

/* One second of frames, as the frame thread runs them. */
static void one_second(void)
{
    int i;
    for (i = 0; i < 48000 / NUM_SAMPLES_PER_FRAME; i++)
        mcpx_apu_xgscnt_advance(d, NUM_SAMPLES_PER_FRAME);
}

int main(void)
{
    const char *arm = getenv("APU_XGSCNT_ARM");
    int expect_pass;
    uint32_t v;

    if (!arm) arm = "samples";
    expect_pass = !strcmp(arm, "samples");
    /* Zeroed is enough: XGSCNT reads and writes touch only regs[]. */
    d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) { fprintf(stderr, "out of memory\n"); return 2; }

    mcpx_apu_mmio_write(d, NV_PAPU_XGSCNT, 0, 4);       /* DirectSound's reset */
    v = rd();
    check(v == 0, "reads 0 after the title writes 0", v, 0);

    one_second();
    v = rd();
    check(v == 48000, "one second of frames is 48000 samples", v, 48000);

    mcpx_apu_mmio_write(d, NV_PAPU_XGSCNT, 0xFFFFFFF0u, 4);
    mcpx_apu_xgscnt_advance(d, NUM_SAMPLES_PER_FRAME);
    v = rd();
    check(v == 0x10, "wraps at 32 bits from a written value", v, 0x10);

    printf("arm %s: %d check(s) failed\n", arm, fails);
    if (expect_pass)
        return fails ? 1 : 0;
    /* Negative control: the old read must get at least one check wrong. */
    return fails ? 0 : 1;
}
