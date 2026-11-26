/**
 * Lit vertex programs: which of a title's programs light a vertex the way
 * the title says they do, and where their lighting terms can be taken from,
 * so the light can be worked out per pixel instead of per vertex.
 *
 * A program lights per vertex: an ambient term from the normal, a key light
 * (the normal dotted with a light direction), darkened by "blobs" (soft
 * round shadows other characters cast), written to the colour outputs and
 * interpolated across each triangle. Where the light changes quickly along
 * a surface (a tail or an arm against the sun) the interpolation shows the
 * triangles: light edges crawl as the model moves. Worked out per pixel
 * from the interpolated normal and position, the same maths gives the same
 * colours at the corners and smooth ones between.
 *
 * The toolkit knows no title's lighting, so the title names its constant
 * rows once (xbox_SetLitPattern); unset, nothing here applies. The scan
 * then follows each program's data flow symbolically: every temp component
 * holds a value with an id (which instruction lane wrote it) and, where it
 * recognises one, what it is (the normal, an ambient channel, the key term,
 * a blob distance...). A program is lit only if its colour outputs come out
 * as exactly the pattern's maths; anything else stays per vertex, with the
 * reason. The registers each program uses, the order of its instructions
 * and how it interleaves other work do not matter.
 *
 * The pattern (rows are hardware constant numbers, c[0..191]):
 *
 *   n        the normal, normalised in the program (v * rsq(dot(v, v)))
 *   ambient  per channel ch (rows a = amb_first + 4 ch .. a + 3):
 *              dot(n, (dph(n, c[a]), dph(n, c[a+1]), dph(n, c[a+2])))
 *              + dph(n, c[a+3])                       (two DPH stages)
 *   key      max(dot(n, -c[key_dir].xyz), 0)                  (LIT's y)
 *   blobs    per group g (rows b = blob_first + 4 g: x, y, clip height,
 *            strength; one blob per component), with x, y, h any values the
 *            program works out (their capture points are reported):
 *              sum over lanes of rcc((c[b] - x)^2 + (c[b+1] - y)^2)
 *                                * (h >= c[b+2]) * c[b+3]
 *   term     key * (1 - the groups' sums), in some programs then floored:
 *            max(term, c[lo_row].<lo_comp>)
 *
 * and the colour outputs in one of four layouts:
 *
 *   A  oD0.rgb = ambient                 oD1.rgb = term * c[key_col].rgb
 *   B  oD0.rgb = ambient * f (an input)  oD1.rgb = term * c[key_col].rgb
 *   C  oD0.rgb = term * c[key_col].rgb + ambient
 *   D  oD0.rgb = term * c[key_col].rgb + ambient * f
 *
 * Alphas are left as the program writes them. Portable C; no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_LIT_H
#define XBOXRECOMP_NV2A_VSH_LIT_H

#include <stddef.h>
#include <stdint.h>

#include "nv2a_vsh_interp.h"

enum { NV2A_LIT_NONE = 0, NV2A_LIT_A = 1, NV2A_LIT_B = 2, NV2A_LIT_C = 3,
       NV2A_LIT_D = 4 };

/* How a lit program writes oD1.w (left interpolated; the log says which). */
enum {
    NV2A_LIT_W_OTHER = 0,       /* anything else (a constant, nothing...) */
    NV2A_LIT_W_BLOBS_MUL,       /* c[row].comp * (1 - the blob sums) */
    NV2A_LIT_W_BLOBS_SUB,       /* c[row].comp - the blob sums */
    NV2A_LIT_W_NORMAL_SUB       /* c[row].comp - n.<comp2> */
};

/* The title's constant rows. key_dir < 0: no pattern. */
typedef struct {
    int key_dir, key_col;       /* key light direction, colour */
    int amb_first;              /* 12 rows: 3 channels x 4 */
    int blob_first, blob_groups;/* groups of 4 rows; 0..3 groups */
} Nv2aLitRegs;

/* Where a lit program's terms are, for capturing them in its HLSL
 * (nv2a_vsh_hlsl.c) and working the light out per pixel. */
