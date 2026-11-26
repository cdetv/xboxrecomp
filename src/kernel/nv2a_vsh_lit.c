/**
 * Lit vertex programs: the scan. See nv2a_vsh_lit.h for what it looks for.
 *
 * The program runs once symbolically, in order, with run_program's rules
 * (nv2a_vsh_interp.c): sources read with their swizzle and negate, both
 * units read before either writes, an ILU paired with a MAC op writes R1,
 * reads of temps 13-15 read R0, temp 12 is oPos. Each temp component holds
 * a Sym: an id naming the value (the instruction lane that wrote it, an
 * input lane, a constant lane; a MOV copies the id, so a copy is the same
 * value) and a kind saying what the value is when the scan recognises it.
 * Every op whose operands match a step of the pattern makes the next kind;
 * anything else makes an unknown value with a fresh id, which can never
 * turn back into a lighting term. So a colour output is a recognised term
 * only if every step that led to it was the pattern's.
 */
#include "nv2a_vsh_lit.h"

#include <stdio.h>
#include <string.h>

enum {
    S_ZERO, S_UNK, S_CONST, S_IN,
    S_SELFDOT,      /* dot(v, v)                   n: v's lane ids        */
    S_RSQ,          /* rsq(dot(v, v))              n: v's lane ids        */
    S_NORM,         /* v.<b> * rsq(dot(v, v))      n: v's lane ids        */
    S_D1,           /* dph(n, c[a])                n: the vector read     */
    S_AMB,          /* ambient channel b           n: the normal          */
    S_KD,           /* dot(n, -c[key_dir])         n                      */
    S_KEY,          /* max(KD, 0)                  n                      */
    S_BX, S_BY,     /* c[blob x / y row of group a].<b> - x / y           */
    S_BX2, S_BY2,   /* their squares                                      */
    S_BD2,          /* the squared distance, lane b of group a            */
    S_BAB,          /* h >= c[clip row].<b>                               */
    S_BS,           /* that times c[strength row].<b>                     */
    S_BR,           /* rcc(BD2) of lane b                                 */
    S_BG,           /* group a's sum over its four lanes                  */
    S_KT,           /* key * (1 - the sums of groups in mask a), at least
                       c[lo_row].<lo_comp> when lo_row is set             */
    S_KTC,          /* KT * c[key_col].<b>                                */
    S_AMBF,         /* AMB(b) * an input lane (source a of that op)       */
    S_KTCA,         /* KT * c[key_col].<b> + AMB(b), or + AMBF(b) (fid)   */
    S_W1,           /* c[a].<b> * (1 - the sums)                          */
    S_W2,           /* c[a].<b> - a group's sum                           */
    S_W3            /* c[a].<b> - a lane of the normal (id c[0])          */
};

#define F_NORM 1    /* built on a normalised normal */
#define NEWID 0xFFFF

typedef struct {
    uint16_t id;        /* equal ids (and negates): the same value */
    uint8_t kind, neg;
    uint8_t a, b, f, k; /* k: the instruction of the key's dot product */
    uint16_t n[3];      /* the normal's lane ids (a vector's, SELFDOT..NORM) */
    uint16_t c[3];      /* blob x, y, height: their ids */
    uint16_t fid;       /* KTCA: the AMBF value added (0: plain ambient) */
    uint8_t fsrc;       /*   and the factor's source in its instruction */
    uint8_t lo_row, lo_comp;  /* KT..: the term's floor (lo_row 0xFF none) */
} Sym;

static Nv2aLitRegs s_regs = { -1, -1, -1, -1, 0 };

void xbox_SetLitPattern(int key_dir, int key_col, int amb_first,
                        int blob_first, int blob_groups)
{
    if (blob_groups < 0) blob_groups = 0;
    if (blob_groups > 3) blob_groups = 3;
    s_regs.key_dir = key_dir;
    s_regs.key_col = key_col;
    s_regs.amb_first = amb_first;
    s_regs.blob_first = blob_first;
    s_regs.blob_groups = blob_groups;
    fprintf(stderr, "[VSHLIT] pattern: key c%d colour c%d, ambient c%d-c%d,"
            " blobs c%d (%d groups)\n", key_dir, key_col, amb_first,
            amb_first + 11, blob_first, blob_groups);
}

const Nv2aLitRegs *nv2a_vsh_lit_regs(void)
{
    return s_regs.key_dir >= 0 ? &s_regs : NULL;
}

static volatile int s_mode;

void xbox_SetPerPixelLighting(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 1;
    s_mode = mode;
    fprintf(stderr, "[VSHLIT] per-pixel lighting %s\n",
            mode == 2 ? "on in even frames" : mode ? "on" : "off");
}

