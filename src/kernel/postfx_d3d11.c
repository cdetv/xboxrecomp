/**
 * The filter chain of the D3D11 back end: what happens to the shown frame on
 * its way to the back buffer, before a host overlay draws on top.
 *
 * shown_to_back (nv2a_draw_d3d11.c) asks postfx_active once per present,
 * held presents too (a paused game, an open menu), so a change shows at
 * once. Every filter off and the frame the back buffer's size: it copies
 * the frame exactly as before and nothing here runs. Any filter on, or a
 * frame of another size (a render scale, or one drawn at an older render
 * resolution): postfx_draw draws the frame through the chain instead.
 *
 * The chain, in order (one full-screen draw per stage that is on):
 *
 *   aa        anti-aliasing: NVIDIA FXAA 3.11 or SMAA 1x (RECOMP_AA=
 *             off|fxaa|smaa; xbox_SetAntiAlias), off by default; their
 *             shaders are compiled the first time each is used. SMAA is
 *             three draws: edges, blend weights, blending
 *   scale     only when the frame is not the target's size: bigger (a
 *             render scale above 100 %, supersampling) is shrunk by area,
 *             each target pixel the average of the frame pixels it covers,
 *             in linear light; smaller is stretched by the upscaler
 *             (RECOMP_UPSCALER=bilinear|fsr; xbox_SetUpscaler): bilinear,
 *             or AMD FidelityFX FSR 1's EASU, compiled the first time it
 *             is used
 *   sharpen   AMD FidelityFX CAS, sharpen-only (RECOMP_SHARPEN, percent;
 *             xbox_SetSharpen), 0 = off; behind FSR's EASU, FSR's own RCAS
 *             instead, at the same setting
 *   picture   gamma, contrast, saturation (RECOMP_GAMMA, RECOMP_CONTRAST,
 *             RECOMP_SATURATION; xbox_SetPicture), each 1.0 = unchanged
 *
 * Every stage but the last draws into an intermediate texture (s_mid[i]
 * for stage i, 16-bit float, the size that stage puts out: the source
 * rectangle's before the scale stage, the target's from it on), which the
 * next one reads; the last draws into the target. Anti-aliasing runs at
 * the source's own size, sharpening at the target's. The picture stage is
 * left out when it would only copy pixel for pixel behind another stage;
 * a bilinear stretch with no sharpening is done by the picture stage
 * itself, one draw. Anti-aliasing and EASU sample by texture coordinates,
 * so a source rectangle that is not its whole texture is first copied out
 * alone (s_in): what lies around it never bleeds in. SMAA always copies,
 * into a typeless texture with an sRGB view as well, so that its last draw
 * blends in linear light as SMAA asks.
 *
 * Values come from the environment once, then only from the setters (no
 * getenv per frame). A menu changes them on the executor thread, between
 * presents; anything else changing them may tear for one frame, harmlessly.
 *
 * RECOMP_POSTFX_FORCE=1 (debug) runs the chain even when every filter is
 * neutral: with RECOMP_BB_DUMP its back buffers must match the copied ones.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include "postfx_d3d11.h"
#include <d3dcompiler.h>

/* ---- settings ------------------------------------------------------------- */

/* Each as float bits in a LONG, swapped whole. */
static volatile LONG s_gamma, s_contrast, s_saturation, s_sharpen;
static volatile LONG s_aa;            /* AA_*, a plain number */
static volatile LONG s_upscale;       /* UP_*, a plain number */
static int s_force;

/* Anti-aliasing (xbox_SetAntiAlias; RECOMP_AA by these names). */
enum { AA_OFF, AA_FXAA, AA_SMAA, AA_MODES };
static const char *const s_aa_name[AA_MODES] = { "off", "fxaa", "smaa" };

/* The upscaler (xbox_SetUpscaler; RECOMP_UPSCALER by these names). */
enum { UP_BILINEAR, UP_FSR, UP_MODES };
static const char *const s_up_name[UP_MODES] = { "bilinear", "fsr" };
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;

static LONG fbits(float f)
{
    LONG l;
    memcpy(&l, &f, sizeof l);
    return l;
}

static float fval(volatile LONG *p)
{
    LONG l = *p;
    float f;
    memcpy(&f, &l, sizeof f);
    return f;
}

/* Wider than a menu offers: the toolkit only refuses nonsense. */
static int in_range(float v, float lo, float hi)
{
    return v >= lo && v <= hi;
}

/* def when unset or not taken. */
static float env_float(const char *name, float lo, float hi, float def)
{
    const char *v = getenv(name);
    char *end;
    float f;

    if (!v || !*v)
        return def;
    f = strtof(v, &end);
    if (*end || !in_range(f, lo, hi)) {
        fprintf(stderr, "[GPU] postfx: %s=%s not a number from %g to %g;"
                " %g%c", name, v, lo, hi, def, 10);
        return def;
    }
    return f;
}

#define GAMMA_LO 0.1f
#define GAMMA_HI 4.0f
#define ADJ_LO   0.0f
#define ADJ_HI   4.0f
#define SHARP_HI 100.0f

/* Variable name's value among names[0..n-1] (any case); 0 when unset or
 * not one of them (said, with what). */
static int env_choice(const char *name, const char *const *names, int n,
                      const char *what)
{
    const char *v = getenv(name);
    int i;

    if (!v || !*v)
        return 0;
    for (i = 0; i < n; i++)
        if (!_stricmp(v, names[i]))
            return i;
    fprintf(stderr, "[GPU] postfx: %s=%s not %s; %s%c", name, v, what,
            names[0], 10);
    return 0;
}

