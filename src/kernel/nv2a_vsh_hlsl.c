/**
 * NV2A vertex programs as HLSL. See nv2a_vsh_hlsl.h; the reference for every
 * line written here is run_program in nv2a_vsh_interp.c.
 */
#include "nv2a_vsh_hlsl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The helpers mirror run_mac / run_ilu where one HLSL operator would not:
 * rcp of 0 is +inf whatever the sign of the zero, rcc clamps the result's
 * size, log splits off the exponent from the bits (a GPU log2 of 8 may come
 * out 2.9999998 and floor to 2), lit's pow of 0 is powf's (1 to the power
 * 0, +inf to a negative power, where HLSL's pow gives NaN and 0 on WARP and
 * on hardware). min/max/slt/sge are written as the C compares, so NaNs go
 * the same way.
 *
 * Zeroes that may be negated or divided by come from the cbuffer (z, the
 * row after c[191]), never a literal: fxc folds -0.0 of a known zero to
 * +0.0 even under IEEE strictness, and the CPU's -0 then has an rcc of
 * -1.9e19 where the GPU's is +1.9e19 (tests/nv2a_vsh_gpu, 2026-10-18). */
static const char k_prelude[] =
"struct VshOut {\n"
"  float4 pos, d0, d1, fog, t0, t1, t2, t3; float pts;\n"
"};\n"
"cbuffer VshC : register(b3) { float4 vc[193]; };   /* vc[192]: 0 */\n"
"float4 vsh_c(int i) { return vc[(uint)i < 192u ? (uint)i : 0u]; }\n"
"float vsh_abs(float x) { return asfloat(asuint(x) & 0x7FFFFFFFu); }\n"
"float vsh_rcp(float x) { return x == 0.0 ? asfloat(0x7F800000u) : 1.0 / x; }\n"
"float vsh_rcc(float x) {\n"
"  float f = 1.0 / x, s = f < 0.0 ? -1.0 : 1.0;\n"
"  if (vsh_abs(f) < 5.42101e-20) f = s * 5.42101e-20;\n"
"  if (vsh_abs(f) > 1.884467e19) f = s * 1.884467e19;\n"
"  return f;\n"
"}\n"
"float4 vsh_exp(float x) {\n"
"  float fl = floor(x);\n"
"  return float4(exp2(fl), x - fl, exp2(x), 1.0);\n"
"}\n"
"float4 vsh_log(float x) {\n"
"  float ax = vsh_abs(x); uint b = asuint(ax);\n"
"  if (ax == 0.0) return float4(asfloat(0xFF800000u), 1.0,\n"
"                               asfloat(0xFF800000u), 1.0);\n"
"  if (!(ax < asfloat(0x7F800000u))) {   /* inf: inf NaN inf; NaN: NaN */\n"
"    float l = log2(ax);\n"
"    return float4(l, l - l, l, 1.0);\n"
"  }\n"
"  return float4((float)((int)(b >> 23) - 127),\n"
"                asfloat((b & 0x007FFFFFu) | 0x3F800000u), log2(ax), 1.0);\n"
"}\n"
"float4 vsh_lit(float4 c) {\n"
"  float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
"  float p = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);\n"
"  float sp = p == 0.0 ? 1.0\n"
"           : nh == 0.0 ? (p < 0.0 ? asfloat(0x7F800000u) : (p > 0.0 ? 0.0 : p))\n"
"           : pow(nh, p);\n"
"  return float4(1.0, nl, c.x > 0.0 ? sp : 0.0, 1.0);\n"
"}\n"
"VshOut vsh_prog(float4 v[16]) {\n"
"  /* The temps start at zero read from the cbuffer: a literal 0 lets the\n"
"     compiler fold -R of an unwritten register to +0 (the CPU's is -0, and\n"
"     an rcc of it -1.9e19), strictness or not. */\n"
"  float4 z = vc[192];\n"
"  float4 r0 = z, r1 = z, r2 = z, r3 = z, r4 = z, r5 = z;\n"
"  float4 r6 = z, r7 = z, r8 = z, r9 = z, r10 = z, r11 = z, opos = z;\n"
"  float4 o3 = float4(0, 0, 0, 1), o4 = float4(0, 0, 0, 1), o5 = 0, o6 = 0;\n"
"  float4 o9 = float4(0, 0, 0, 1), o10 = float4(0, 0, 0, 1);\n"
"  float4 o11 = float4(0, 0, 0, 1), o12 = float4(0, 0, 0, 1);\n"
"  int a0 = 0;\n";