typedef struct {
    uint8_t  layout;            /* NV2A_LIT_*; NONE = per vertex (why) */
    uint8_t  groups;            /* bit g: blob group g darkens the key */
    uint8_t  skinned;           /* reads a0-relative rows (bones) */
    uint8_t  n_src, n_temp;     /* the normal: source n_src (0 A, 1 B) */
    uint16_t n_ins;             /*   of instruction n_ins, temp n_temp */
    uint16_t p_ins[3];          /* blob x, y, height: the instruction */
    uint8_t  p_unit[3];         /*   whose MAC (0) or ILU (1) result */
    uint8_t  p_comp[3];         /*   lane p_comp is the value */
    uint16_t f_ins;             /* layouts B, D: the factor is source */
    uint8_t  f_src, f_reg;      /*   f_src of f_ins, input register f_reg */
    uint8_t  lo_row, lo_comp;   /* the term's floor; lo_row 0xFF: none */
    uint8_t  d1w, d1w_row, d1w_comp, d1w_comp2;  /* oD1.w: NV2A_LIT_W_* */
    const char *why;            /* layout NONE: why not */
} Nv2aVshLit;

/* Scans count decoded instructions against regs. Returns 0 when the program
 * reads none of the pattern's key or ambient rows (not lit at all: nothing
 * to say), else 1 with *out filled: a layout, or NONE and why. */
int nv2a_vsh_lit_scan(const Nv2aVshIns *ins, uint32_t count,
                      const Nv2aLitRegs *regs, Nv2aVshLit *out);

/* "layout A, normal r2@71, blobs x/y/h @34/35/36 (3 groups), skinned"
 * or "per vertex (<why>)", for [VSHLIT] lines. */
void nv2a_vsh_lit_describe(const Nv2aVshLit *l, char *buf, size_t cap);

/* What a lit program's pixel shader depends on, as one word (0: not lit):
 * the layout (bits 0-2), the blob groups (3-5), a floor (bit 6; its row
 * bits 8-15, component 16-17), how oD1.w is written (18-19, NV2A_LIT_W_*;
 * its row 20-27, component 28-29, the normal's component 30-31). Programs
 * with the same word share their pixel shaders (one per combiner setup). */
uint32_t nv2a_vsh_lit_word(const Nv2aVshLit *l);

/* Debug views (xbox_SetLitView), drawn by the lit pixel shader in place of
 * the lit batches' colours: */
enum {
    NV2A_LIT_VIEW_OFF = 0,
    NV2A_LIT_VIEW_VERTEX,   /* the maths per vertex in the lit vertex shader
                               (must equal the program's own colours) */
    NV2A_LIT_VIEW_KEY,      /* the key light alone, grey */
    NV2A_LIT_VIEW_AMBIENT,  /* the ambient alone */
    NV2A_LIT_VIEW_BLOBS,    /* 1 - the blob shadows, grey */
    NV2A_LIT_VIEW_TERM,     /* key x (1 - blobs) x the key colour */
    NV2A_LIT_VIEW_FACE,     /* the side the title's culling keeps green,
                               the other red */
    NV2A_LIT_VIEWS
};
void xbox_SetLitView(int view);
int nv2a_vsh_lit_view(void);

/* The title's pattern (xbox_SetLitPattern), or NULL while none is set. */
const Nv2aLitRegs *nv2a_vsh_lit_regs(void);

/* Per-pixel lighting on (xbox_SetPerPixelLighting): 0 off, 1 on, 2 on in
 * even frames only (the held-picture check: neighbouring frames compared,
 * cmp_alt.py). Always 0 while no pattern is set. */
int nv2a_vsh_lit_mode(void);

/* For the title (a setting, at once): 0 off, 1 on, 2 even frames only. */
void xbox_SetPerPixelLighting(int mode);

/* For the title, once at start-up, before anything draws: its lit
 * programs' constant rows (above). key_dir < 0 forgets the pattern. */
void xbox_SetLitPattern(int key_dir, int key_col, int amb_first,
                        int blob_first, int blob_groups);

#endif