static BOOL CALLBACK params_init(PINIT_ONCE once, PVOID p, PVOID *ctx)
{
    const char *f = getenv("RECOMP_POSTFX_FORCE");
    float g = env_float("RECOMP_GAMMA", GAMMA_LO, GAMMA_HI, 1.0f);
    float c = env_float("RECOMP_CONTRAST", ADJ_LO, ADJ_HI, 1.0f);
    float s = env_float("RECOMP_SATURATION", ADJ_LO, ADJ_HI, 1.0f);
    float sh = env_float("RECOMP_SHARPEN", 0.0f, SHARP_HI, 0.0f);
    int aa = env_choice("RECOMP_AA", s_aa_name, AA_MODES, "off, fxaa or smaa");
    int up = env_choice("RECOMP_UPSCALER", s_up_name, UP_MODES,
                        "bilinear or fsr");

    (void)once; (void)p; (void)ctx;
    InterlockedExchange(&s_gamma, fbits(g));
    InterlockedExchange(&s_contrast, fbits(c));
    InterlockedExchange(&s_saturation, fbits(s));
    InterlockedExchange(&s_sharpen, fbits(sh));
    InterlockedExchange(&s_aa, aa);
    InterlockedExchange(&s_upscale, up);
    s_force = f && !strcmp(f, "1");
    if (g != 1.0f || c != 1.0f || s != 1.0f)
        fprintf(stderr, "[GPU] postfx: picture gamma %.2f contrast %.2f"
                " saturation %.2f%c", g, c, s, 10);
    if (sh != 0.0f)
        fprintf(stderr, "[GPU] postfx: sharpen %.0f %%%c", sh, 10);
    if (aa != AA_OFF)
        fprintf(stderr, "[GPU] postfx: anti-aliasing %s%c", s_aa_name[aa],
                10);
    if (up != UP_BILINEAR)
        fprintf(stderr, "[GPU] postfx: upscaler %s%c", s_up_name[up], 10);
    if (s_force)
        fprintf(stderr, "[GPU] postfx: RECOMP_POSTFX_FORCE: the chain runs"
                " even when every filter is off%c", 10);
    return TRUE;
}

static void params(void)
{
    InitOnceExecuteOnce(&s_once, params_init, NULL, NULL);
}

/* For a settings menu: gamma, contrast and saturation, each 1.0 =
 * unchanged; gamma above 1 brightens the mid-tones. A negative value keeps
 * that one as it is. Applies from the next present. 1 when every value
 * given was taken; one out of range is refused and the others still taken. */
int xbox_SetPicture(float gamma, float contrast, float saturation)
{
    int ok = 1;

    params();
    if (gamma >= 0.0f) {
        if (in_range(gamma, GAMMA_LO, GAMMA_HI))
            InterlockedExchange(&s_gamma, fbits(gamma));
        else
            ok = 0;
    }
    if (contrast >= 0.0f) {
        if (in_range(contrast, ADJ_LO, ADJ_HI))
            InterlockedExchange(&s_contrast, fbits(contrast));
        else
            ok = 0;
    }
    if (saturation >= 0.0f) {
        if (in_range(saturation, ADJ_LO, ADJ_HI))
            InterlockedExchange(&s_saturation, fbits(saturation));
        else
            ok = 0;
    }
    return ok;
}

/* For a settings menu: sharpening in percent, 0 = off, 100 = CAS at its
 * strongest. Applies from the next present. 1 when taken. */
int xbox_SetSharpen(float percent)
{
    params();
    if (!in_range(percent, 0.0f, SHARP_HI))
        return 0;
    InterlockedExchange(&s_sharpen, fbits(percent));
    return 1;
}

/* For a settings menu: anti-aliasing, 0 = off, 1 = FXAA, 2 = SMAA. Applies from the
 * next present. 1 when taken. */
int xbox_SetAntiAlias(int mode)
{
    params();
    if (mode < 0 || mode >= AA_MODES)
        return 0;
    InterlockedExchange(&s_aa, mode);
    return 1;
}

/* For a settings menu: how a frame drawn smaller than the window (a render
 * scale below 100 %) is stretched, 0 = bilinear, 1 = FSR 1. Applies from
 * the next present. 1 when taken. */
int xbox_SetUpscaler(int mode)
{
    params();
    if (mode < 0 || mode >= UP_MODES)
        return 0;
    InterlockedExchange(&s_upscale, mode);
    return 1;
}

static int picture_on(void)
{
    return fval(&s_gamma) != 1.0f || fval(&s_contrast) != 1.0f
        || fval(&s_saturation) != 1.0f;
}

int postfx_active(void)
{
    params();
    return s_force || picture_on() || fval(&s_sharpen) != 0.0f
        || s_aa != AA_OFF;
}

/* ---- the passes ----------------------------------------------------------- */

/* One full-screen triangle over the viewport. pk.xy: source texture units
 * per target pixel, pk.zw: the source rectangle's corner in texture units;
 * po.xy: the viewport's corner in target pixels, po.z nonzero: pixel for
 * pixel, read from pl.xy (the source corner in pixels) on; pl.zw: the
 * source rectangle's far corner in pixels; pa: gamma, contrast,
 * saturation; pc.x: CAS's peak (negative lobe), from the sharpen setting.
 *
 * pps, the picture stage: a value at 1 is skipped, so a neutral pass gives
 * back exactly what it read. Stretching (po.z zero), it samples
 * bilinearly, clamped to pc (the source rectangle's outermost pixel
 * centres, in texture units) so nothing beside the rectangle bleeds in.
 *
 * zps, the scale stage's shrink: each target pixel covers pk.xy source
 * pixels (above 1); it is the average of every source pixel under it,
 * weighted by how much of it is covered (at most 8 a side), in linear
 * light (the sRGB curve), so a supersampled frame keeps its brightness.
 *
 * cps, the sharpen stage, is AMD FidelityFX CAS 1.20190610 (ffx_cas.h,
 * CasFilter), sharpen-only, ported to plain HLSL: exact rcp and sqrt
 * (CAS_GO_SLOWER), the 5-tap min/max (no CAS_BETTER_DIAGONALS), the green
 * weight for all three channels (no CAS_SLOW). Pixel for pixel, taps
 * clamped to the source rectangle. The game's colours are gamma-encoded
 * and CAS wants linear ones: squared on load and square-rooted on output,
 * the gamma 2.0 approximation ffx_cas.h gives for UNORM input.
 *
 *   Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights
 *   reserved. MIT licence; the full text is in LICENSES/FidelityFX-CAS.txt
 *   (see NOTICE). */