static const char k_epilogue[] =
"  {\n"
"    VshOut o;\n"
"    o.pos = opos; o.d0 = saturate(o3); o.d1 = saturate(o4); o.fog = o5;\n"
"    o.t0 = o9; o.t1 = o10; o.t2 = o11; o.t3 = o12; o.pts = o6.x;\n"
"    return o;\n"
"  }\n"
"}\n"
"struct VshIn {\n"
"  float4 x0 : ATTR0; float4 x1 : ATTR1; float4 x2 : ATTR2;\n"
"  float4 x3 : ATTR3; float4 x4 : ATTR4; float4 x5 : ATTR5;\n"
"  float4 x6 : ATTR6; float4 x7 : ATTR7; float4 x8 : ATTR8;\n"
"  float4 x9 : ATTR9; float4 x10 : ATTR10; float4 x11 : ATTR11;\n"
"  float4 x12 : ATTR12; float4 x13 : ATTR13; float4 x14 : ATTR14;\n"
"  float4 x15 : ATTR15;\n"
"};\n"
"struct VshCheck {\n"
"  float4 pos : TEXCOORD0; float4 d0 : TEXCOORD1; float4 d1 : TEXCOORD2;\n"
"  float4 fog : TEXCOORD3; float4 t0 : TEXCOORD4; float4 t1 : TEXCOORD5;\n"
"  float4 t2 : TEXCOORD6; float4 t3 : TEXCOORD7; float pts : TEXCOORD8;\n"
"};\n"
"VshCheck vs_check(VshIn i) {\n"
"  float4 v[16] = { i.x0, i.x1, i.x2, i.x3, i.x4, i.x5, i.x6, i.x7,\n"
"                   i.x8, i.x9, i.x10, i.x11, i.x12, i.x13, i.x14, i.x15 };\n"
"  VshOut r = vsh_prog(v);\n"
"  VshCheck o;\n"
"  o.pos = r.pos; o.d0 = r.d0; o.d1 = r.d1; o.fog = r.fog;\n"
"  o.t0 = r.t0; o.t1 = r.t1; o.t2 = r.t2; o.t3 = r.t3; o.pts = r.pts;\n"
"  return o;\n"
"}\n";

typedef struct {
    char *buf;
    size_t cap, len;
    int full;
} Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->full)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
        o->full = 1;
    else
        o->len += (size_t)n;
}

/* A temp register by number, as read_dsrc and the writes take them: 12 is
 * oPos; reads of 13-15 read R0. */
static const char *temp_name(uint32_t r, char *b)
{
    if (r == 12)
        return "opos";
    sprintf(b, "r%u", r < 12 ? r : 0);
    return b;
}

/* An output register by o[] address, or NULL where run_program's value
 * goes nowhere this returns (1, 2, 7, 8: back colours and unused). */
static const char *out_name(uint32_t a)
{
    static const char *const k[13] = {
        "opos", NULL, NULL, "o3", "o4", "o5", "o6", NULL, NULL,
        "o9", "o10", "o11", "o12"
    };
    return a < 13 ? k[a] : NULL;
}

static void mask_text(uint32_t m, char *b)
{
    *b++ = '.';
    if (m & 8) *b++ = 'x';
    if (m & 4) *b++ = 'y';
    if (m & 2) *b++ = 'z';
    if (m & 1) *b++ = 'w';
    *b = 0;
}

static void src_text(Out *o, const Nv2aVshIns *I, int which)
{
    static const char sw[] = "xyzw";
    const Nv2aVshSrc *d = &I->src[which];
    char t[8];

    put(o, "%s", d->neg ? "-" : "");
    if (d->mux == 1)
        put(o, "%s", temp_name(d->reg, t));
    else if (d->mux == 2)
        put(o, "v[%u]", I->iidx);
    else if (I->rel)
        put(o, "vsh_c(%d + a0)", (int)I->cidx);
    else
        put(o, "vc[%u]", I->cidx < NV2A_VSH_CONSTANTS ? I->cidx : 0u);
    put(o, ".%c%c%c%c", sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]],
        sw[d->sw[3]]);
}

/* run_mac's result as an expression of A, B, C. */
static const char *mac_expr(uint32_t op)
{
    switch (op) {
    case 2:  return "A * B";
    case 3:  return "A + C";
    case 4:  return "A * B + C";
    case 5:  return "(float4)dot(A.xyz, B.xyz)";
    case 6:  return "(float4)(dot(A.xyz, B.xyz) + B.w)";
    case 7:  return "(float4)dot(A, B)";
    case 8:  return "float4(1.0, A.y * B.y, A.z, B.w)";
    case 9:  return "A < B ? A : B";
    case 10: return "A >= B ? A : B";
    case 11: return "(float4)(A < B)";
    case 12: return "(float4)(A >= B)";
    default: return "A";                 /* mov, arl, the two unknown ops */
    }
}

