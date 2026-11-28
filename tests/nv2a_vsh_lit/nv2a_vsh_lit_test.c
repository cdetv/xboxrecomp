/* Regression test for the lit-program scan (src/kernel/nv2a_vsh_lit.c).
 *
 * The programs here are made up for the test, written with a tiny
 * assembler below: one of each output layout, each with its own register
 * use and instruction order (copies, paired MAC + ILU ops, operands either
 * way round), and near misses that must stay per vertex. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vsh_lit.h"

static Nv2aVshIns P[NV2A_VSH_SLOTS];
static uint32_t np;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* "-r8.xyz", "v3", "c179.xyzz", "a108" (c[a0 + 108]), "opos" */
static void src(Nv2aVshIns *I, int k, const char *s)
{
    Nv2aVshSrc *d = &I->src[k];
    const char *dot;
    int j, n;

    if (!s)
        return;
    I->need |= (uint8_t)(1 << k);
    d->neg = 0;
    if (*s == '-') { d->neg = 1; s++; }
    if (!strncmp(s, "opos", 4)) {
        d->mux = 1; d->reg = 12;
    } else {
        n = atoi(s + 1);
        switch (*s) {
        case 'r': d->mux = 1; d->reg = (uint8_t)n; break;
        case 'v': d->mux = 2; I->iidx = (uint16_t)n; break;
        case 'a': d->mux = 3; I->cidx = (uint16_t)n; I->rel = 1; break;
        default:  d->mux = 3; I->cidx = (uint16_t)n; break;
        }
    }
    for (j = 0; j < 4; j++)
        d->sw[j] = (uint8_t)j;
    if ((dot = strchr(s, '.')) != NULL) {
        size_t len = strlen(dot + 1);
        for (j = 0; j < 4; j++) {
            char c = dot[1 + ((size_t)j < len ? (size_t)j : len - 1)];
            d->sw[j] = (uint8_t)(c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3);
        }
    }
}

static uint8_t mask(const char *m)
{
    uint8_t b = 0;
    for (; *m && *m != ','; m++)
        b |= (uint8_t)(*m == 'x' ? 8 : *m == 'y' ? 4 : *m == 'z' ? 2 : 1);
    return b;
}

/* "r3.xy", "o4.xyz", "r9.x,o4.w": a temp and/or an output. */
static void dst(Nv2aVshIns *I, const char *d, int ilu)
{
    while (d && *d) {
        const char *dot = strchr(d, '.');
        uint8_t m = mask(dot + 1);
        if (*d == 'o') {
            I->out_o = 1;
            I->oaddr = (uint16_t)atoi(d + 1);
            I->omask = m;
            I->out_ilu = (uint8_t)ilu;
        } else if (ilu) {
            I->ilu_mask = m;
            if (!I->mac)
                I->tdst = (uint8_t)atoi(d + 1);
        } else {
            I->tdst = (uint8_t)atoi(d + 1);
            I->mac_mask = m;
        }
        d = strchr(d, ',');
        if (d)
            d++;
    }
}

static int mac_code(const char *op)
{
    static const char *const k[] = { "nop", "mov", "mul", "add", "mad", "dp3",
        "dph", "dp4", "dst", "min", "max", "slt", "sge", "arl" };
    int i;
    for (i = 0; i < 14; i++)
        if (!strcmp(op, k[i]))
            return i;
    abort();
}

static int ilu_code(const char *op)
{
    static const char *const k[] = { "nop", "imov", "rcp", "rcc", "rsq", "exp",
        "log", "lit" };
    int i;
    for (i = 0; i < 8; i++)
        if (!strcmp(op, k[i]))
            return i;
    abort();
}

static void begin(void)
{
    memset(P, 0, sizeof P);
    np = 0;
}

/* A MAC op alone: A, B (dp*, mul, mad, sge...), C (add, mad). */
static void mac(const char *op, const char *d, const char *a, const char *b,
                const char *c)
{
    Nv2aVshIns *I = &P[np++];
    I->mac = (uint8_t)mac_code(op);
    src(I, 0, a); src(I, 1, b); src(I, 2, c);
    dst(I, d, 0);
}

/* An ILU op alone (writes its temp field). */
static void ilu(const char *op, const char *d, const char *c)
{
    Nv2aVshIns *I = &P[np++];
    I->ilu = (uint8_t)ilu_code(op);
    src(I, 2, c);
    dst(I, d, 1);
}

/* Both units: the ILU's temp write goes to R1 (id is "r1.<mask>"). */
static void pair(const char *op, const char *d, const char *a, const char *b,
                 const char *c, const char *iop, const char *id)
{
    Nv2aVshIns *I = &P[np++];
    I->mac = (uint8_t)mac_code(op);
    I->ilu = (uint8_t)ilu_code(iop);
    src(I, 0, a); src(I, 1, b); src(I, 2, c);
    dst(I, d, 0);
    dst(I, id, 1);
}