static const char s_hlsl[] =
"cbuffer P : register(b0) {\n"
"  float4 pk; float4 po; float4 pl; float4 pa; float4 pc;\n"
"};\n"
"Texture2D pt : register(t0); SamplerState psm : register(s0);\n"
"float4 pvs(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"}\n"
"float4 pps(float4 p : SV_Position) : SV_Target {\n"
"  float2 t = p.xy - po.xy;\n"
"  float4 c = po.z != 0.0 ? pt.Load(int3(t + pl.xy, 0))\n"
"                         : pt.Sample(psm, clamp(t * pk.xy + pk.zw, pc.xy,\n"
"                                               pc.zw));\n"
"  float3 v = c.rgb;\n"
"  [branch] if (pa.x != 1.0) v = pow(saturate(v), 1.0 / pa.x);\n"
"  [branch] if (pa.y != 1.0) v = saturate((v - 0.5) * pa.y + 0.5);\n"
"  [branch] if (pa.z != 1.0)\n"
"    v = saturate(lerp(dot(v, float3(0.2126, 0.7152, 0.0722)).xxx, v,\n"
"                      pa.z));\n"
"  return float4(v, c.a);\n"
"}\n"
"float3 cl(int2 q) {\n"
"  float3 c = pt.Load(int3(clamp(q, int2(pl.xy), int2(pl.zw) - 1), 0)).rgb;\n"
"  return c * c;\n"
"}\n"
"float4 cps(float4 p : SV_Position) : SV_Target {\n"
"  int2 q = int2(p.xy - po.xy + pl.xy);\n"
"  float3 b = cl(q + int2(0, -1)), d = cl(q + int2(-1, 0)), e = cl(q);\n"
"  float3 f = cl(q + int2(1, 0)), h = cl(q + int2(0, 1));\n"
"  float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);\n"
"  float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);\n"
"  float amp = sqrt(saturate(min(mn, 1.0 - mx) * rcp(mx)));\n"
"  float w = amp * pc.x;\n"
"  float3 o = saturate(((b + d + f + h) * w + e) * rcp(1.0 + 4.0 * w));\n"
"  return float4(sqrt(o), pt.Load(int3(q, 0)).a);\n"
"}\n"
"float3 zlin(float3 v) {\n"
"  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);\n"
"}\n"
"float4 zps(float4 p : SV_Position) : SV_Target {\n"
"  float2 a = (p.xy - po.xy - 0.5) * pk.xy, b = a + pk.xy;\n"
"  int2 i0 = int2(floor(a)), i1 = min(int2(ceil(b)) - 1, i0 + 7);\n"
"  float4 s = 0.0;\n"
"  float n = 0.0;\n"
"  [loop] for (int y = i0.y; y <= i1.y; y++) {\n"
"    float wy = min(b.y, y + 1.0) - max(a.y, (float)y);\n"
"    [loop] for (int x = i0.x; x <= i1.x; x++) {\n"
"      float w = (min(b.x, x + 1.0) - max(a.x, (float)x)) * wy;\n"
"      float4 c = pt.Load(int3(clamp(int2(x, y) + int2(pl.xy),\n"
"                                    int2(pl.xy), int2(pl.zw) - 1), 0));\n"
"      s += float4(zlin(saturate(c.rgb)), c.a) * w;\n"
"      n += w;\n"
"    }\n"
"  }\n"
"  float3 v = saturate(s.rgb / n);\n"
"  v = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;\n"
"  return float4(v, s.a / n);\n"
"}\n";

/* fps, the anti-aliasing stage: NVIDIA FXAA 3.11 (third_party/fxaa/
 * Fxaa3_11.h, unchanged; its text is built into the library by
 * cmake/embed_text.cmake), the PC quality shader at preset 39 (the
 * highest), luma from green (the game's colours have none in alpha), and
 * the header's own default strengths: subpixel 0.75, edge threshold 0.166,
 * minimum 0.0833. Compiled after s_hlsl (constants and texture) and the
 * defines below. pk.xy: one over the texture's size. Alpha is the
 * source's, not FXAA's luma.
 *
 *   NVIDIA FXAA 3.11 by Timothy Lottes, (c) 2010, 2011 NVIDIA
 *   Corporation; its notice is in the header (see NOTICE). */
static const char s_fxaa_pre[] =
"#define FXAA_PC 1\n"
"#define FXAA_HLSL_5 1\n"
"#define FXAA_QUALITY__PRESET 39\n"
"#define FXAA_GREEN_AS_LUMA 1\n";
static const char s_fxaa_main[] =
"float4 fps(float4 p : SV_Position) : SV_Target {\n"
"  float2 q = p.xy - po.xy + pl.xy;\n"
"  FxaaTex t;\n"
"  t.smpl = psm;\n"
"  t.tex = pt;\n"
"  float4 c = FxaaPixelShader(q * pk.xy, 0.0, t, t, t, pk.xy, 0.0, 0.0,\n"
"                             0.0, 0.75, 0.166, 0.0833, 0.0, 0.0, 0.0,\n"
"                             0.0);\n"
"  return float4(c.rgb, pt.Load(int3(q, 0)).a);\n"
"}\n";
extern const unsigned char postfx_text_fxaa[];
extern const unsigned postfx_text_fxaa_len;

/* The anti-aliasing stage's other choice: SMAA 1x (third_party/smaa/
 * SMAA.hlsl, unchanged, built in like FXAA), preset HIGH, luma edges, no
 * predication, three draws over the source's size, each with its own
 * vertex shader for SMAA's offsets:
 *
 *   se   edges of the source (t0, UNORM: gamma, as SMAA wants) into
 *        s_edges (R8G8, cleared first: the shader discards non-edges)
 *   sb   blend weights from the edges (t0), areaTex (t1) and searchTex
 *        (t2), SMAA's two lookup images, into s_blend (RGBA8, cleared)
 *   sn   blending: the weights (t0) and the source (t3), its sRGB view so
 *        that the mixing is in linear light (pc.y nonzero; encoded back to
 *        sRGB on output), else its UNORM view (gamma space)
 *
 * pk: SMAA_RT_METRICS (1/w, 1/h, w, h). SMAA's porting macros are ours
 * (SMAA_CUSTOM_SL): sampler s0 bilinear, s1 point, both clamped.
 *
 *   SMAA (c) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando
 *   Navarro, Diego Gutierrez. MIT licence; the full text is in
 *   third_party/smaa/LICENSE.txt (see NOTICE). */
