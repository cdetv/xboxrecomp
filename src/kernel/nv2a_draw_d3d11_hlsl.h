/**
 * The D3D11 back end's one shader pair, as HLSL source (nv2a_draw_d3d11.c
 * compiles it at start-up).
 *
 * The vertex shader takes what the software path's vertex stage produced --
 * screen-space x, y, z with the clip-space w kept, as nv2a_vsh_run leaves
 * them -- and maps pixels back to clip space, times w so the GPU divides it
 * out again and interpolates texture coordinates with perspective. Colours
 * and fog interpolate across the screen without it, as xf_rows does. z is
 * in the zeta format's units (0..0xFFFFFF for Z24) and goes to the depth
 * buffer as 0..1; like xf_rows' z it interpolates linearly on the screen.
 *
 * The pixel shader has three paths, chosen per batch (mode.x):
 *   0  flat: the texel alone, or the triangle's first vertex colour
 *      (raster_triangle: fixed-function batches, no combiners);
 *   1  texel times the interpolated colour (xf_rows without combiners);
 *   2  the register combiners, evaluated from their registers in the
 *      constant buffer: a line-for-line port of nv2a_rc_eval
 *      (nv2a_combiner.c) and of rc_stage_fetch (nv2a_draw_sw.c). Keep the
 *      three in step; the software ones are the reference.
 * Alpha test (mode.z, the GL function; 0 off) follows alpha_test_pass.
 *
 * 2D textures sample their mip chain (SampleGrad, with the coordinates'
 * derivatives taken once at the top of ps, outside any branch); the
 * software sampler reads level 0 only, so distant surfaces differ.
 * ponytail: volumes and cube maps level 0 only (SampleLevel 0); the
 * dot-product stage modes read as the software path's fallbacks do, near
 * enough: an unbound stage reads white.
 */
#ifndef NV2A_DRAW_D3D11_HLSL_H
#define NV2A_DRAW_D3D11_HLSL_H

/* What both vertex shaders need -- this one (vs) and the one compiled per
 * vertex program (vs_gpu, s_hlsl_vsh_draw) -- and the pixel shader reads:
 * the constant buffer, the vertex the pixel shader takes, the fog unit. */
#define NV2A_D3D11_HLSL_COMMON \
"cbuffer C : register(b0) {\n" \
"  float4 rt;          /* x*rt.x+rt.z, y*rt.y+rt.w: pixels to NDC */\n" \
"  float4 tscale[4];   /* per stage: xy coordinates to normalised;\n" \
"                         z alpha reads 1, w red/blue swap (a surface\n" \
"                         sampled in another format, RTT_* flags) */\n" \
"  uint4  mode;        /* path, bound-stage bits, alpha func, alpha ref */\n" \
"  uint4  stage;       /* stage program, clip-plane mode, control, fog */\n" \
"  uint4  icw[8];      /* colour ICW, alpha ICW, colour OCW, alpha OCW */\n" \
"  float4 cf0[8];      /* FACTOR0 per stage */\n" \
"  float4 cf1[8];      /* FACTOR1 per stage */\n" \
"  uint4  fin;         /* final combiner words 0, 1 */\n" \
"  float4 fc0, fc1;    /* final combiner constants */\n" \
"  float4 fogc;        /* fog colour, r g b */\n" \
"  float4 fogp;        /* fog parameters 0, 1 */\n" \
"  float4 zsc;         /* x: guest depth units to 0..1 (1/0xFFFFFF Z24) */\n" \
"  float4 bmat[4];     /* per stage: bump-env matrix 00 01 10 11 */\n" \
"  float4 bsc[4];      /* per stage: bump scale, offset, input stage */\n" \
"};\n" \
"/* Colours and fog interpolate with perspective, as xemu's smooth ones do.\n" \
"   Not noperspective: the GPU path leaves near-plane clipping to the\n" \
"   rasteriser, and a strip running from behind the eye (Conker's bullet\n" \
"   smoke, w -777 to 720) came out as opaque bands across the screen:\n" \
"   the cut vertices' screen-linear colours are not clip_triangle's\n" \
"   (2026-10-20). The software path still interpolates them linearly. */\n" \
"struct V {\n" \
"  float4 p : SV_Position;\n" \
"  nointerpolation float4 cf : COLOR0;\n" \
"  float4 d0 : COLOR1;\n" \
"  float4 d1 : COLOR2;\n" \
"  float fog : FOG;\n" \
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n" \
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n" \
"};\n" \
"\n" \
"/* fog_factor (nv2a_draw_sw.c): the fog unit, per vertex. */\n" \
"float fog_factor(float d) {\n" \
"  uint m = stage.w & 0xFFFF; float f;\n" \
"  if (!(stage.w & 0x10000)) return 1.0;\n" \
"  if (m == 0x800 || m == 0x802) f = fogp.x + exp2(d * fogp.y * 16.0) - 1.5;\n" \
"  else if (m == 0x801 || m == 0x803)\n" \
"    f = fogp.x + exp2(-d * d * fogp.y * fogp.y * 32.0) - 1.5;\n" \
"  else f = fogp.x + d * fogp.y - 1.0;\n" \
"  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n" \
"  if (!(f == f)) f = 1.0;\n" \
"  return saturate(f);\n" \
"}\n"