static int scan(Nv2aVshLit *L)
{
    char d[256];
    int r;
    P[np - 1].final = 1;
    r = nv2a_vsh_lit_scan(P, np, nv2a_vsh_lit_regs(), L);
    nv2a_vsh_lit_describe(L, d, sizeof d);
    printf("  %s\n", r ? d : "(not a candidate)");
    return r;
}

/* The ambient's two DPH stages for normal n into channel lane ch of out,
 * through the temp tmp. */
static void ambient(const char *n, int ch, const char *tmp, const char *out)
{
    static const char *const l = "xyzw";
    char t[16], c[8], o[16];
    int k;
    for (k = 0; k < 4; k++) {
        snprintf(t, sizeof t, "%s.%c", tmp, l[k]);
        snprintf(c, sizeof c, "c%d", 166 + 4 * ch + k);
        mac("dph", t, n, c, NULL);
    }
    snprintf(o, sizeof o, "%s.%c", out, l[ch]);
    mac("dph", o, n, tmp, NULL);
}

/* Blob group g from x, y, h (lanes of temp xyz) into lane `lane` of sum,
 * with temps d, e and the rcc results in r1. */
static void blobs(int g, const char *xyz, const char *sum, int lane)
{
    char a[16], b[16], c[16], x[16], y[16], h[16], s[16];
    static const char *const l = "xyzw";
    int k;
    snprintf(x, sizeof x, "-%s.xxxx", xyz);
    snprintf(y, sizeof y, "-%s.yyyy", xyz);
    snprintf(h, sizeof h, "%s.zzzz", xyz);
    snprintf(a, sizeof a, "c%d", 146 + 4 * g);
    mac("add", "r0.xyzw", x, NULL, a);
    snprintf(a, sizeof a, "c%d", 147 + 4 * g);
    mac("add", "r6.xyzw", y, NULL, a);
    mac("mul", "r0.xyzw", "r0", "r0", NULL);
    mac("mad", "r0.xyzw", "r6", "r6", "r0");
    snprintf(a, sizeof a, "c%d", 148 + 4 * g);
    mac("sge", "r6.xyzw", h, a, NULL);
    snprintf(a, sizeof a, "c%d", 149 + 4 * g);
    mac("mul", "r6.xyzw", "r6", a, NULL);
    for (k = 0; k < 4; k++) {
        snprintf(b, sizeof b, "r1.%c", l[k]);
        snprintf(c, sizeof c, "r0.%c%c%c%c", l[k], l[k], l[k], l[k]);
        ilu("rcc", b, c);
    }
    snprintf(s, sizeof s, "%s.%c", sum, l[lane]);
    mac("dp4", s, "r1", "r6", NULL);
}

/* Layout A, three groups, a0-relative reads: the normal copied before
 * part of the ambient reads it, the key's operands the other way round. */
static void prog_a(const char *key_colour, int normalise, int swap_rows)
{
    begin();
    mac("mul", "r11.xyzw", "a108", "v2.xxxx", NULL);      /* skinned */
    if (normalise) {
        mac("dp3", "r0.w", "v3", "v3", NULL);
        ilu("rsq", "r1.xyz", "r0.wwww");
        mac("mul", "r4.xyz", "r1", "v3.xyzz", NULL);
    } else {
        mac("mov", "r4.xyzw", "v3", NULL, NULL);
    }
    mac("mov", "r7.xyzw", "r4", NULL, NULL);
    mac("mov", "r10.xyzw", "v0", NULL, NULL);
    mac("dph", "r5.x", "r10", "c159", NULL);
    mac("dph", "r5.y", "r10", "c160", NULL);
    mac("dph", "r5.z", "r10", "c161", NULL);
    ambient("r4", 0, "r2", "r8");
    if (swap_rows) {                    /* channel 1's rows 0 and 1 swapped */
        mac("dph", "r3.x", "r7", "c171", NULL);
        mac("dph", "r3.y", "r7", "c170", NULL);
        mac("dph", "r3.z", "r7", "c172", NULL);
        mac("dph", "r3.w", "r7", "c173", NULL);
        mac("dph", "r8.y", "r7", "r3", NULL);
    } else {
        ambient("r7", 1, "r3", "r8");
    }
    ambient("r4", 2, "r2", "r8");
    blobs(0, "r5", "r9", 0);
    blobs(1, "r5", "r9", 1);
    blobs(2, "r5", "r9", 2);
    mac("dp3", "r11.x", "-c179.xyzz", "r4", NULL);
    ilu("lit", "r1.y", "r11");
    mac("dph", "r11.x", "-r9.xyzz", "r1.yyyy", NULL);
    mac("mov", "o3.xyz", "r8", NULL, NULL);
    mac("mul", "o4.xyz", "r11.xxxx", key_colour, NULL);
    mac("dph", "o4.w", "-r9.xyzz", "c191.wwww", NULL);
}