static const char s_smaa_pre[] =
"SamplerState ppt : register(s1);\n"
"Texture2D sarea : register(t1); Texture2D ssearch : register(t2);\n"
"Texture2D scol : register(t3);\n"
"#define SMAA_RT_METRICS pk\n"
"#define SMAA_PRESET_HIGH 1\n"
"#define SMAA_CUSTOM_SL 1\n"
"#define SMAATexture2D(tex) Texture2D tex\n"
"#define SMAATexturePass2D(tex) tex\n"
"#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(psm, coord, 0)\n"
"#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(ppt, coord, 0)\n"
"#define SMAASampleLevelZeroOffset(tex, coord, offset)"
" tex.SampleLevel(psm, coord, 0, offset)\n"
"#define SMAASample(tex, coord) tex.Sample(psm, coord)\n"
"#define SMAASamplePoint(tex, coord) tex.Sample(ppt, coord)\n"
"#define SMAASampleOffset(tex, coord, offset) tex.Sample(psm, coord, offset)\n"
"#define SMAA_FLATTEN [flatten]\n"
"#define SMAA_BRANCH [branch]\n"
"#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex\n"
"#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)\n"
"#define SMAAGather(tex, coord) tex.Gather(psm, coord, 0)\n";
static const char s_smaa_main[] =
"float4 sfull(uint id, out float2 tc) {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  tc = p;\n"
"  return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"}\n"
"void sevs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
"          out float2 tc : TEXCOORD0, out float4 off[3] : TEXCOORD1) {\n"
"  pos = sfull(id, tc);\n"
"  SMAAEdgeDetectionVS(tc, off);\n"
"}\n"
"float4 seps(float4 pos : SV_Position, float2 tc : TEXCOORD0,\n"
"            float4 off[3] : TEXCOORD1) : SV_Target {\n"
"  return float4(SMAALumaEdgeDetectionPS(tc, off, pt), 0.0, 0.0);\n"
"}\n"
"void sbvs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
"          out float2 tc : TEXCOORD0, out float2 pix : TEXCOORD1,\n"
"          out float4 off[3] : TEXCOORD2) {\n"
"  pos = sfull(id, tc);\n"
"  SMAABlendingWeightCalculationVS(tc, pix, off);\n"
"}\n"
"float4 sbps(float4 pos : SV_Position, float2 tc : TEXCOORD0,\n"
"            float2 pix : TEXCOORD1, float4 off[3] : TEXCOORD2)\n"
"            : SV_Target {\n"
"  return SMAABlendingWeightCalculationPS(tc, pix, off, pt, sarea, ssearch,\n"
"                                         0.0);\n"
"}\n"
"void snvs(uint id : SV_VertexID, out float4 pos : SV_Position,\n"
"          out float2 tc : TEXCOORD0, out float4 off : TEXCOORD1) {\n"
"  pos = sfull(id, tc);\n"
"  SMAANeighborhoodBlendingVS(tc, off);\n"
"}\n"
"float4 snps(float4 pos : SV_Position, float2 tc : TEXCOORD0,\n"
"            float4 off : TEXCOORD1) : SV_Target {\n"
"  float4 c = SMAANeighborhoodBlendingPS(tc, off, scol, pt);\n"
"  float3 v = saturate(c.rgb);\n"
"  [branch] if (pc.y != 0.0)\n"
"    v = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;\n"
"  return float4(v, SMAASampleLevelZeroPoint(scol, tc).a);\n"
"}\n";
extern const unsigned char postfx_text_smaa[];
extern const unsigned postfx_text_smaa_len;

/* The upscaler's other choice: AMD FidelityFX FSR 1.0 (third_party/fsr1/
 * ffx_a.h and ffx_fsr1.h, unchanged, built in like FXAA), its 32-bit float
 * shaders, two draws, each target pixel for pixel:
 *
 *   eps   EASU, the scale stage: the source (t0, its whole texture, gamma-
 *         encoded as EASU wants) stretched to the target size; pk, pl,
 *         pa, pc: FsrEasuCon's con0..con3, as float bits
 *   rps   RCAS, the sharpen stage behind EASU: pc.x FsrRcasCon's con[0]
 *         (as float bits); taps clamped to the rectangle pl; alpha passed
 *         through
 *
 *   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
 *   MIT licence; the full text is in third_party/fsr1/license.txt and
 *   LICENSES/FidelityFX-FSR.txt (see NOTICE). */
static const char s_fsr_pre[] =
"#define A_GPU 1\n"
"#define A_HLSL 1\n"
"#define FSR_EASU_F 1\n"
"#define FSR_RCAS_F 1\n"
"#define FSR_RCAS_PASSTHROUGH_ALPHA 1\n";
static const char s_fsr_mid[] =
"AF4 FsrEasuRF(AF2 p) { return pt.GatherRed(psm, p); }\n"
"AF4 FsrEasuGF(AF2 p) { return pt.GatherGreen(psm, p); }\n"
"AF4 FsrEasuBF(AF2 p) { return pt.GatherBlue(psm, p); }\n"
"AF4 FsrRcasLoadF(ASU2 p) {\n"
"  return pt.Load(int3(clamp(p, int2(pl.xy), int2(pl.zw) - 1), 0));\n"
"}\n"
"void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n";
static const char s_fsr_main[] =
"float4 eps(float4 p : SV_Position) : SV_Target {\n"
"  AF3 c;\n"
"  FsrEasuF(c, AU2(p.xy - po.xy), asuint(pk), asuint(pl), asuint(pa),\n"
"           asuint(pc));\n"
"  return float4(c, 1.0);\n"
"}\n"
"float4 rps(float4 p : SV_Position) : SV_Target {\n"
"  float4 c;\n"
"  FsrRcasF(c.r, c.g, c.b, c.a, AU2(p.xy - po.xy + pl.xy), asuint(pc));\n"
"  return c;\n"
"}\n";
extern const unsigned char postfx_text_ffx_a[];
extern const unsigned postfx_text_ffx_a_len;
extern const unsigned char postfx_text_ffx_fsr1[];
extern const unsigned postfx_text_ffx_fsr1_len;