static const char *ilu_expr(uint32_t op)
{
    switch (op) {
    case 2:  return "(float4)vsh_rcp(C.x)";
    case 3:  return "(float4)vsh_rcc(C.x)";
    case 4:  return "(float4)rsqrt(vsh_abs(C.x))";
    case 5:  return "vsh_exp(C.x)";
    case 6:  return "vsh_log(C.x)";
    case 7:  return "vsh_lit(C)";
    default: return "C";                 /* mov */
    }
}

static void put_ins(Out *o, const Nv2aVshIns *I, uint32_t n)
{
    static const char abc[] = "ABC";
    char m[8], t[8];
    int k;

    put(o, "  { /* %u: mac %u ilu %u */\n", n, I->mac, I->ilu);
    /* Every source first: both units read before either writes. */
    for (k = 0; k < 3; k++)
        if (I->need & (1 << k)) {
            put(o, "    float4 %c = ", abc[k]);
            src_text(o, I, k);
            put(o, ";\n");
        }
    if (I->mac)
        put(o, "    float4 M = %s;\n", mac_expr(I->mac));
    if (I->ilu)
        put(o, "    float4 I = %s;\n", ilu_expr(I->ilu));
    if (I->mac == 13)
        put(o, "    a0 = (int)floor(A.x + 0.001);\n");

    if (I->mac && I->mac != 13 && I->mac_mask && I->tdst <= 12) {
        mask_text(I->mac_mask, m);
        put(o, "    %s%s = M%s;\n", temp_name(I->tdst, t), m, m);
    }
    if (I->ilu && I->ilu_mask) {
        uint32_t it = I->mac ? 1u : I->tdst;     /* the R1 rule, run_program */
        if (it <= 12) {
            mask_text(I->ilu_mask, m);
            put(o, "    %s%s = I%s;\n", temp_name(it, t), m, m);
        }
    }
    /* Outputs; a write to c[] needs CXT_WRITE_EN, whose programs stay on
     * the CPU, so it is dropped as run_program drops it without. */
    if (I->omask && I->out_o && out_name(I->oaddr)) {
        int have = I->out_ilu ? I->ilu != 0 : I->mac != 0;
        mask_text(I->omask, m);
        if (have)
            put(o, "    %s%s = %c%s;\n", out_name(I->oaddr), m,
                I->out_ilu ? 'I' : 'M', m);
        else             /* an idle unit's result is 0 (z: see the prelude) */
            put(o, "    %s%s = z%s;\n", out_name(I->oaddr), m, m);
    }
    put(o, "  }\n");
}

size_t nv2a_vsh_hlsl(const Nv2aVshIns *ins, uint32_t count, char *buf,
                     size_t cap)
{
    Out o;
    uint32_t n;

    if (!ins || !count || !cap)
        return 0;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    o.full = 0;
    buf[0] = 0;
    put(&o, "%s", k_prelude);
    for (n = 0; n < count; n++)
        put_ins(&o, &ins[n], n);
    put(&o, "%s", k_epilogue);
    return o.full ? 0 : o.len;
}

/* ---- vertex inputs ----------------------------------------------------- */

/* nv2a_attr_decode in HLSL. Elements need not be 4-byte aligned (a ubyte or
 * short attribute at an odd offset), so every read is of the dword at a byte
 * address, from the two aligned ones around it; bytes past the end of the
 * copy read as zeroes and are masked off. The normalising divides are
 * divides, as the C's: no multiply by a reciprocal. */