int nv2a_vsh_lit_mode(void)
{
    return s_regs.key_dir >= 0 ? s_mode : 0;
}

static volatile int s_view;

void xbox_SetLitView(int view)
{
    static const char *const k[NV2A_LIT_VIEWS] = {
        "off", "vertex", "key", "ambient", "blobs", "term", "face"
    };
    if (view < 0 || view >= NV2A_LIT_VIEWS)
        view = 0;
    s_view = view;
    fprintf(stderr, "[VSHLIT] view %s\n", k[view]);
}

int nv2a_vsh_lit_view(void)
{
    return s_view;
}

static volatile int s_soft_ramp, s_soft_knee;

void xbox_SetBlobSoftness(int ramp, int knee)
{
    if (ramp < 0) ramp = 0;
    if (ramp > 255) ramp = 255;
    if (knee < 0) knee = 0;
    if (knee > 100) knee = 100;     /* past 1 the corner would lift s = 0 */
    s_soft_ramp = ramp;
    s_soft_knee = knee;
    fprintf(stderr, "[VSHLIT] blob softness: ramp %d, knee %d\n", ramp, knee);
}

void nv2a_vsh_lit_soft(int *ramp, int *knee)
{
    if (ramp) *ramp = s_soft_ramp;
    if (knee) *knee = s_soft_knee;
}

uint32_t nv2a_vsh_lit_soft_bits(void)
{
    return ((uint32_t)s_soft_ramp << 8) | ((uint32_t)s_soft_knee << 16);
}

uint32_t nv2a_vsh_lit_word(const Nv2aVshLit *l)
{
    uint32_t w;

    if (!l || l->layout == NV2A_LIT_NONE)
        return 0;
    w = (uint32_t)l->layout | ((uint32_t)(l->groups & 7) << 3);
    if (l->lo_row != 0xFF)
        w |= 0x40u | ((uint32_t)l->lo_row << 8)
             | ((uint32_t)(l->lo_comp & 3) << 16);
    if (l->d1w)
        w |= ((uint32_t)(l->d1w & 3) << 18) | ((uint32_t)l->d1w_row << 20)
             | ((uint32_t)(l->d1w_comp & 3) << 28)
             | ((uint32_t)(l->d1w_comp2 & 3) << 30);
    return w;
}

/* ---- values ------------------------------------------------------------- */

/* Ids: 0 the zero; a write 1 + (ins * 2 + unit) * 4 + lane (unit 0 MAC,
 * 1 ILU); inputs, constants and a0-relative reads in ranges of their own. */
#define ID_IN    0x2000u
#define ID_CONST 0x3000u
#define ID_REL   0x4000u

static int is_write_id(uint16_t id) { return id >= 1 && id < ID_IN; }
static uint32_t id_ins(uint16_t id)  { return (uint32_t)(id - 1) >> 3; }
static uint32_t id_unit(uint16_t id) { return ((uint32_t)(id - 1) >> 2) & 1; }
static uint32_t id_lane(uint16_t id) { return (uint32_t)(id - 1) & 3; }

static Sym mk(int kind, uint32_t id)
{
    Sym s;
    memset(&s, 0, sizeof s);
    s.kind = (uint8_t)kind;
    s.id = (uint16_t)id;
    s.lo_row = 0xFF;
    return s;
}

static Sym unk(void) { return mk(S_UNK, NEWID); }

static void read_src(const Nv2aVshIns *I, uint32_t n, int which,
                     Sym t[13][4], Sym out[4])
{
    const Nv2aVshSrc *d = &I->src[which];
    int j;

    for (j = 0; j < 4; j++) {
        uint32_t comp = d->sw[j];
        Sym s;
        if (d->mux == 1) {
            uint32_t r = d->reg == 12 ? 12u : (d->reg < 12 ? d->reg : 0u);
            s = t[r][comp];
        } else if (d->mux == 2) {
            s = mk(S_IN, ID_IN + I->iidx * 4u + comp);
            s.a = (uint8_t)I->iidx;
            s.b = (uint8_t)comp;
        } else if (I->rel) {
            s = mk(S_UNK, ID_REL + n * 16u + (uint32_t)which * 4u + (uint32_t)j);
        } else {
            uint32_t row = I->cidx < NV2A_VSH_CONSTANTS ? I->cidx : 0u;
            s = mk(S_CONST, ID_CONST + row * 4u + comp);
            s.a = (uint8_t)row;
            s.b = (uint8_t)comp;
        }
        if (d->neg)
            s.neg ^= 1;
        out[j] = s;
    }
}

static int is_const(const Sym *s, int row, int comp, int neg)
{
    return s->kind == S_CONST && s->a == row && s->b == comp && s->neg == neg;
}