static ID3D11Device          *s_dev;      /* the objects below are its */
static ID3D11VertexShader    *s_vs;
static ID3D11PixelShader     *s_ps;       /* picture */
static ID3D11PixelShader     *s_cas;      /* sharpen */
static ID3D11PixelShader     *s_shrink;   /* scale, bigger to smaller */
static ID3D11PixelShader     *s_easu;     /* scale (FSR), made on first use */
static ID3D11PixelShader     *s_rcas;     /* sharpen behind EASU, with it */
static int                    s_fsr_broken;  /* not made, said once */
static ID3D11PixelShader     *s_fxaa;     /* aa (FXAA), made on first use */
static ID3D11VertexShader    *s_svs[3];   /* aa (SMAA): edges, weights, */
static ID3D11PixelShader     *s_sps[3];   /* blending; made on first use */
static ID3D11Buffer          *s_cb;
static ID3D11SamplerState    *s_samp;     /* bilinear, s0 */
static ID3D11SamplerState    *s_point;    /* point, s1 (SMAA) */
static ID3D11RasterizerState *s_rs;
static int                    s_broken;   /* could not be made: copy */
static int                    s_aa_broken[AA_MODES];  /* none, said once */

/* SMAA's lookup images (third_party/smaa/AreaTex.h, SearchTex.h). */
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
static ID3D11ShaderResourceView *s_area, *s_search;

/* A texture the chain draws into or copies to: made at a size, again when
 * that changes. */
typedef struct {
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;      /* NULL for s_in */
    ID3D11ShaderResourceView *srv;
    ID3D11ShaderResourceView *srv2;     /* s_in for SMAA: the sRGB view */
    UINT                      w, h;
    DXGI_FORMAT               fmt;
} Surf;

static Surf s_mid[3];   /* stage i's output, when not the last */
static Surf s_in;       /* the source rectangle alone, for anti-aliasing */
static Surf s_edges;    /* SMAA's edges and blend weights */
static Surf s_blend;

static ID3DBlob *compile(const char *src, size_t len, const char *entry,
                         const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, "postfx_d3d11", NULL, NULL, entry,
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                            &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: shader %s failed: %s%c", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?",
                10);
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static HRESULT make_ps_from(ID3D11Device *dev, const char *src, size_t len,
                            const char *entry, const char *target,
                            ID3D11PixelShader **ps)
{
    ID3DBlob *code = compile(src, len, entry, target);
    HRESULT hr;

    if (!code)
        return E_FAIL;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code), NULL, ps);
    ID3D10Blob_Release(code);
    return hr;
}

static HRESULT make_ps(ID3D11Device *dev, const char *entry,
                       ID3D11PixelShader **ps)
{
    return make_ps_from(dev, s_hlsl, sizeof s_hlsl - 1, entry, "ps_4_0", ps);
}

/* s_hlsl, then the n parts (part[i], size[i] bytes) in order: one shader
 * source around vendored text, *len bytes, for free(). NULL when out of
 * memory. */
static char *join_source(const void *const *part, const size_t *size, int n,
                         size_t *len)
{
    size_t total = sizeof s_hlsl - 1, at;
    char *src;
    int i;

    for (i = 0; i < n; i++)
        total += size[i];
    src = (char *)malloc(total);
    if (src) {
        memcpy(src, s_hlsl, sizeof s_hlsl - 1);
        at = sizeof s_hlsl - 1;
        for (i = 0; i < n; i++) {
            memcpy(src + at, part[i], size[i]);
            at += size[i];
        }
    }
    *len = total;
    return src;
}

/* s_hlsl, then pre, the vendored text and main: one anti-aliasing
 * shader source (join_source). */
static char *aa_source(const char *pre, size_t n2, const unsigned char *text,
                       size_t n3, const char *main, size_t n4, size_t *len)
{
    const void *part[3];
    size_t size[3];

    part[0] = pre;
    size[0] = n2;
    part[1] = text;
    size[1] = n3;
    part[2] = main;
    size[2] = n4;
    return join_source(part, size, 3, len);
}

/* FSR's EASU and RCAS shaders, the first time the FSR upscaler is used
 * (they take a moment to compile). 0 (said once) when they cannot be made:
 * the stretch is then bilinear. */
static int fsr_setup(ID3D11Device *dev)
{
    const void *part[5];
    size_t size[5], len;
    char *src;
    DWORD t0;
    HRESULT hr;

    if (s_rcas)
        return 1;
    if (s_fsr_broken)
        return 0;
    t0 = GetTickCount();
    part[0] = s_fsr_pre;
    size[0] = sizeof s_fsr_pre - 1;
    part[1] = postfx_text_ffx_a;
    size[1] = postfx_text_ffx_a_len;
    part[2] = s_fsr_mid;
    size[2] = sizeof s_fsr_mid - 1;
    part[3] = postfx_text_ffx_fsr1;
    size[3] = postfx_text_ffx_fsr1_len;
    part[4] = s_fsr_main;
    size[4] = sizeof s_fsr_main - 1;
    src = join_source(part, size, 5, &len);
    hr = src ? make_ps_from(dev, src, len, "eps", "ps_5_0", &s_easu)
             : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
        hr = make_ps_from(dev, src, len, "rps", "ps_5_0", &s_rcas);
    free(src);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: FSR 1 could not be made (hr 0x%08lX);"
                " the upscaler is bilinear%c", (unsigned long)hr, 10);
        if (s_easu) {
            ID3D11PixelShader_Release(s_easu);
            s_easu = NULL;
        }
        s_fsr_broken = 1;
        return 0;
    }
    fprintf(stderr, "[GPU] postfx: fsr ready (%lu ms)%c",
            (unsigned long)(GetTickCount() - t0), 10);
    return 1;
}

/* The FXAA shader, the first time it is chosen (it takes a moment to
 * compile). */
static HRESULT fxaa_make(ID3D11Device *dev)
{
    size_t len;
    char *src = aa_source(s_fxaa_pre, sizeof s_fxaa_pre - 1,
                          postfx_text_fxaa, postfx_text_fxaa_len,
                          s_fxaa_main, sizeof s_fxaa_main - 1, &len);
    HRESULT hr;

    if (!src)
        return E_OUTOFMEMORY;
    hr = make_ps_from(dev, src, len, "fps", "ps_5_0", &s_fxaa);
    free(src);
    return hr;
}

/* An immutable texture from one of SMAA's lookup images. */
static HRESULT lut_make(ID3D11Device *dev, const unsigned char *bytes,
                        UINT w, UINT h, UINT pitch, DXGI_FORMAT fmt,
                        ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr;

    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = bytes;
    sd.SysMemPitch = pitch;
    sd.SysMemSlicePitch = 0;
    hr = ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex,
                                                   NULL, srv);
    if (tex)
        ID3D11Texture2D_Release(tex);
    return hr;
}