static const char s_hlsl[] =
"Texture2D t0 : register(t0); Texture2D t1 : register(t1);\n"
"Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n"
"/* Volume textures of stages that sample in 3D (mode.y bits 4-7). */\n"
"Texture3D v0 : register(t7); Texture3D v1 : register(t8);\n"
"Texture3D v2 : register(t9); Texture3D v3 : register(t10);\n"
"/* Cube maps of stages that have one (mode.y bits 8-11): the six faces\n"
"   +X -X +Y -Y +Z -Z as slices, picked as rc_stage_fetch picks them. */\n"
"Texture2DArray c0 : register(t11); Texture2DArray c1 : register(t12);\n"
"Texture2DArray c2 : register(t13); Texture2DArray c3 : register(t14);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
NV2A_D3D11_HLSL_COMMON
"struct VI {\n"
"  float4 p : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
"\n"
"V vs(VI i) {\n"
"  V o; float w = i.p.w;\n"
"  o.p = float4((i.p.x * rt.x + rt.z) * w, (i.p.y * rt.y + rt.w) * w,\n"
"               i.p.z * zsc.x * w, w);\n"
"  o.cf = i.d0; o.d0 = i.d0; o.d1 = i.d1; o.fog = fog_factor(i.fog.x);\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
"\n"
"/* One stage's texel at coordinates c; an unbound stage reads white.\n"
"   g: the coordinates' screen derivatives (ddx in xy, ddy in zw), which\n"
"   pick the mip level; taken in ps, as a derivative inside a branch is\n"
"   not defined for every pixel. */\n"
"float4 texel(Texture2D t, SamplerState s, uint st, float2 c, float4 g) {\n"
"  float4 x; float2 k = tscale[st].xy;\n"
"  if (!((mode.y >> st) & 1)) return float4(1, 1, 1, 1);\n"
"  x = t.SampleGrad(s, c * k, g.xy * k, g.zw * k);\n"
"  if (tscale[st].z != 0.0) x.a = 1.0;\n"
"  if (tscale[st].w != 0.0) x = x.bgra;\n"
"  return x;\n"
"}\n"
"\n"
"/* A volume stage's texel at (s, t, r), already normalised (volumes are\n"
"   swizzled). Conker picks a slice of its 64x64x16 grime volume per draw\n"
"   through r; read in 2D it only ever got slice 0, the clean one. */\n"
"float4 texel3(Texture3D t, SamplerState s, uint st, float3 c) {\n"
"  float4 x = t.SampleLevel(s, c, 0);\n"
"  if (tscale[st].z != 0.0) x.a = 1.0;\n"
"  if (tscale[st].w != 0.0) x = x.bgra;\n"
"  return x;\n"
"}\n"
"\n"
"/* A cube stage's texel in face f at (u, v), normalised (cube maps are\n"
"   swizzled); face 0 for the modes that read a cube in 2D. */\n"
"float4 texelc(Texture2DArray t, SamplerState s, uint st, float2 c, uint f) {\n"
"  float4 x = t.SampleLevel(s, float3(c, f), 0);\n"
"  if (tscale[st].z != 0.0) x.a = 1.0;\n"
"  if (tscale[st].w != 0.0) x = x.bgra;\n"
"  return x;\n"
"}\n"
"\n"
"/* rc_stage_fetch's CUBEMAP mode: the face by the largest axis of\n"
"   (s, t, r), then the other two over it. Shiny metal (lamp posts, bells,\n"
"   door fittings) and bottles; unbound, they all came out white. */\n"
"float4 cube_fetch(Texture2DArray t, SamplerState s, uint st, float3 c) {\n"
"  float3 a = abs(c); float ma, sc, tc; uint f;\n"
"  if (a.x >= a.y && a.x >= a.z) {\n"
"    f = c.x > 0.0 ? 0 : 1; ma = a.x; sc = c.x > 0.0 ? -c.z : c.z; tc = -c.y;\n"
"  } else if (a.y >= a.z) {\n"
"    f = c.y > 0.0 ? 2 : 3; ma = a.y; sc = c.x; tc = c.y > 0.0 ? c.z : -c.z;\n"
"  } else {\n"
"    f = c.z > 0.0 ? 4 : 5; ma = a.z; sc = c.z > 0.0 ? c.x : -c.x; tc = -c.y;\n"
"  }\n"
"  if (ma == 0.0) ma = 1.0;\n"
"  return texelc(t, s, st, float2(sc / ma + 1.0, tc / ma + 1.0) * 0.5, f);\n"
"}\n"
"\n"
"/* sign3 (nv2a_draw_sw.c): a two's-complement byte read as unsigned. */\n"
"float sign3(float x) {\n"
"  float b = floor(x * 255.0 + 0.5);\n"
"  return b >= 128.0 ? (b - 256.0) / 127.0 : b / 127.0;\n"
"}\n"
"\n"
"/* rc_stage_fetch: what stage st contributes, by its program mode; P holds\n"
"   the earlier stages' results (bump-env reads one). */\n"
"float4 stage_fetch(Texture2D t, Texture3D v, Texture2DArray cu,\n"
"                   SamplerState s, uint st, float4 c, float4 g,\n"
"                   float4 P[4]) {\n"
"  uint m = (stage.x >> (st * 5)) & 31, j;\n"
"  bool cb = (mode.y >> (st + 8)) & 1;\n"
"  if (m == 0) return float4(0, 0, 0, 1);\n"
"  if (cb) {\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    if (m == 3) return cube_fetch(cu, s, st, c.xyz);\n"
"    if (m == 1 || m == 2) return texelc(cu, s, st, c.xy / q, 0);\n"
"    if (m == 6 || m == 7) return float4(1, 1, 1, 1);\n"
"  }\n"
"  if ((m == 6 || m == 7) && st > 0) {\n"
"    float4 p = P[(uint)bsc[st].z], x;\n"
"    float du = sign3(p.b), dv = sign3(p.g);\n"
"    x = texel(t, s, st, c.xy + float2(bmat[st].x * du + bmat[st].z * dv,\n"
"                                      bmat[st].y * du + bmat[st].w * dv), g);\n"
"    if (m == 7) x *= p.r * bsc[st].x + bsc[st].y;\n"
"    return x;\n"
"  }\n"
"  if (m == 1 || m == 2) {\n"
"    float q = c.w != 0.0 ? c.w : 1.0;\n"
"    if (m == 2 && ((mode.y >> (st + 4)) & 1))\n"
"      return texel3(v, s, st, c.xyz / q);\n"
"    return texel(t, s, st, c.xy / q, g);\n"
"  }\n"
"  if (m == 3) return texel(t, s, st, c.xy, g);\n"
"  if (m == 4) return saturate(c);\n"
"  if (m == 5) {\n"
"    [unroll] for (j = 0; j < 4; j++) {\n"
"      bool ge = (stage.y >> (st * 4 + j)) & 1;\n"
"      if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard;\n"
"    }\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"\n"
"/* map_in / map_out (nv2a_combiner.c). Every selector here is the same\n"
"   for the whole draw (constant buffer), so the switches branch, cheaply,\n"
"   instead of computing every case. */\n"
"float3 map_in3(float3 x, uint b) {\n"
"  float3 p = max(x, 0.0), r;\n"
"  [forcecase] switch ((b >> 5) & 7) {\n"
"  case 0: r = p; break;\n"
"  case 1: r = 1.0 - saturate(x); break;\n"
"  case 2: r = 2.0 * p - 1.0; break;\n"
"  case 3: r = -2.0 * p + 1.0; break;\n"
"  case 4: r = p - 0.5; break;\n"
"  case 5: r = -p + 0.5; break;\n"
"  case 6: r = x; break;\n"
"  default: r = -x; break;\n"
"  }\n"
"  return r;\n"
"}\n"
"float map_in(float x, uint b) { return map_in3(x.xxx, b).x; }\n"
"float3 map_out3(float3 x, uint m) {\n"
"  float3 r;\n"
"  [forcecase] switch (m) {\n"
"  case 0x08: r = x - 0.5; break;\n"
"  case 0x10: r = x * 2.0; break;\n"
"  case 0x18: r = (x - 0.5) * 2.0; break;\n"
"  case 0x20: r = x * 4.0; break;\n"
"  case 0x30: r = x * 0.5; break;\n"
"  default: r = x; break;\n"
"  }\n"
"  return r;\n"
"}\n"
"float map_out(float x, uint m) { return map_out3(x.xxx, m).x; }\n"
"\n"
"/* The combiner registers, 1-15 (0 reads zero and is never written), as\n"
"   separate variables reached through switches on the register number.\n"
"   As an array indexed at run time (float4 R[16]) they were kept in\n"
"   slow indexable memory, for every pixel of every combiner draw: the\n"
"   limit at high render resolutions (2026-10-14). Same results. */\n"
"struct Regs { float4 x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15; };\n"
"float4 rget(Regs R, uint i) {\n"
"  float4 r;\n"
"  [forcecase] switch (i & 15) {\n"
"  case 1: r = R.x1; break;\n"
"  case 2: r = R.x2; break;\n"
"  case 3: r = R.x3; break;\n"
"  case 4: r = R.x4; break;\n"
"  case 5: r = R.x5; break;\n"
"  case 6: r = R.x6; break;\n"
"  case 7: r = R.x7; break;\n"
"  case 8: r = R.x8; break;\n"
"  case 9: r = R.x9; break;\n"
"  case 10: r = R.x10; break;\n"
"  case 11: r = R.x11; break;\n"
"  case 12: r = R.x12; break;\n"
"  case 13: r = R.x13; break;\n"
"  case 14: r = R.x14; break;\n"
"  case 15: r = R.x15; break;\n"
"  default: r = float4(0, 0, 0, 0); break;\n"
"  }\n"
"  return r;\n"
"}\n"
"void rset_rgb(inout Regs R, uint d, float3 v) {\n"
"  [forcecase] switch (d) {\n"
"  case 1: R.x1.rgb = v; break;\n"
"  case 2: R.x2.rgb = v; break;\n"
"  case 3: R.x3.rgb = v; break;\n"
"  case 4: R.x4.rgb = v; break;\n"
"  case 5: R.x5.rgb = v; break;\n"
"  case 6: R.x6.rgb = v; break;\n"
"  case 7: R.x7.rgb = v; break;\n"
"  case 8: R.x8.rgb = v; break;\n"
"  case 9: R.x9.rgb = v; break;\n"
"  case 10: R.x10.rgb = v; break;\n"
"  case 11: R.x11.rgb = v; break;\n"
"  case 12: R.x12.rgb = v; break;\n"
"  case 13: R.x13.rgb = v; break;\n"
"  case 14: R.x14.rgb = v; break;\n"
"  case 15: R.x15.rgb = v; break;\n"
"  default: break;\n"
"  }\n"
"}\n"
"void rset_a(inout Regs R, uint d, float v) {\n"
"  [forcecase] switch (d) {\n"
"  case 1: R.x1.a = v; break;\n"
"  case 2: R.x2.a = v; break;\n"
"  case 3: R.x3.a = v; break;\n"
"  case 4: R.x4.a = v; break;\n"
"  case 5: R.x5.a = v; break;\n"
"  case 6: R.x6.a = v; break;\n"
"  case 7: R.x7.a = v; break;\n"
"  case 8: R.x8.a = v; break;\n"
"  case 9: R.x9.a = v; break;\n"
"  case 10: R.x10.a = v; break;\n"
"  case 11: R.x11.a = v; break;\n"
"  case 12: R.x12.a = v; break;\n"
"  case 13: R.x13.a = v; break;\n"
"  case 14: R.x14.a = v; break;\n"
"  case 15: R.x15.a = v; break;\n"
"  default: break;\n"
"  }\n"
"}\n"
"float3 rd_rgb(Regs R, uint b) {\n"
"  float4 r = rget(R, b);\n"
"  return map_in3((b & 0x10) ? r.aaa : r.rgb, b);\n"
"}\n"
"float rd_a(Regs R, uint b) {\n"
"  float4 r = rget(R, b);\n"
"  return map_in((b & 0x10) ? r.a : r.b, b);\n"
"}\n"
"\n"
"/* nv2a_rc_eval. Registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0, 5 v1,\n"
" * 8-11 t0-t3, 12 r0, 13 r1, 14 v1+r0, 15 E*F. */\n"
"float4 combine(float4 v0, float4 v1, float fogf, float4 T[4]) {\n"
"  Regs R = (Regs)0; uint s, n = min(stage.z & 0xFF, 8), fl8 = stage.z >> 8;\n"
"  R.x3 = float4(fogc.rgb, fogf); R.x4 = v0; R.x5 = v1;\n"
"  R.x8 = T[0]; R.x9 = T[1]; R.x10 = T[2]; R.x11 = T[3];\n"
"  R.x12.a = (stage.x & 31) ? T[0].a : 1.0;\n"
"  [loop] for (s = 0; s < n; s++) {\n"
"    uint ic = icw[s].x, ai = icw[s].y, oc = icw[s].z, ao = icw[s].w;\n"
"    uint fl = oc >> 12, afl = ao >> 12, d;\n"
"    float3 A, B, Cc, D, pab, pcd, sum, ab, cd, ms;\n"
"    float aA, aB, aC, aD, apab, apcd, asum, aab, acd, ams;\n"
"    bool mux;\n"
"    R.x1 = cf0[(fl8 & 0x010) ? s : 0]; R.x2 = cf1[(fl8 & 0x100) ? s : 0];\n"
"    mux = (fl8 & 1) ? R.x12.a >= 0.5 : ((int)(R.x12.a * 255.0) & 1) != 0;\n"
"    A = rd_rgb(R, ic >> 24); B = rd_rgb(R, ic >> 16);\n"
"    Cc = rd_rgb(R, ic >> 8); D = rd_rgb(R, ic);\n"
"    aA = rd_a(R, ai >> 24); aB = rd_a(R, ai >> 16);\n"
"    aC = rd_a(R, ai >> 8);  aD = rd_a(R, ai);\n"
"    pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"    pcd = (fl & 1) ? dot(Cc, D).xxx : Cc * D;\n"
"    sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    ab = clamp(map_out3(pab, fl & 0x38), -1.0, 1.0);\n"
"    cd = clamp(map_out3(pcd, fl & 0x38), -1.0, 1.0);\n"
"    ms = clamp(map_out3(sum, fl & 0x38), -1.0, 1.0);\n"
"    apab = aA * aB; apcd = aC * aD;\n"
"    asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    aab = clamp(map_out(apab, afl & 0x38), -1.0, 1.0);\n"
"    acd = clamp(map_out(apcd, afl & 0x38), -1.0, 1.0);\n"
"    ams = clamp(map_out(asum, afl & 0x38), -1.0, 1.0);\n"
"    d = (oc >> 4) & 15;\n"
"    rset_rgb(R, d, ab); if (fl & 0x80) rset_a(R, d, ab.b);\n"
"    d = oc & 15;\n"
"    rset_rgb(R, d, cd); if (fl & 0x40) rset_a(R, d, cd.b);\n"
"    rset_rgb(R, (oc >> 8) & 15, ms);\n"
"    rset_a(R, (ao >> 4) & 15, aab);\n"
"    rset_a(R, ao & 15, acd);\n"
"    rset_a(R, (ao >> 8) & 15, ams);\n"
"  }\n"
"  if (fin.x || fin.y) {\n"
"    uint w0 = fin.x, w1 = fin.y, ff = w1 & 0xFF;\n"
"    float3 a, b, A, B, Cc, D, E, F;\n"
"    float4 o;\n"
"    R.x1 = fc0; R.x2 = fc1;\n"
"    a = (ff & 0x40) ? 1.0 - R.x5.rgb : R.x5.rgb;\n"
"    b = (ff & 0x20) ? 1.0 - R.x12.rgb : R.x12.rgb;\n"
"    R.x14 = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"    E = rd_rgb(R, w1 >> 24); F = rd_rgb(R, w1 >> 16);\n"
"    R.x15 = float4(E * F, 0.0);\n"
"    A = rd_rgb(R, w0 >> 24); B = rd_rgb(R, w0 >> 16);\n"
"    Cc = rd_rgb(R, w0 >> 8); D = rd_rgb(R, w0);\n"
"    o.rgb = D + Cc * (1.0 - A) + B * A;\n"
"    o.a = rd_a(R, w1 >> 8);\n"
"    return saturate(o);\n"
"  }\n"
"  return saturate(R.x12);\n"
"}\n"
"\n"
"/* The derivatives of the 2D coordinates stage st samples at: divided by\n"
"   q for the projective modes in the combiner path, as stage_fetch reads\n"
"   them; the bump-env modes shift them per pixel and keep these. */\n"
"float4 grad(uint st, float4 c) {\n"
"  uint m = (stage.x >> (st * 5)) & 31;\n"
"  float q = c.w != 0.0 ? c.w : 1.0;\n"
"  float2 x = (mode.x == 2 && (m == 1 || m == 2)) ? c.xy / q : c.xy;\n"
"  return float4(ddx(x), ddy(x));\n"
"}\n"
"\n"
"float4 ps(V i) : SV_Target {\n"
"  float4 o;\n"
"  float4 g0 = grad(0, i.t0), g1 = grad(1, i.t1);\n"
"  float4 g2 = grad(2, i.t2), g3 = grad(3, i.t3);\n"
"  if (mode.x == 2) {\n"
"    float4 T[4];\n"
"    T[0] = T[1] = T[2] = T[3] = float4(0, 0, 0, 1);\n"
"    T[0] = stage_fetch(t0, v0, c0, s0, 0, i.t0, g0, T);\n"
"    T[1] = stage_fetch(t1, v1, c1, s1, 1, i.t1, g1, T);\n"
"    T[2] = stage_fetch(t2, v2, c2, s2, 2, i.t2, g2, T);\n"
"    T[3] = stage_fetch(t3, v3, c3, s3, 3, i.t3, g3, T);\n"
"    o = combine(i.d0, i.d1, i.fog, T);\n"
"  } else {\n"
"    float4 t = texel(t0, s0, 0, i.t0.xy, g0);\n"
"    if (mode.x == 1) o = t * i.d0;\n"
"    else o = (mode.y & 1) ? t : i.cf;\n"
"  }\n"
"  if (mode.z) {\n"
"    int v = (int)(o.a * 255.0 + 0.5), r = (int)mode.w;\n"
"    bool ok = true;\n"
"    switch (mode.z) {\n"
"    case 0x200: ok = false; break;\n"
"    case 0x201: ok = v <  r; break;\n"
"    case 0x202: ok = v == r; break;\n"
"    case 0x203: ok = v <= r; break;\n"
"    case 0x204: ok = v >  r; break;\n"
"    case 0x205: ok = v != r; break;\n"
"    case 0x206: ok = v >= r; break;\n"
"    }\n"
"    if (!ok) discard;\n"
"  }\n"
"  return o;\n"
"}\n"
"\n"
"/* The zeta copy pass (texture_from_zeta): a depth/stencil buffer read as\n"
"   a colour texture. Each pixel becomes the dword zeta_readback writes to\n"
"   guest memory (Z24 << 8 | stencil), decoded as pb_tex_texel decodes\n"
"   that dword in format zk.x. zk.yz: the buffer's size; past it, 0. */\n"
"Texture2D<float> zd : register(t4); Texture2D<uint2> zst : register(t5);\n"
"cbuffer Z : register(b1) { uint4 zk; };\n"
"float4 zvs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"}\n"
"float4 zps(float4 p : SV_Position) : SV_Target {\n"
"  int3 c = int3(p.xy, 0); uint d = 0, a;\n"
"  if ((uint)c.x < zk.y && (uint)c.y < zk.z)\n"
"    d = min((uint)(zd.Load(c) * 16777215.0 + 0.5), 0xFFFFFFu) << 8\n"
"        | (zst.Load(c).y & 0xFF);\n"
"  if (zk.x == 0x41) a = (d << 24) | (d >> 8);\n"
"  else if (zk.x == 0x40) a = (d << 24) | ((d >> 8) & 0xFF) << 16\n"
"                           | ((d >> 16) & 0xFF) << 8 | (d >> 24);\n"
"  else if (zk.x == 0x3F) a = (d & 0xFF00FF00u) | ((d & 0xFF) << 16)\n"
"                           | ((d >> 16) & 0xFF);\n"
"  else if (zk.x == 0x1E) a = d | 0xFF000000u;\n"
"  else a = d;\n"
"  return float4((a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, a >> 24)\n"
"         / 255.0;\n"
"}\n"
"\n"
"/* The stretch copy (blit_stretch), on zvs: a rectangle of bt sampled\n"
"   bilinearly over the viewport. bk.xy: texture units per target pixel,\n"
"   bk.zw: the rectangle's corner in texture units, bo.xy: the viewport's\n"
"   corner in target pixels. */\n"
"Texture2D bt : register(t6); SamplerState bsm : register(s4);\n"
"cbuffer Bc : register(b2) { float4 bk; float4 bo; };\n"
"float4 bps(float4 p : SV_Position) : SV_Target {\n"
"  return bt.Sample(bsm, (p.xy - bo.xy) * bk.xy + bk.zw);\n"
"}\n";