/* Lanes 0-2 of v as a vector of written (or input) values, no negate. */
static int is_vector(const Sym v[4])
{
    int j;
    for (j = 0; j < 3; j++)
        if (v[j].neg || v[j].kind == S_ZERO || v[j].kind == S_CONST
            || v[j].id == NEWID)
            return 0;
    return 1;
}

static int same_vec(const uint16_t a[3], const uint16_t b[3])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* F_NORM when lanes 0-2 are one normalised vector, x y z in order. */
static uint8_t norm_flag(const Sym v[4])
{
    int j;
    for (j = 0; j < 3; j++)
        if (v[j].kind != S_NORM || v[j].b != j || v[j].neg
            || !same_vec(v[j].n, v[0].n))
            return 0;
    return F_NORM;
}

static void vec_ids(const Sym v[4], uint16_t n[3])
{
    n[0] = v[0].id; n[1] = v[1].id; n[2] = v[2].id;
}

/* The blob group and row (0 x, 1 y, 2 clip height, 3 strength) of a
 * constant, or -1. */
static int blob_row(const Nv2aLitRegs *R, const Sym *s, int *g)
{
    int d;
    if (s->kind != S_CONST || R->blob_first < 0)
        return -1;
    d = (int)s->a - R->blob_first;
    if (d < 0 || d >= 4 * R->blob_groups)
        return -1;
    *g = d / 4;
    return d % 4;
}

/* KEY as a key term with no groups. */
static int as_term(const Sym *s, Sym *t)
{
    if (s->neg)
        return 0;
    if (s->kind == S_KT) {
        *t = *s;
        return 1;
    }
    if (s->kind == S_KEY) {
        *t = *s;
        t->kind = S_KT;
        t->a = 0;
        memset(t->c, 0, sizeof t->c);
        return 1;
    }
    return 0;
}

/* Two groups' coordinates agree (a group-less term has none). */
static int coords_join(uint8_t mask, const uint16_t c[3], uint8_t mask2,
                       const uint16_t c2[3], uint16_t out[3])
{
    if (!mask) { memcpy(out, c2, 6); return 1; }
    if (!mask2) { memcpy(out, c, 6); return 1; }
    if (!same_vec(c, c2))
        return 0;
    memcpy(out, c, 6);
    return 1;
}

/* ---- the ops ------------------------------------------------------------ */

static Sym mul_lane(const Nv2aLitRegs *R, const Sym *x, const Sym *y, int j,
                    int x_src)
{
    Sym r = unk(), t;
    int g, k;

    /* x * x of a blob difference */
    if (x->id == y->id && x->neg == y->neg
        && (x->kind == S_BX || x->kind == S_BY)) {
        r.kind = x->kind == S_BX ? S_BX2 : S_BY2;
        r.a = x->a; r.b = x->b;
        memcpy(r.c, x->c, sizeof r.c);
        return r;
    }
    /* (h >= clip) * strength, the same lane */
    if (x->kind == S_BAB && !x->neg && (k = blob_row(R, y, &g)) == 3
        && g == x->a && y->b == x->b && !y->neg) {
        r.kind = S_BS;
        r.a = x->a; r.b = x->b;
        memcpy(r.c, x->c, sizeof r.c);
        return r;
    }
    /* rsq(dot(v, v)) * v.<j> */
    if (x->kind == S_RSQ && !x->neg && j < 3 && !y->neg
        && y->id == x->n[j] && y->id != NEWID) {
        r.kind = S_NORM;
        r.b = (uint8_t)j;
        memcpy(r.n, x->n, sizeof r.n);
        return r;
    }
    /* term * key colour */
    if (j < 3 && as_term(x, &t) && is_const(y, R->key_col, j, 0)) {
        r = t;
        r.kind = S_KTC;
        r.b = (uint8_t)j;
        r.id = NEWID;
        return r;
    }
    /* ambient * an input lane */
    if (x->kind == S_AMB && !x->neg && x->b == j && y->kind == S_IN
        && !y->neg) {
        r = *x;
        r.kind = S_AMBF;
        r.a = (uint8_t)(1 - x_src);     /* the factor's source: the other */
        r.id = NEWID;
        return r;
    }
    return r;
}

static Sym op_mul(const Nv2aLitRegs *R, const Sym *a, const Sym *b, int j)
{
    Sym r = mul_lane(R, a, b, j, 0);
    if (r.kind == S_UNK)
        r = mul_lane(R, b, a, j, 1);
    return r;
}