/* SMAA's three shader pairs, its point sampler and lookup images, the
 * first time it is chosen. */
static HRESULT smaa_make(ID3D11Device *dev)
{
    static const char *const vs[3] = { "sevs", "sbvs", "snvs" };
    static const char *const ps[3] = { "seps", "sbps", "snps" };
    D3D11_SAMPLER_DESC sd;
    size_t len;
    char *src = aa_source(s_smaa_pre, sizeof s_smaa_pre - 1,
                          postfx_text_smaa, postfx_text_smaa_len,
                          s_smaa_main, sizeof s_smaa_main - 1, &len);
    HRESULT hr = src ? S_OK : E_OUTOFMEMORY;
    int i;

    for (i = 0; i < 3 && SUCCEEDED(hr); i++) {
        ID3DBlob *code = compile(src, len, vs[i], "vs_5_0");
        hr = code ? S_OK : E_FAIL;
        if (code) {
            hr = ID3D11Device_CreateVertexShader(dev,
                    ID3D10Blob_GetBufferPointer(code),
                    ID3D10Blob_GetBufferSize(code), NULL, &s_svs[i]);
            ID3D10Blob_Release(code);
        }
        if (SUCCEEDED(hr))
            hr = make_ps_from(dev, src, len, ps[i], "ps_5_0", &s_sps[i]);
    }
    free(src);
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(dev, &sd, &s_point);
    }
    if (SUCCEEDED(hr))
        hr = lut_make(dev, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT,
                      AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM, &s_area);
    if (SUCCEEDED(hr))
        hr = lut_make(dev, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                      SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM, &s_search);
    return hr;
}

/* The chosen anti-aliasing's objects, the first time it is on (they take
 * a moment to compile). 0 (said once) when they cannot be made: that
 * anti-aliasing is then skipped. */
static int aa_setup(ID3D11Device *dev, int aa)
{
    DWORD t0;
    HRESULT hr;

    /* s_search is the last of SMAA's objects made. */
    if (aa == AA_FXAA ? s_fxaa != NULL : s_search != NULL)
        return 1;
    if (s_aa_broken[aa])
        return 0;
    t0 = GetTickCount();
    hr = aa == AA_FXAA ? fxaa_make(dev) : smaa_make(dev);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: %s could not be made (hr 0x%08lX);"
                " anti-aliasing skipped%c", s_aa_name[aa], (unsigned long)hr,
                10);
        s_aa_broken[aa] = 1;
        return 0;
    }
    fprintf(stderr, "[GPU] postfx: %s ready (%lu ms)%c", s_aa_name[aa],
            (unsigned long)(GetTickCount() - t0), 10);
    return 1;
}

/* The pipeline objects, made on first use. 0 (said once) when they cannot
 * be: the frame is then copied unfiltered. */
static int setup(ID3D11Device *dev)
{
    ID3DBlob *vs;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (s_dev == dev && s_rs)
        return 1;
    if (s_broken)
        return 0;
    s_dev = dev;
    vs = compile(s_hlsl, sizeof s_hlsl - 1, "pvs", "vs_4_0");
    hr = vs ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateVertexShader(dev,
                ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
                NULL, &s_vs);
    if (vs) ID3D10Blob_Release(vs);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "pps", &s_ps);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "cps", &s_cas);
    if (SUCCEEDED(hr))
        hr = make_ps(dev, "zps", &s_shrink);
    if (SUCCEEDED(hr)) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb);
    }
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof sd);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(dev, &sd, &s_samp);
    }
    if (SUCCEEDED(hr)) {
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(dev, &rd, &s_rs);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[GPU] postfx: setup failed (hr 0x%08lX); frames are"
                " shown unfiltered%c", (unsigned long)hr, 10);
        s_broken = 1;
        return 0;
    }
    fprintf(stderr, "[GPU] postfx: filter chain ready%c", 10);
    return 1;
}

static void surf_free(Surf *s)
{
    if (s->srv2) ID3D11ShaderResourceView_Release(s->srv2);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
}

/* A view of s's texture in format view. */
static HRESULT surf_view(ID3D11Device *dev, Surf *s, DXGI_FORMAT view,
                         ID3D11ShaderResourceView **srv)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;

    memset(&vd, 0, sizeof vd);
    vd.Format = view;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    return ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)s->tex,
                                                 &vd, srv);
}

/* s at w x h in format fmt, a render target too when rt. A typeless fmt
 * (only B8G8R8A8_TYPELESS here, for SMAA's s_in) gets its UNORM view as
 * srv and its sRGB one as srv2. 0 (said once) when it cannot be made. */
static int surf_fit(ID3D11Device *dev, Surf *s, UINT w, UINT h,
                    DXGI_FORMAT fmt, int rt)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    int typeless = fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS;

    if (s->tex && s->w == w && s->h == h && s->fmt == fmt)
        return 1;
    surf_free(s);
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE
                 | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->tex);
    if (SUCCEEDED(hr) && rt)
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s->tex,
                                                 NULL, &s->rtv);
    if (SUCCEEDED(hr) && typeless) {
        hr = surf_view(dev, s, DXGI_FORMAT_B8G8R8A8_UNORM, &s->srv);
        if (SUCCEEDED(hr))
            hr = surf_view(dev, s, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, &s->srv2);
    } else if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(dev,
                (ID3D11Resource *)s->tex, NULL, &s->srv);
    }
    if (FAILED(hr)) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GPU] postfx: no %ux%u texture (format %d, hr"
                    " 0x%08lX) for the chain; stages skipped%c", w, h,
                    (int)fmt, (unsigned long)hr, 10);
        surf_free(s);
        return 0;
    }
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    return 1;
}

/* One draw: shaders vs (NULL = the plain full-screen triangle) and ps
 * over the rectangle dx0,dy0 dw x dh of dst, reading srv[0..nsrv-1] from
 * t0 on, with constants k (see s_hlsl). Sets every piece of pipeline
 * state it uses and leaves no target or view bound. */