static const char k_fetch[] =
"ByteAddressBuffer vin : register(t0);\n"
"cbuffer VshA : register(b4) { uint4 va[16]; };\n"
"uint vsh_ld(uint a) {\n"
"  uint s = (a & 3u) * 8u, b = a & ~3u, lo = vin.Load(b);\n"
"  return s == 0u ? lo : (lo >> s) | (vin.Load(b + 4u) << (32u - s));\n"
"}\n"
"float vsh_s16(uint x) { return (float)((int)(x << 16) >> 16); }\n"
"float4 vsh_fetch(uint k, uint i) {\n"
"  uint4 d = va[k];\n"
"  uint kind = d.z & 0xFFu, n = min(d.z >> 8, 4u), a = d.x + i * d.y;\n"
"  float4 r = float4(0, 0, 0, 1);\n"
"  if (kind == 0u || i > d.w) return r;\n"
"  if (kind == 1u) {                          /* D3DCOLOR: B,G,R,A bytes */\n"
"    uint c = vsh_ld(a);\n"
"    return float4((float)((c >> 16) & 255u) / 255.0,\n"
"                  (float)((c >> 8) & 255u) / 255.0,\n"
"                  (float)(c & 255u) / 255.0, (float)(c >> 24) / 255.0);\n"
"  }\n"
"  if (kind == 7u) {                          /* packed 11:11:10 */\n"
"    uint c = vsh_ld(a);\n"
"    r.x = (float)((int)(c << 21) >> 21) / 1023.0;\n"
"    r.y = (float)((int)(c << 10) >> 21) / 1023.0;\n"
"    r.z = (float)((int)c >> 22) / 511.0;\n"
"    return r;\n"
"  }\n"
"  [unroll] for (uint j = 0; j < 4u; j++) {\n"
"    if (j < n) {\n"
"      if (kind == 3u) r[j] = asfloat(vsh_ld(a + 4u * j));\n"
"      else if (kind == 5u) r[j] = (float)(vsh_ld(a + j) & 255u) / 255.0;\n"
"      else if (kind == 2u) r[j] = vsh_s16(vsh_ld(a + 2u * j)) / 32767.0;\n"
"      else if (kind == 6u) r[j] = vsh_s16(vsh_ld(a + 2u * j));\n"
"    }\n"
"  }\n"
"  return r;\n"
"}\n";

static const char k_fetch_check[] =
"struct VshFetched {\n"
"  float4 a0 : TEXCOORD0; float4 a1 : TEXCOORD1; float4 a2 : TEXCOORD2;\n"
"  float4 a3 : TEXCOORD3; float4 a4 : TEXCOORD4; float4 a5 : TEXCOORD5;\n"
"  float4 a6 : TEXCOORD6; float4 a7 : TEXCOORD7; float4 a8 : TEXCOORD8;\n"
"  float4 a9 : TEXCOORD9; float4 a10 : TEXCOORD10; float4 a11 : TEXCOORD11;\n"
"  float4 a12 : TEXCOORD12; float4 a13 : TEXCOORD13;\n"
"  float4 a14 : TEXCOORD14; float4 a15 : TEXCOORD15;\n"
"};\n"
"VshFetched vs_fetch(uint i : SV_VertexID) {\n"
"  VshFetched o;\n"
"  o.a0 = vsh_fetch(0, i); o.a1 = vsh_fetch(1, i); o.a2 = vsh_fetch(2, i);\n"
"  o.a3 = vsh_fetch(3, i); o.a4 = vsh_fetch(4, i); o.a5 = vsh_fetch(5, i);\n"
"  o.a6 = vsh_fetch(6, i); o.a7 = vsh_fetch(7, i); o.a8 = vsh_fetch(8, i);\n"
"  o.a9 = vsh_fetch(9, i); o.a10 = vsh_fetch(10, i);\n"
"  o.a11 = vsh_fetch(11, i); o.a12 = vsh_fetch(12, i);\n"
"  o.a13 = vsh_fetch(13, i); o.a14 = vsh_fetch(14, i);\n"
"  o.a15 = vsh_fetch(15, i);\n"
"  return o;\n"
"}\n";

size_t nv2a_vsh_hlsl_fetch(char *buf, size_t cap)
{
    Out o;

    if (!cap)
        return 0;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    o.full = 0;
    buf[0] = 0;
    put(&o, "%s", k_fetch);
    put(&o, "%s", k_fetch_check);
    return o.full ? 0 : o.len;
}

/* ---- comparing results ------------------------------------------------- */

int nv2a_vsh_outputs_close(const Nv2aVshOutput *a, const Nv2aVshOutput *b)
{
    const float *p = (const float *)a, *q = (const float *)b;
    int i, n = (int)(sizeof *a / sizeof(float));

    for (i = 0; i < n; i++) {
        float x = p[i], y = q[i];
        float m;
        if (x != x || y != y) {
            if (x != x && y != y)
                continue;
            return i;
        }
        if (isinf(x) || isinf(y)) {
            if (x == y)
                continue;
            return i;
        }
        m = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
        if (fabsf(x - y) > 1e-4f * m + 1e-3f)
            return i;
    }
    return -1;
}

const char *nv2a_vsh_output_name(int field)
{
    static const char *const reg[8] = {
        "pos", "d0", "d1", "fog", "t0", "t1", "t2", "t3"
    };
    static char b[8][8];
    static int k;
    char *s;

    if (field == 32)
        return "pts";
    if (field < 0 || field > 32)
        return "?";
    s = b[k++ & 7];
    snprintf(s, 8, "%s.%c", reg[field / 4], "xyzw"[field % 4]);
    return s;
}