static Sym op_add(const Nv2aLitRegs *R, const Sym *a, const Sym *c, int j)
{
    const Sym *x, *k;
    Sym r = unk();
    int pass, g, row;

    for (pass = 0; pass < 2; pass++) {
        x = pass ? c : a;               /* the negated value */
        k = pass ? a : c;               /* the constant */
        if (k->kind != S_CONST || k->neg || !x->neg)
            continue;
        /* c[blob x / y].<j> - x */
        if ((row = blob_row(R, k, &g)) >= 0 && row < 2 && k->b == j
            && is_write_id(x->id)) {
            r.kind = row == 0 ? S_BX : S_BY;
            r.a = (uint8_t)g; r.b = (uint8_t)j;
            r.c[row] = x->id;
            return r;
        }
        /* c - a group's sum: oD1.w */
        if (x->kind == S_BG) {
            r.kind = S_W2;
            r.a = k->a; r.b = k->b;
            r.f = (uint8_t)(1u << x->a);
            memcpy(r.c, x->c, sizeof r.c);
            return r;
        }
        /* c - a normal lane: oD1.w */
        if (x->kind == S_NORM) {
            r.kind = S_W3;
            r.a = k->a; r.b = k->b;
            r.c[0] = x->id;
            return r;
        }
    }
    return r;
}

static Sym op_mad(const Nv2aLitRegs *R, const Sym *a, const Sym *b,
                  const Sym *c, int j)
{
    Sym r = unk(), t, u;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        const Sym *x = pass ? b : a, *y = pass ? a : b;
        /* y*y + x^2 (either order): the squared distance */
        if (x->id == y->id && x->neg == y->neg && !c->neg
            && ((x->kind == S_BY && c->kind == S_BX2)
                || (x->kind == S_BX && c->kind == S_BY2))
            && x->a == c->a && x->b == c->b) {
            r.kind = S_BD2;
            r.a = x->a; r.b = x->b;
            r.c[0] = x->kind == S_BX ? x->c[0] : c->c[0];
            r.c[1] = x->kind == S_BY ? x->c[1] : c->c[1];
            return r;
        }
        /* -sum * key + term: one more group off the key (not after a
         * floor: that would be another formula) */
        if (x->kind == S_BG && x->neg && y->kind == S_KEY && !y->neg
            && as_term(c, &t) && t.lo_row == 0xFF && same_vec(t.n, y->n)
            && (c->kind == S_KEY ? c->id == y->id : 1)
            && !(t.a & (1u << x->a))) {
            u = t;
            u.id = NEWID;
            u.a = (uint8_t)(t.a | (1u << x->a));
            if (!coords_join(t.a, t.c, (uint8_t)(1u << x->a), x->c, u.c))
                return r;
            return u;
        }
        /* term * key colour + ambient (layout C), or + ambient * an
         * input (layout D) */
        if (j < 3 && as_term(x, &t) && is_const(y, R->key_col, j, 0)
            && (c->kind == S_AMB || c->kind == S_AMBF) && !c->neg
            && c->b == j && same_vec(c->n, t.n)) {
            u = t;
            u.id = NEWID;
            u.kind = S_KTCA;
            u.b = (uint8_t)j;
            u.f = (uint8_t)(t.f & c->f);
            if (c->kind == S_AMBF) {
                u.fid = c->id;
                u.fsrc = c->a;
            }
            return u;
        }
    }
    return r;
}

static Sym op_dp3(const Nv2aLitRegs *R, const Sym A[4], const Sym B[4],
                  uint32_t n)
{
    Sym r = unk();
    int pass, j;

    /* dot(v, v) */
    if (is_vector(A)) {
        for (j = 0; j < 3; j++)
            if (A[j].id != B[j].id || B[j].neg)
                break;
        if (j == 3) {
            r.kind = S_SELFDOT;
            vec_ids(A, r.n);
            return r;
        }
    }
    /* dot(n, -key) or dot(-n, key) */
    for (pass = 0; pass < 2; pass++) {
        const Sym *v = pass ? B : A, *k = pass ? A : B;
        int kneg = k[0].neg, ok = 1;
        Sym w[4];
        for (j = 0; j < 3; j++) {
            if (!is_const(&k[j], R->key_dir, j, kneg))
                ok = 0;
            w[j] = v[j];
            w[j].neg = (uint8_t)(v[j].neg ^ kneg ^ 1);   /* n's own sign */
        }
        w[3] = v[3];
        if (ok && v[0].neg == v[1].neg && v[1].neg == v[2].neg
            && is_vector(w)) {
            r.kind = S_KD;
            vec_ids(w, r.n);
            r.f = norm_flag(w);
            r.k = (uint8_t)n;
            return r;
        }
    }
    return r;
}