static int run_draw(ID3D11DeviceContext *ctx, ID3D11VertexShader *vs,
                    ID3D11PixelShader *ps,
                    ID3D11ShaderResourceView *const *srv, UINT nsrv,
                    ID3D11RenderTargetView *dst, float dx0, float dy0,
                    float dw, float dh, const float *k)
{
    static ID3D11ShaderResourceView *const none[4] = { NULL, NULL, NULL,
                                                       NULL };
    ID3D11SamplerState *samp[2];
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;

    samp[0] = s_samp;
    samp[1] = s_point;

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s_cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, k, 20 * sizeof *k);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s_cb, 0);

    vp.TopLeftX = dx0;
    vp.TopLeftY = dy0;
    vp.Width = dw;
    vp.Height = dh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    sc.left = (LONG)dx0;
    sc.top = (LONG)dy0;
    sc.right = (LONG)(dx0 + dw + 0.5f);
    sc.bottom = (LONG)(dy0 + dh + 0.5f);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs ? vs : s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, nsrv, srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, s_point ? 2 : 1, samp);
    ID3D11DeviceContext_RSSetState(ctx, s_rs);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, nsrv, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    return 1;
}

/* One draw of the plain triangle, reading srv alone. */
static int run_pass(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps,
                    ID3D11ShaderResourceView *srv, ID3D11RenderTargetView *dst,
                    float dx0, float dy0, float dw, float dh, const float *k)
{
    return run_draw(ctx, NULL, ps, &srv, 1, dst, dx0, dy0, dw, dh, k);
}

/* SMAA's three draws: in (s_in, its UNORM view; srgb its sRGB one or
 * NULL) at w x h, into dst's rectangle dx0,dy0 of that size. */
static int smaa_run(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                    ID3D11ShaderResourceView *in,
                    ID3D11ShaderResourceView *srgb, UINT w, UINT h,
                    ID3D11RenderTargetView *dst, float dx0, float dy0)
{
    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11ShaderResourceView *srv[4];
    float k[20];

    if (!surf_fit(dev, &s_edges, w, h, DXGI_FORMAT_R8G8_UNORM, 1)
        || !surf_fit(dev, &s_blend, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 1))
        return 0;
    memset(k, 0, sizeof k);
    k[0] = 1.0f / (float)w;
    k[1] = 1.0f / (float)h;
    k[2] = (float)w;
    k[3] = (float)h;
    k[17] = srgb ? 1.0f : 0.0f;
    ID3D11DeviceContext_ClearRenderTargetView(ctx, s_edges.rtv, zero);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, s_blend.rtv, zero);
    srv[0] = in;
    if (!run_draw(ctx, s_svs[0], s_sps[0], srv, 1, s_edges.rtv, 0.0f, 0.0f,
                  (float)w, (float)h, k))
        return 0;
    srv[0] = s_edges.srv;
    srv[1] = s_area;
    srv[2] = s_search;
    if (!run_draw(ctx, s_svs[1], s_sps[1], srv, 3, s_blend.rtv, 0.0f, 0.0f,
                  (float)w, (float)h, k))
        return 0;
    srv[0] = s_blend.srv;
    srv[1] = NULL;
    srv[2] = NULL;
    srv[3] = srgb ? srgb : in;
    return run_draw(ctx, s_svs[2], s_sps[2], srv, 4, dst, dx0, dy0,
                    (float)w, (float)h, k);
}

enum { ST_AA, ST_SCALE, ST_SHARPEN, ST_PICTURE };
enum { SC_NONE, SC_SHRINK, SC_BILINEAR, SC_FSR };

/* pps's stretch (po.z zero) of the rectangle ix0,iy0 cw x ch of a tw x th
 * texture over vw x vh target pixels, into k (see s_hlsl). */
static void stretch_k(float *k, float ix0, float iy0, float cw, float ch,
                      float tw, float th, float vw, float vh)
{
    k[0] = cw / (vw * tw);
    k[1] = ch / (vh * th);
    k[2] = ix0 / tw;
    k[3] = iy0 / th;
    k[6] = 0.0f;
    k[16] = (ix0 + 0.5f) / tw;
    k[17] = (iy0 + 0.5f) / th;
    k[18] = (ix0 + cw - 0.5f) / tw;
    k[19] = (iy0 + ch - 0.5f) / th;
}

