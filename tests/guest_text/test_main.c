/*
 * A title's 8-bit text, converted the Xbox's way whatever the PC's code page.
 *
 * The Xbox kernel's Rtl string calls treat a byte as one Latin-1 character
 * (0xE9 is U+00E9) and give one byte per character back. The runtime used to
 * convert with CP_ACP, the PC's code page, so a title's text depended on the
 * player's Windows -- and a game that asks for a UTF-8 code page in its
 * manifest (Windows 10 1903+, to take folder names with any letters) turned
 * 'é' into two bytes, which RtlUnicodeStringToAnsiString's one-byte-a-character
 * buffer has no room for.
 *
 * This test runs under exactly that manifest (utf8.manifest, Windows), so it
 * fails if any of these conversions follows the code page again. Checked
 * through the kernel bridge (the path a title's calls take, as in
 * tests/kernel_bridge) and through the host-side xbox_Rtl* versions.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */
#include "guest_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;
static uint8_t *mem;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* "Conk é ÿ \x80": past ASCII, each byte must become the same-numbered
 * character, and back. */
static const char k_bytes[] = "Conk \xE9 \xFF \x80";
static const WCHAR k_wide[] = { 'C', 'o', 'n', 'k', ' ', 0x00E9, ' ', 0x00FF, ' ', 0x0080, 0 };
#define K_LEN 10

enum { THUNK_VA = 0x10000, STACK_VA = 0x20000, DATA_VA = 0x30000 };
static const uint32_t k_ordinals[] = { 260, 308, 299, 300, 310, 311, 315 };
#define N_ORD (sizeof k_ordinals / sizeof k_ordinals[0])

static recomp_func_t ordinal(uint32_t ord)
{
    for (unsigned i = 0; i < N_ORD; i++)
        if (k_ordinals[i] == ord)
            return recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4 * i));
    return NULL;
}

/* stdcall from the guest: a return address, then the arguments. */
static void call(uint32_t ord, int n, const uint32_t *args)
{
    uint32_t *sp = (uint32_t *)(mem + STACK_VA);
    recomp_func_t fn = ordinal(ord);
    sp[0] = 0xBEEF0001u;
    for (int i = 0; i < n; i++)
        sp[1 + i] = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    if (fn)
        fn();
}

#define G(va) ((void *)(mem + (va)))

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

#ifdef _WIN32
    /* The point of the test: the PC's code page is UTF-8 here. */
    printf("  GetACP = %u\n", GetACP());
    check(GetACP() == 65001, "process code page is UTF-8 (utf8.manifest took effect)");