static Sym op_dph(const Nv2aLitRegs *R, const Sym A[4], const Sym B[4])
{
    Sym r = unk();
    int j, ch;

    /* dph(v, c[row]) */
    if (is_vector(A) && B[0].kind == S_CONST) {
        int row = B[0].a;
        for (j = 0; j < 4; j++)
            if (!is_const(&B[j], row, j, 0))
                break;
        if (j == 4) {
            r.kind = S_D1;
            r.a = (uint8_t)row;
            vec_ids(A, r.n);
            r.f = norm_flag(A);
            return r;
        }
    }
    /* dph(n, (dph(n, c[a]) .. dph(n, c[a+3]))): an ambient channel */
    if (is_vector(A) && R->amb_first >= 0 && B[0].kind == S_D1) {
        uint16_t v[3];
        vec_ids(A, v);
        ch = ((int)B[0].a - R->amb_first) / 4;
        if (ch >= 0 && ch < 3 && (int)B[0].a == R->amb_first + 4 * ch) {
            for (j = 0; j < 4; j++)
                if (B[j].kind != S_D1 || B[j].neg
                    || B[j].a != R->amb_first + 4 * ch + j
                    || !same_vec(B[j].n, v))
                    break;
            if (j == 4) {
                r.kind = S_AMB;
                r.b = (uint8_t)ch;
                memcpy(r.n, v, sizeof r.n);
                r.f = (uint8_t)(norm_flag(A) & B[0].f);
                return r;
            }
        }
    }
    /* dph(-(sums), key): key * (1 - sums); dph(-(sums), c): oD1.w */
    {
        uint8_t mask = 0;
        uint16_t c[3] = { 0, 0, 0 };
        int ok = 1;
        for (j = 0; j < 3 && ok; j++) {
            if (A[j].kind == S_ZERO)
                continue;
            if (A[j].kind != S_BG || !A[j].neg || (mask & (1u << A[j].a)))
                ok = 0;
            else if (!coords_join(mask, c, (uint8_t)(1u << A[j].a), A[j].c, c))
                ok = 0;
            else
                mask |= (uint8_t)(1u << A[j].a);
        }
        for (j = 1; j < 4 && ok; j++)
            if (B[j].id != B[0].id || B[j].neg != B[0].neg
                || B[j].kind != B[0].kind)
                ok = 0;
        if (ok && mask && B[0].kind == S_KEY && !B[0].neg) {
            r = B[0];
            r.kind = S_KT;
            r.id = NEWID;
            r.a = mask;
            memcpy(r.c, c, sizeof r.c);
            return r;
        }
        if (ok && mask && B[0].kind == S_CONST && !B[0].neg) {
            r.kind = S_W1;
            r.a = B[0].a; r.b = B[0].b;
            r.f = mask;                 /* (W kinds: f holds the groups) */
            memcpy(r.c, c, sizeof r.c);
            return r;
        }
    }
    return r;
}

static Sym op_dp4(const Sym A[4], const Sym B[4])
{
    Sym r = unk();
    int pass, j;

    /* sum of rcc(d^2) * strength over a group's four lanes */
    for (pass = 0; pass < 2; pass++) {
        const Sym *x = pass ? B : A, *y = pass ? A : B;
        for (j = 0; j < 4; j++)
            if (x[j].kind != S_BR || y[j].kind != S_BS || x[j].neg
                || y[j].neg || x[j].b != j || y[j].b != j
                || x[j].a != x[0].a || y[j].a != x[0].a
                || x[j].c[0] != x[0].c[0] || x[j].c[1] != x[0].c[1]
                || y[j].c[2] != y[0].c[2])
                break;
        if (j == 4) {
            r.kind = S_BG;
            r.a = x[0].a;
            r.c[0] = x[0].c[0];
            r.c[1] = x[0].c[1];
            r.c[2] = y[0].c[2];
            return r;
        }
    }
    return r;
}

/* max(term, c): the term floored (a layout C / D program keeps big blob
 * sums from taking light off the ambient) */
static Sym op_max(const Sym *a, const Sym *b)
{
    Sym t;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        const Sym *x = pass ? b : a, *y = pass ? a : b;
        if (as_term(x, &t) && t.lo_row == 0xFF && y->kind == S_CONST
            && !y->neg) {
            t.id = NEWID;
            t.lo_row = y->a;
            t.lo_comp = y->b;
            return t;
        }
    }
    return unk();
}

static Sym op_sge(const Nv2aLitRegs *R, const Sym *a, const Sym *b, int j)
{
    Sym r = unk();
    int g;

    /* h >= c[clip].<j> */
    if (!a->neg && is_write_id(a->id) && blob_row(R, b, &g) == 2
        && b->b == j && !b->neg) {
        r.kind = S_BAB;
        r.a = (uint8_t)g; r.b = (uint8_t)j;
        r.c[2] = a->id;
    }
    return r;
}