int postfx_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx,
                ID3D11Texture2D *src, float sx0, float sy0, float sw,
                float sh, ID3D11RenderTargetView *dst, float dx0, float dy0,
                float dw, float dh)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11ShaderResourceView *srv = NULL, *in;
    float k[20], sharpen;
    float tw, th, ix0, iy0, cw, ch;
    UINT mw = (UINT)(sw + 0.5f), mh = (UINT)(sh + 0.5f);
    UINT ow = (UINT)(dw + 0.5f), oh = (UINT)(dh + 0.5f);
    int aa, scale = SC_NONE, whole, pic, stage[4], n = 0, i, ok = 1;

    /* The values, read here too: a frame of another size comes here
     * without postfx_active having been asked, and unread values are 0
     * (contrast 0 is a flat grey). */
    params();
    sharpen = fval(&s_sharpen);
    aa = s_aa;
    if (!src || !dst || dw < 1.0f || dh < 1.0f || sw <= 0.0f || sh <= 0.0f
        || !setup(dev))
        return 0;
    ID3D11Texture2D_GetDesc(src, &td);
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev,
            (ID3D11Resource *)src, NULL, &srv)))
        return 0;
    in = srv;
    tw = (float)td.Width;
    th = (float)td.Height;
    ix0 = sx0;
    iy0 = sy0;
    whole = sx0 == 0.0f && sy0 == 0.0f && mw == td.Width && mh == td.Height;

    if (aa != AA_OFF && !aa_setup(dev, aa))
        aa = AA_OFF;
    if (mw != ow || mh != oh) {
        if (mw >= ow && mh >= oh)
            scale = SC_SHRINK;
        else if (s_upscale == UP_FSR && fsr_setup(dev))
            scale = SC_FSR;
        else
            scale = SC_BILINEAR;
    }
    if ((aa != AA_OFF && (aa == AA_SMAA || !whole))
        || (aa == AA_OFF && scale == SC_FSR && !whole)) {
        /* The rectangle alone, so that sampling around its edge clamps
         * there instead of reading what lies next to it; for SMAA with
         * an sRGB view too (srv2) when the source is 8-bit BGRA. */
        DXGI_FORMAT f = td.Format;
        if (aa == AA_SMAA && f == DXGI_FORMAT_B8G8R8A8_UNORM)
            f = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        if (surf_fit(dev, &s_in, mw, mh, f, 0)) {
            D3D11_BOX box;
            box.left = (UINT)sx0;
            box.top = (UINT)sy0;
            box.front = 0;
            box.right = box.left + mw;
            box.bottom = box.top + mh;
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(ctx,
                    (ID3D11Resource *)s_in.tex, 0, 0, 0, 0,
                    (ID3D11Resource *)src, 0, &box);
            in = s_in.srv;
            tw = (float)mw;
            th = (float)mh;
            ix0 = iy0 = 0.0f;
        } else {
            aa = AA_OFF;
            if (scale == SC_FSR)
                scale = SC_BILINEAR;
        }
    }

    /* The stages that are on; every one but the last draws into s_mid. A
     * bilinear stretch with nothing between it and the picture stage is
     * the picture stage's own. */
    pic = picture_on();
    if (aa != AA_OFF)
        stage[n++] = ST_AA;
    if (scale == SC_BILINEAR && sharpen == 0.0f)
        pic = 1;
    else if (scale != SC_NONE)
        stage[n++] = ST_SCALE;
    if (sharpen != 0.0f)
        stage[n++] = ST_SHARPEN;
    if (pic || n == 0)
        stage[n++] = ST_PICTURE;
    for (i = 0; i < n - 1; i++) {
        /* Before the scale stage the source's size, from it on the
         * target's. */
        int before = stage[i] == ST_AA;
        if (!surf_fit(dev, &s_mid[i], before ? mw : ow, before ? mh : oh,
                      DXGI_FORMAT_R16G16B16A16_FLOAT, 1))
            break;
    }
    if (i < n - 1) {
        /* Only the picture stage, from the source itself. */
        n = 0;
        stage[n++] = ST_PICTURE;
        in = srv;
        tw = (float)td.Width;
        th = (float)td.Height;
        ix0 = sx0;
        iy0 = sy0;
    }

    cw = (float)mw;                     /* the input rectangle's size */
    ch = (float)mh;
    for (i = 0; ok && i < n; i++) {
        int last = i == n - 1;
        Surf *o = last ? NULL : &s_mid[i];
        ID3D11RenderTargetView *rtv = last ? dst : o->rtv;
        float ox = last ? dx0 : 0.0f, oy = last ? dy0 : 0.0f;
        float vw = last ? dw : (float)o->w, vh = last ? dh : (float)o->h;

        memset(k, 0, sizeof k);
        k[4] = ox;
        k[5] = oy;
        k[8] = ix0;
        k[9] = iy0;
        k[10] = ix0 + cw;
        k[11] = iy0 + ch;
        switch (stage[i]) {
        case ST_AA:
            if (aa == AA_SMAA) {
                /* Always from s_in (copied above). */
                ok = smaa_run(dev, ctx, in, s_in.srv2, mw, mh, rtv, ox, oy);
                break;
            }
            k[0] = 1.0f / tw;
            k[1] = 1.0f / th;
            ok = run_pass(ctx, s_fxaa, in, rtv, ox, oy, vw, vh, k);
            break;
        case ST_SCALE:
            if (scale == SC_SHRINK) {
                k[0] = cw / vw;
                k[1] = ch / vh;
                ok = run_pass(ctx, s_shrink, in, rtv, ox, oy, vw, vh, k);
            } else if (scale == SC_FSR) {
                /* FsrEasuCon(con0..con3, cw, ch, tw, th, vw, vh); the
                 * input is its whole texture here (s_in or s_mid). */
                k[0] = cw / vw;
                k[1] = ch / vh;
                k[2] = 0.5f * cw / vw - 0.5f;
                k[3] = 0.5f * ch / vh - 0.5f;
                k[8] = 1.0f / tw;
                k[9] = 1.0f / th;
                k[10] = 1.0f / tw;
                k[11] = -1.0f / th;
                k[12] = -1.0f / tw;
                k[13] = 2.0f / th;
                k[14] = 1.0f / tw;
                k[15] = 2.0f / th;
                k[16] = 0.0f;
                k[17] = 4.0f / th;
                ok = run_pass(ctx, s_easu, in, rtv, ox, oy, vw, vh, k);
            } else {
                stretch_k(k, ix0, iy0, cw, ch, tw, th, vw, vh);
                k[12] = k[13] = k[14] = 1.0f;
                ok = run_pass(ctx, s_ps, in, rtv, ox, oy, vw, vh, k);
            }
            break;
        case ST_SHARPEN:
            if (scale == SC_FSR) {
                /* FsrRcasCon's con[0], exp2(-stops): 100 % = 0 stops
                 * (RCAS at its strongest), each 50 % less one stop less
                 * sharp; AMD's default, 0.2 stops, is 90 %. */
                k[16] = exp2f(-2.0f * (1.0f - sharpen / 100.0f));
                ok = run_pass(ctx, s_rcas, in, rtv, ox, oy, vw, vh, k);
                break;
            }
            /* 100 % = CasSetup's sharpness 1 (-1/5); AMD's default,
             * sharpness 0 (-1/8), is 62.5 %. */
            k[16] = -0.2f * sharpen / 100.0f;
            ok = run_pass(ctx, s_cas, in, rtv, ox, oy, vw, vh, k);
            break;
        default:
            if (cw == vw && ch == vh)
                k[6] = 1.0f;            /* pixel for pixel */
            else
                stretch_k(k, ix0, iy0, cw, ch, tw, th, vw, vh);
            k[12] = fval(&s_gamma);
            k[13] = fval(&s_contrast);
            k[14] = fval(&s_saturation);
            ok = run_pass(ctx, s_ps, in, rtv, ox, oy, vw, vh, k);
            break;
        }
        if (!last) {
            in = o->srv;
            tw = cw = (float)o->w;
            th = ch = (float)o->h;
            ix0 = iy0 = 0.0f;
        }
    }
    ID3D11ShaderResourceView_Release(srv);
    return ok;
}

#else

int xbox_SetPicture(float gamma, float contrast, float saturation)
{
    (void)gamma; (void)contrast; (void)saturation;
    return 0;
}

int xbox_SetSharpen(float percent)
{
    (void)percent;
    return 0;
}

int xbox_SetAntiAlias(int mode)
{
    (void)mode;
    return 0;
}

int xbox_SetUpscaler(int mode)
{
    (void)mode;
    return 0;
}

#endif