/* Vertex programs on the GPU (RECOMP_VSH_GPU, nv2a_vsh_gpu.c): each
 * program's vertex shader is s_hlsl_vsh_head, the fetch and the program
 * (nv2a_vsh_hlsl.c), then this. vs_gpu reads the vertex at SV_VertexID (the
 * batch's own index: the draw is indexed with no base), runs the program and
 * maps its screen-space position to clip space as vs does. The GPU then
 * clips; the two clip distances are clip_triangle's planes, w past NV_CLIP_W
 * and z in front of the viewport's near plane (c[58], c[59] as the Xbox D3D
 * epilogue leaves them), so a triangle loses what the CPU path cut off
 * whether or not the depth clip is on. A vertex that is not finite goes to
 * NaN everywhere, which the rasteriser drops with its triangles, as
 * mark_unclipped drops them. The position is precise: two programs that
 * compute it the same way must give the same depth (layered passes test
 * LEQUAL against the base pass), so no fusing or reordering. */
static const char s_hlsl_vsh_head[] = NV2A_D3D11_HLSL_COMMON;
static const char s_hlsl_vsh_draw[] =
"struct VG {\n"
"  float4 p : SV_Position;\n"
"  nointerpolation float4 cf : COLOR0;\n"
"  float4 d0 : COLOR1;\n"
"  float4 d1 : COLOR2;\n"
"  float fog : FOG;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"  float2 cd : SV_ClipDistance0;\n"
"};\n"
"VG vs_gpu(uint id : SV_VertexID) {\n"
"  float4 v[16]; VshOut r; VG o;\n"
"  vsh_inputs(id, v);\n"
"  r = vsh_prog(v);\n"
"  {\n"
"    precise float w = r.pos.w;\n"
"    precise float4 p = float4((r.pos.x * rt.x + rt.z) * w,\n"
"                              (r.pos.y * rt.y + rt.w) * w,\n"
"                              r.pos.z * zsc.x * w, w);\n"
"    o.p = p;\n"
"    o.cd.x = w - 1e-3;\n"
"    o.cd.y = vc[58].z != 0.0 ? (r.pos.z - vc[59].z) / vc[58].z * w : 0.0;\n"
"    if (!all(isfinite(r.pos))) {\n"
"      o.p = asfloat(0x7FC00000u).xxxx; o.cd = asfloat(0x7FC00000u).xx;\n"
"    }\n"
"  }\n"
"  o.cf = r.d0; o.d0 = r.d0; o.d1 = r.d1; o.fog = fog_factor(r.fog.x);\n"
"  o.t0 = r.t0; o.t1 = r.t1; o.t2 = r.t2; o.t3 = r.t3;\n"
"  return o;\n"
"}\n";

#endif