static void run_mac(const Nv2aLitRegs *R, const Nv2aVshIns *I, uint32_t n,
                    const Sym A[4], const Sym B[4], const Sym C[4],
                    Sym out[4])
{
    Sym r;
    int j;

    switch (I->mac) {
    case 1:                                             /* mov */
        for (j = 0; j < 4; j++) out[j] = A[j];
        return;
    case 2:
        for (j = 0; j < 4; j++) out[j] = op_mul(R, &A[j], &B[j], j);
        return;
    case 3:
        for (j = 0; j < 4; j++) out[j] = op_add(R, &A[j], &C[j], j);
        return;
    case 4:
        for (j = 0; j < 4; j++) out[j] = op_mad(R, &A[j], &B[j], &C[j], j);
        return;
    case 5: r = op_dp3(R, A, B, n); break;
    case 6: r = op_dph(R, A, B); break;
    case 7: r = op_dp4(A, B); break;
    case 10:
        for (j = 0; j < 4; j++) out[j] = op_max(&A[j], &B[j]);
        return;
    case 12:
        for (j = 0; j < 4; j++) out[j] = op_sge(R, &A[j], &B[j], j);
        return;
    default: r = unk(); break;
    }
    for (j = 0; j < 4; j++)
        out[j] = r;
}

static void run_ilu(const Nv2aVshIns *I, const Sym C[4], Sym out[4])
{
    Sym r = unk();
    int j;

    switch (I->ilu) {
    case 1:                                             /* mov */
        for (j = 0; j < 4; j++) out[j] = C[j];
        return;
    case 3:                                             /* rcc */
        if (C[0].kind == S_BD2 && !C[0].neg) {
            r.kind = S_BR;
            r.a = C[0].a; r.b = C[0].b;
            memcpy(r.c, C[0].c, sizeof r.c);
        }
        break;
    case 4:                                             /* rsq */
        if (C[0].kind == S_SELFDOT && !C[0].neg) {
            r.kind = S_RSQ;
            memcpy(r.n, C[0].n, sizeof r.n);
        }
        break;
    case 7:                                             /* lit: y = max(x, 0) */
        for (j = 0; j < 4; j++) out[j] = unk();
        if (C[0].kind == S_KD && !C[0].neg) {
            out[1] = C[0];
            out[1].kind = S_KEY;
            out[1].id = NEWID;
        }
        return;
    }
    for (j = 0; j < 4; j++)
        out[j] = r;
}

static void write_lanes(Sym *dst, const Sym src[4], uint32_t mask)
{
    int j;
    for (j = 0; j < 4; j++)
        if (mask & (8u >> j))
            dst[j] = src[j];
}

/* ---- the scan ----------------------------------------------------------- */

static int reads_row(const Nv2aVshIns *I, int lo, int hi)
{
    int k;
    for (k = 0; k < 3; k++)
        if ((I->need & (1 << k)) && I->src[k].mux != 1
            && I->src[k].mux != 2 && !I->rel && (int)I->cidx >= lo
            && (int)I->cidx <= hi)
            return 1;
    return 0;
}

/* Layouts whose oD1 the lit pixel shader writes (A, B); in C and D oD1 is
 * not lighting and passes through as interpolated, its alpha too. */
static int layout_has_d1(int layout)
{
    return layout == NV2A_LIT_A || layout == NV2A_LIT_B;
}

static int fail(Nv2aVshLit *o, const char *why)
{
    o->layout = NV2A_LIT_NONE;
    o->why = why;
    return 1;
}