int main(void)
{
    Nv2aVshLit L;
    uint32_t k;

    printf("no pattern set:\n");
    prog_a("c180.xyzz", 1, 0);
    CHECK(!scan(&L), "a scan without a pattern");

    xbox_SetLitPattern(179, 180, 166, 146, 3);

    printf("layout A, 3 groups:\n");
    prog_a("c180.xyzz", 1, 0);
    CHECK(scan(&L) && L.layout == NV2A_LIT_A, "layout A");
    CHECK(L.groups == 7 && L.skinned, "groups 7, skinned");
    k = 0;
    while (k < np && !(P[k].mac == 5 && P[k].cidx == 179)) k++;
    CHECK(L.n_ins == k && L.n_src == 1 && L.n_temp == 4,
          "normal: source B of %u, r4 (got %u %u %u)", k, L.n_ins, L.n_src,
          L.n_temp);
    CHECK(L.p_ins[0] == 6 && L.p_ins[1] == 7 && L.p_ins[2] == 8
          && L.p_unit[0] == 0 && L.p_comp[0] == 0 && L.p_comp[1] == 1
          && L.p_comp[2] == 2, "blob coordinates at 6/7/8 lanes x/y/z");
    CHECK(L.d1w == NV2A_LIT_W_BLOBS_MUL && L.d1w_row == 191
          && L.d1w_comp == 3, "oD1.w = c191.w x (1 - blobs)");

    printf("layout A, wrong key colour row:\n");
    prog_a("c181.xyzz", 1, 0);
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE, "per vertex");

    printf("layout A, normal not normalised:\n");
    prog_a("c180.xyzz", 0, 0);
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE && L.why
          && strstr(L.why, "normalised"), "per vertex: not normalised");

    printf("layout A, ambient rows out of order:\n");
    prog_a("c180.xyzz", 1, 1);
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE, "per vertex");

    /* Layout B, one group through a MAD, the normal's rsq paired with a
     * MAC op (so it lands in R1), operands the other way round, the
     * factor as source A and a broadcast input lane. */
    printf("layout B, 1 group:\n");
    begin();
    mac("dp3", "r3.w", "v2", "v2", NULL);
    pair("dph", "r9.x", "v0", "c159", "r3.wwww", "rsq", "r1.xyz");
    mac("mul", "r6.xyz", "r1", "v2.xyzz", NULL);
    mac("dph", "r9.y", "v0", "c160", NULL);
    mac("dph", "r9.z", "v0", "c161", NULL);
    ambient("r6", 2, "r2", "r7");
    ambient("r6", 0, "r3", "r7");
    ambient("r6", 1, "r2", "r7");
    mac("add", "r0.xyzw", "-r9.xxxx", NULL, "c146");
    mac("add", "r2.xyzw", "c147", NULL, "-r9.yyyy");
    mac("mul", "r2.xyzw", "r2", "r2", NULL);
    mac("mad", "r0.xyzw", "r0", "r0", "r2");
    mac("sge", "r4.xyzw", "r9.zzzz", "c148", NULL);
    mac("mul", "r4.xyzw", "c149", "r4", NULL);
    ilu("rcc", "r1.x", "r0.xxxx");
    ilu("rcc", "r1.y", "r0.yyyy");
    ilu("rcc", "r1.z", "r0.zzzz");
    ilu("rcc", "r1.w", "r0.wwww");
    mac("dp4", "r5.x", "r4", "r1", NULL);
    mac("dp3", "r8.x", "r6", "-c179", NULL);
    ilu("lit", "r1.y", "r8");
    mac("mad", "r8.x", "r1.yyyy", "-r5.xxxx", "r1.yyyy");
    k = np;
    mac("mul", "o3.xyz", "v6.xxxx", "r7", NULL);
    mac("mul", "o4.xyz", "c180.xyzz", "r8.xxxx", NULL);
    mac("add", "o4.w", "-r5.xxxx", NULL, "c191.wwww");
    CHECK(scan(&L) && L.layout == NV2A_LIT_B, "layout B");
    CHECK(L.groups == 1 && !L.skinned, "group 0 only, not skinned");
    CHECK(L.f_ins == k && L.f_src == 0 && L.f_reg == 6,
          "factor: source A of %u, v6 (got %u %u %u)", k, L.f_ins, L.f_src,
          L.f_reg);
    CHECK(L.n_src == 0 && L.n_temp == 6, "normal: source A, r6");
    CHECK(L.p_ins[0] == 1 && L.p_ins[1] == 3 && L.p_ins[2] == 4,
          "blob coordinates at 1/3/4");
    CHECK(L.d1w == NV2A_LIT_W_BLOBS_SUB, "oD1.w = c191.w - blobs");

    printf("layout B, strength from another group's row:\n");
    for (k = 0; k < np; k++)
        if (P[k].mac == 2 && P[k].cidx == 149)
            P[k].cidx = 153;
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE, "per vertex");

    /* Layout C, no blobs, the normal negated into the key's dot. */
    printf("layout C, no blobs:\n");
    begin();
    mac("dp3", "r0.w", "v3", "v3", NULL);
    ilu("rsq", "r1.xyz", "r0.wwww");
    mac("mul", "r2.xyz", "v3.xyzz", "r1", NULL);
    ambient("r2", 0, "r3", "r5");
    ambient("r2", 1, "r3", "r5");
    ambient("r2", 2, "r3", "r5");
    mac("dp3", "r4.x", "-r2", "c179", NULL);
    ilu("lit", "r1.y", "r4");
    mac("mad", "o3.xyz", "r1.yyyy", "c180.xyzz", "r5");
    CHECK(scan(&L) && L.layout == NV2A_LIT_C && L.groups == 0, "layout C");
    CHECK(L.p_ins[0] == 0xFFFF, "no blob coordinates");

    printf("layout C, the key not negated:\n");
    for (k = 0; k < np; k++)
        if (P[k].mac == 5 && P[k].cidx == 179)
            P[k].src[0].neg = 0;
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE, "per vertex");

    /* Layout D: the term floored, then times the colour plus the ambient
     * times an input's lane; one group, its coordinates from ambient
     * rows (programs that place nothing in the light's frame do that). */
    printf("layout D, floored term:\n");
    begin();
    mac("dp3", "r0.w", "v4", "v4", NULL);
    ilu("rsq", "r1.xyz", "r0.wwww");
    mac("mul", "r2.xyz", "r1", "v4.xyzz", NULL);
    ambient("r2", 0, "r4", "r5");
    ambient("r2", 1, "r6", "r5");
    mac("dph", "r7.x", "r2", "c174", NULL);
    mac("dph", "r7.y", "r2", "c175", NULL);
    mac("dph", "r7.z", "r2", "c176", NULL);
    mac("dph", "r7.w", "r2", "c177", NULL);
    mac("dph", "r5.z", "r2", "r7", NULL);
    k = np;
    mac("mul", "r8.xyz", "r5.xyzz", "v0.xxxx", NULL);
    blobs(0, "r7", "r9", 0);
    mac("dp3", "r10.x", "r2", "-c179", NULL);
    ilu("lit", "r1.y", "r10");
    mac("mad", "r6.x", "-r9.xxxx", "r1.yyyy", "r1.yyyy");
    mac("max", "r6.x", "r6.xxxx", "c191.zzzz", NULL);
    mac("mad", "o3.xyz", "r6.xxxx", "c180.xyzz", "r8.xyzz");
    CHECK(scan(&L) && L.layout == NV2A_LIT_D, "layout D");
    CHECK(L.lo_row == 191 && L.lo_comp == 2, "term >= c191.z");
    CHECK(L.f_ins == k && L.f_src == 1 && L.f_reg == 0, "factor v0 at %u", k);
    CHECK(L.groups == 1 && L.p_ins[0] == 13 && L.p_ins[1] == 14
          && L.p_ins[2] == 15,
          "blob coordinates from the ambient rows' first stage");

    printf("layout D, a group taken off after the floor:\n");
    begin();
    mac("dp3", "r0.w", "v4", "v4", NULL);
    ilu("rsq", "r1.xyz", "r0.wwww");
    mac("mul", "r2.xyz", "r1", "v4.xyzz", NULL);
    ambient("r2", 0, "r4", "r5");
    ambient("r2", 1, "r6", "r5");
    ambient("r2", 2, "r7", "r5");
    mac("dph", "r3.x", "r2", "c159", NULL);
    mac("dph", "r3.y", "r2", "c160", NULL);
    mac("dph", "r3.z", "r2", "c161", NULL);
    blobs(0, "r3", "r9", 0);
    mac("dp3", "r10.x", "r2", "-c179", NULL);
    ilu("lit", "r1.y", "r10");
    mac("max", "r6.x", "r1.yyyy", "c191.zzzz", NULL);
    mac("mad", "r6.x", "-r9.xxxx", "r1.yyyy", "r6.xxxx");
    mac("mad", "o3.xyz", "r6.xxxx", "c180.xyzz", "r5.xyzz");
    CHECK(scan(&L) && L.layout == NV2A_LIT_NONE, "per vertex");

    printf("no key light dot product:\n");
    begin();
    mac("mul", "r0.xyzw", "v0", "c179", NULL);
    mac("mov", "o3.xyzw", "r0", NULL, NULL);
    CHECK(!scan(&L), "not a candidate");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