#endif

    /* The helpers themselves. */
    {
        WCHAR w[16];
        char b[16];
        check(xbox_guest_text_to_wide(k_bytes, K_LEN, w, 16) == K_LEN
              && !memcmp(w, k_wide, K_LEN * sizeof(WCHAR)), "bytes -> UTF-16, one character a byte");
        check(xbox_guest_text_from_wide(k_wide, K_LEN, b, 16) == K_LEN
              && !memcmp(b, k_bytes, K_LEN), "UTF-16 -> bytes, one byte a character");
        {
            const WCHAR far_[] = { 0x0142, 0x30BB, 'x', 0 };   /* ł, セ: no byte */
            check(xbox_guest_text_from_wide(far_, -1, b, 16) == 4 && !strcmp(b, "??x"),
                  "a character past U+00FF becomes '?'");
        }
        check(xbox_guest_text_to_wide(k_bytes, -1, NULL, 0) == K_LEN + 1,
              "-1 counts the NUL, NULL asks for the size");
        check(xbox_guest_text_to_wide(k_bytes, K_LEN, w, K_LEN - 1) == 0,
              "too small a buffer fails, as MultiByteToWideChar does");
    }

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(mem != NULL, "guest memory stand-in allocated");
    if (!mem) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    for (unsigned i = 0; i < N_ORD; i++)
        *(uint32_t *)(mem + THUNK_VA + 4 * i) = 0x80000000u | k_ordinals[i];
    xbox_kernel_set_thunk_address(THUNK_VA, (uint32_t)N_ORD);
    xbox_kernel_bridge_init();
    for (unsigned i = 0; i < N_ORD; i++) {
        char what[80];
        snprintf(what, sizeof what, "ordinal %u resolves to a bridge entry point", k_ordinals[i]);
        check(ordinal(k_ordinals[i]) != NULL, what);
    }

    /* Guest data: an ANSI_STRING / UNICODE_STRING is Length, MaximumLength
     * (16 bits each), Buffer (a guest VA). */
    const uint32_t SRC_STR = DATA_VA, DST_STR = DATA_VA + 0x10;
    const uint32_t SRC_BUF = DATA_VA + 0x100, DST_BUF = DATA_VA + 0x200, OUT_VA = DATA_VA + 0x400;

    /* 260 RtlAnsiStringToUnicodeString(dst, src, FALSE). */
    memcpy(G(SRC_BUF), k_bytes, K_LEN);
    *(uint16_t *)G(SRC_STR) = K_LEN;
    *(uint16_t *)G(SRC_STR + 2) = K_LEN + 1;
    *(uint32_t *)G(SRC_STR + 4) = SRC_BUF;
    memset(G(DST_BUF), 0xCC, 64);
    *(uint16_t *)G(DST_STR) = 0;
    *(uint16_t *)G(DST_STR + 2) = 64;
    *(uint32_t *)G(DST_STR + 4) = DST_BUF;
    { uint32_t a[] = { DST_STR, SRC_STR, 0 }; call(260, 3, a); }
    check(g_eax == 0 && *(uint16_t *)G(DST_STR) == K_LEN * 2
          && !memcmp(G(DST_BUF), k_wide, (K_LEN + 1) * sizeof(WCHAR)),
          "260 RtlAnsiStringToUnicodeString: e-acute is U+00E9");

    /* 308 RtlUnicodeStringToAnsiString(dst, src, FALSE): a buffer of exactly
     * one byte a character plus the NUL, as the call sizes it. */
    memcpy(G(SRC_BUF), k_wide, (K_LEN + 1) * sizeof(WCHAR));
    *(uint16_t *)G(SRC_STR) = K_LEN * 2;
    *(uint16_t *)G(SRC_STR + 2) = (K_LEN + 1) * 2;
    memset(G(DST_BUF), 0xCC, 64);
    *(uint16_t *)G(DST_STR) = 0;
    *(uint16_t *)G(DST_STR + 2) = K_LEN + 1;
    { uint32_t a[] = { DST_STR, SRC_STR, 0 }; call(308, 3, a); }
    check(g_eax == 0 && *(uint16_t *)G(DST_STR) == K_LEN
          && !memcmp(G(DST_BUF), k_bytes, K_LEN + 1),
          "308 RtlUnicodeStringToAnsiString: one byte a character");

    /* 299 RtlMultiByteToUnicodeN(uni, max bytes, &bytes, mb, mb bytes). */
    memcpy(G(SRC_BUF), k_bytes, K_LEN);
    memset(G(DST_BUF), 0xCC, 64);
    { uint32_t a[] = { DST_BUF, 64, OUT_VA, SRC_BUF, K_LEN }; call(299, 5, a); }
    check(g_eax == 0 && *(uint32_t *)G(OUT_VA) == K_LEN * 2
          && !memcmp(G(DST_BUF), k_wide, K_LEN * sizeof(WCHAR)),
          "299 RtlMultiByteToUnicodeN");

    /* 300 RtlMultiByteToUnicodeSize(&bytes, mb, mb bytes). */
    *(uint32_t *)G(OUT_VA) = 0;
    { uint32_t a[] = { OUT_VA, SRC_BUF, K_LEN }; call(300, 3, a); }
    check(g_eax == 0 && *(uint32_t *)G(OUT_VA) == K_LEN * 2, "300 RtlMultiByteToUnicodeSize");

    /* 310 RtlUnicodeToMultiByteN(mb, max bytes, &bytes, uni, uni bytes). */
    memcpy(G(SRC_BUF), k_wide, K_LEN * sizeof(WCHAR));
    memset(G(DST_BUF), 0xCC, 64);
    { uint32_t a[] = { DST_BUF, 64, OUT_VA, SRC_BUF, K_LEN * 2 }; call(310, 5, a); }
    check(g_eax == 0 && *(uint32_t *)G(OUT_VA) == K_LEN && !memcmp(G(DST_BUF), k_bytes, K_LEN),
          "310 RtlUnicodeToMultiByteN");

    /* 311 RtlUnicodeToMultiByteSize(&bytes, uni, uni bytes). */
    *(uint32_t *)G(OUT_VA) = 0;
    { uint32_t a[] = { OUT_VA, SRC_BUF, K_LEN * 2 }; call(311, 3, a); }
    check(g_eax == 0 && *(uint32_t *)G(OUT_VA) == K_LEN, "311 RtlUnicodeToMultiByteSize");

    /* 315 RtlUpcaseUnicodeToMultiByteN: one byte a character too. Upcasing
     * goes through towupper, which in the C locale leaves e-acute as it is
     * (0xE9); either byte is one byte, which is what this test is about. */
    memset(G(DST_BUF), 0xCC, 64);
    { uint32_t a[] = { DST_BUF, 64, OUT_VA, SRC_BUF, K_LEN * 2 }; call(315, 5, a); }
    {
        const uint8_t *d = (const uint8_t *)G(DST_BUF);
        check(g_eax == 0 && *(uint32_t *)G(OUT_VA) == K_LEN && d[0] == 'C' && d[1] == 'O'
              && (d[5] == 0xE9 || d[5] == 0xC9) && d[6] == ' '
              && (d[7] == 0xFF || d[7] == '?') && d[8] == ' ',   /* y-diaeresis upcases past U+00FF */
              "315 RtlUpcaseUnicodeToMultiByteN: one byte a character");
    }

    /* The host-side versions (kernel_rtl.c). */
    {
        XBOX_ANSI_STRING a;
        XBOX_UNICODE_STRING u;
        WCHAR wb[32];
        char cb[32];
        a.Length = K_LEN;
        a.MaximumLength = K_LEN + 1;
        a.Buffer = (PCHAR)k_bytes;
        u.Length = 0;
        u.MaximumLength = sizeof wb;
        u.Buffer = wb;
        check(xbox_RtlAnsiStringToUnicodeString(&u, &a, FALSE) == STATUS_SUCCESS
              && u.Length == K_LEN * 2 && !memcmp(wb, k_wide, (K_LEN + 1) * sizeof(WCHAR)),
              "xbox_RtlAnsiStringToUnicodeString");
        a.Length = 0;
        a.MaximumLength = K_LEN + 1;
        a.Buffer = cb;
        check(xbox_RtlUnicodeStringToAnsiString(&a, &u, FALSE) == STATUS_SUCCESS
              && a.Length == K_LEN && !memcmp(cb, k_bytes, K_LEN),
              "xbox_RtlUnicodeStringToAnsiString");
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