int nv2a_vsh_lit_scan(const Nv2aVshIns *ins, uint32_t count,
                      const Nv2aLitRegs *R, Nv2aVshLit *o)
{
    Sym t[13][4], out[2][4];            /* temps (12 = oPos); oD0, oD1 */
    Sym A[4], B[4], C[4], M[4], I4[4], zero4[4];
    uint32_t n, j, k;
    int any = 0;
    Sym term;
    uint8_t f;

    memset(o, 0, sizeof *o);
    o->n_ins = o->f_ins = 0xFFFF;
    o->p_ins[0] = o->p_ins[1] = o->p_ins[2] = 0xFFFF;
    o->lo_row = 0xFF;
    if (!R || R->key_dir < 0 || !ins || !count)
        return 0;
    /* A candidate dots something with the key light's row; other programs
     * may read the row for something else (in Conker 84 of 264 multiply
     * by it), and say nothing. */
    for (n = 0; n < count; n++) {
        if (ins[n].mac == 5 && reads_row(&ins[n], R->key_dir, R->key_dir))
            any = 1;
        if (ins[n].rel)
            o->skinned = 1;
    }
    if (!any)
        return 0;

    for (j = 0; j < 4; j++)
        zero4[j] = mk(S_ZERO, 0);
    for (n = 0; n < 13; n++)
        memcpy(t[n], zero4, sizeof zero4);
    memcpy(out[0], zero4, sizeof zero4);
    memcpy(out[1], zero4, sizeof zero4);

    for (n = 0; n < count; n++) {
        const Nv2aVshIns *I = &ins[n];
        memcpy(A, zero4, sizeof A);
        memcpy(B, zero4, sizeof B);
        memcpy(C, zero4, sizeof C);
        if (I->need & 1) read_src(I, n, 0, t, A);
        if (I->need & 2) read_src(I, n, 1, t, B);
        if (I->need & 4) read_src(I, n, 2, t, C);
        memcpy(M, zero4, sizeof M);
        memcpy(I4, zero4, sizeof I4);
        if (I->mac)
            run_mac(R, I, n, A, B, C, M);
        if (I->ilu)
            run_ilu(I, C, I4);
        /* New values get the id of the lane that makes them (a MOV keeps
         * its source's); then the writes, in run_program's order. */
        for (j = 0; j < 4; j++) {
            if (M[j].id == NEWID)
                M[j].id = (uint16_t)(1 + ((n * 2 + 0) * 4 + j));
            if (I4[j].id == NEWID)
                I4[j].id = (uint16_t)(1 + ((n * 2 + 1) * 4 + j));
        }
        if (I->mac && I->mac != 13 && I->mac_mask && I->tdst <= 12)
            write_lanes(t[I->tdst], M, I->mac_mask);
        if (I->ilu && I->ilu_mask) {
            uint32_t it = I->mac ? 1u : I->tdst;
            if (it <= 12)
                write_lanes(t[it], I4, I->ilu_mask);
        }
        if (I->omask && I->out_o && (I->oaddr == 3 || I->oaddr == 4)) {
            int have = I->out_ilu ? I->ilu != 0 : I->mac != 0;
            write_lanes(out[I->oaddr - 3],
                        have ? (I->out_ilu ? I4 : M) : zero4, I->omask);
        }
    }

    /* The key term: what oD1 (A, B) or oD0 (C) is made of. */
    if (out[0][0].kind == S_KTCA) {
        const Sym *s0 = &out[0][0];
        for (j = 0; j < 3; j++)
            if (out[0][j].kind != S_KTCA || out[0][j].b != j
                || out[0][j].neg || out[0][j].a != s0->a
                || !same_vec(out[0][j].n, s0->n)
                || !same_vec(out[0][j].c, s0->c)
                || out[0][j].lo_row != s0->lo_row
                || out[0][j].lo_comp != s0->lo_comp
                || !out[0][j].fid != !s0->fid
                || (s0->fid && (id_ins(out[0][j].fid) != id_ins(s0->fid)
                                || out[0][j].fsrc != s0->fsrc)))
                return fail(o, "oD0 not term x colour + ambient in every lane");
        term = *s0;
        o->layout = NV2A_LIT_C;
        if (s0->fid) {
            o->layout = NV2A_LIT_D;
            o->f_ins = (uint16_t)id_ins(s0->fid);
            o->f_src = s0->fsrc;
            o->f_reg = (uint8_t)ins[o->f_ins].iidx;
        }
    } else {
        for (j = 0; j < 3; j++)
            if (out[1][j].kind != S_KTC || out[1][j].b != j
                || out[1][j].neg || out[1][j].a != out[1][0].a
                || !same_vec(out[1][j].n, out[1][0].n)
                || !same_vec(out[1][j].c, out[1][0].c)
                || out[1][j].lo_row != out[1][0].lo_row
                || out[1][j].lo_comp != out[1][0].lo_comp)
                return fail(o, out[0][0].kind == S_AMB
                                   || out[0][0].kind == S_AMBF
                               ? "oD1 not the key term x colour"
                               : "oD0 not recognised");
        term = out[1][0];
        if (out[0][0].kind == S_AMB) {
            for (j = 0; j < 3; j++)
                if (out[0][j].kind != S_AMB || out[0][j].b != j
                    || out[0][j].neg || !same_vec(out[0][j].n, term.n))
                    return fail(o, "oD0 not the ambient in every lane");
            o->layout = NV2A_LIT_A;
        } else if (out[0][0].kind == S_AMBF) {
            uint32_t w = id_ins(out[0][0].id);
            for (j = 0; j < 3; j++)
                if (out[0][j].kind != S_AMBF || out[0][j].b != j
                    || out[0][j].neg || !same_vec(out[0][j].n, term.n)
                    || id_ins(out[0][j].id) != w
                    || out[0][j].a != out[0][0].a)
                    return fail(o, "oD0 not ambient x one input");
            o->layout = NV2A_LIT_B;
            o->f_ins = (uint16_t)w;
            o->f_src = out[0][0].a;
            o->f_reg = (uint8_t)ins[w].iidx;
        } else {
            return fail(o, "oD0 not recognised");
        }
    }
    f = term.f;
    for (j = 0; j < 3; j++)
        if (o->layout == NV2A_LIT_A || o->layout == NV2A_LIT_B)
            f &= out[0][j].f;
    if (!(f & F_NORM))
        return fail(o, "normal not normalised");

    /* Where the terms are. */
    o->groups = term.a;
    o->lo_row = term.lo_row;
    o->lo_comp = term.lo_comp;
    o->n_ins = term.k;
    k = term.k;
    o->n_src = (ins[k].src[0].mux != 1 && ins[k].src[0].mux != 2
                && !ins[k].rel && ins[k].cidx == R->key_dir) ? 1 : 0;
    o->n_temp = ins[k].src[o->n_src].mux == 1 ? ins[k].src[o->n_src].reg
                                              : 0xFF;
    if (term.a) {
        for (j = 0; j < 3; j++) {
            if (!is_write_id(term.c[j]))
                return fail(o, "a blob coordinate not computed");
            o->p_ins[j] = (uint16_t)id_ins(term.c[j]);
            o->p_unit[j] = (uint8_t)id_unit(term.c[j]);
            o->p_comp[j] = (uint8_t)id_lane(term.c[j]);
        }
    }
    /* oD1.w (the title's combiners may read it: Conker's do), worked out
     * per pixel too when it is one of these and its blobs are the key
     * term's */
    switch (out[1][3].kind) {
    case S_W1:
    case S_W2:
        if (out[1][3].f == term.a && same_vec(out[1][3].c, term.c)
            && layout_has_d1(o->layout))
            o->d1w = out[1][3].kind == S_W1 ? NV2A_LIT_W_BLOBS_MUL
                                            : NV2A_LIT_W_BLOBS_SUB;
        break;
    case S_W3:
        for (j = 0; j < 3 && layout_has_d1(o->layout); j++)
            if (term.n[j] == out[1][3].c[0]) {
                o->d1w = NV2A_LIT_W_NORMAL_SUB;
                o->d1w_comp2 = (uint8_t)j;
            }
        break;
    }
    if (o->d1w) {
        o->d1w_row = out[1][3].a;
        o->d1w_comp = out[1][3].b;
    }
    return 1;
}

void nv2a_vsh_lit_describe(const Nv2aVshLit *l, char *buf, size_t cap)
{
    static const char xyzw[] = "xyzw";
    size_t len;

    if (!cap)
        return;
    if (l->layout == NV2A_LIT_NONE) {
        snprintf(buf, cap, "per vertex (%s)%s", l->why ? l->why : "?",
                 l->skinned ? ", skinned" : "");
        return;
    }
    len = (size_t)snprintf(buf, cap, "layout %c, normal %s%u@%u",
                           "?ABCD"[l->layout], l->n_temp == 0xFF ? "?" : "r",
                           l->n_temp == 0xFF ? 0u : l->n_temp, l->n_ins);
    if (len < cap && (l->layout == NV2A_LIT_B || l->layout == NV2A_LIT_D))
        len += (size_t)snprintf(buf + len, cap - len, ", factor v%u@%u",
                                l->f_reg, l->f_ins);
    if (len < cap && l->lo_row != 0xFF)
        len += (size_t)snprintf(buf + len, cap - len, ", term >= c%u.%c",
                                l->lo_row, xyzw[l->lo_comp & 3]);
    if (len < cap && l->groups)
        len += (size_t)snprintf(buf + len, cap - len,
                                ", blobs x/y/h @%u/%u/%u, group%s%s%s%s",
                                l->p_ins[0], l->p_ins[1], l->p_ins[2],
                                (l->groups & (l->groups - 1)) ? "s" : "",
                                (l->groups & 1) ? " 0" : "",
                                (l->groups & 2) ? " 1" : "",
                                (l->groups & 4) ? " 2" : "");
    else if (len < cap)
        len += (size_t)snprintf(buf + len, cap - len, ", no blobs");
    if (len < cap && l->d1w)
        len += (size_t)snprintf(buf + len, cap - len, ", oD1.w %s c%u.%c",
                                l->d1w == NV2A_LIT_W_BLOBS_MUL
                                    ? "(1 - blobs) x"
                                : l->d1w == NV2A_LIT_W_BLOBS_SUB
                                    ? "- blobs +" : "- normal +",
                                l->d1w_row, xyzw[l->d1w_comp & 3]);
    if (len < cap && l->skinned)
        snprintf(buf + len, cap - len, ", skinned");
}
