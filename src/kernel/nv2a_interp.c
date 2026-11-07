/**
 * Frame interpolation (RECOMP_FRAME_INTERP=<n>): n pictures per title frame,
 * the title's own timing left alone.
 *
 * The executor records each frame as it runs it: every method word and run of
 * draw words between two flips (the stream), the front end's state and the
 * vertex unit's (program, constants) where the frame began, and at each draw
 * a key that names it and the vertex constants it drew with. At frame N's flip
 * the stream is run again n - 1 times from N's start, with the title's side
 * effects off (fences, flips, visibility reports), and before each draw the
 * constants are set to a blend of N's and those of the same draw in frame N-1,
 * at t = k / n. Running the front end again rather than the back end's calls
 * redoes everything a frame is drawn with in order -- clears, copies of
 * surfaces read as textures, depth reads, the host's depth of field -- and a
 * batch the CPU transforms (a program that writes constants, points) runs
 * through the interpreter on the blended constants too.
 *
 * A paired draw whose partner drew the same vertex array with another
 * index list (Conker builds its ground pieces' lists each frame from what
 * that frame sees) is drawn with the partner's list too, if it is opaque:
 * the in-between camera sees triangles either frame may have left out.
 * (Also taking partners with other arrays drew shards: not the same
 * pieces.)
 *
 * A replay's colour clear of the shown surface starts from the frame
 * before's picture instead (NV_INTERP_UNDER): where the newer frame drew
 * nothing (culled past its view, seen by an in-between camera), that shows
 * instead of the clear colour.
 *
 * Each picture is copied out of the frame's surface (the back end's
 * NV_INTERP_SAVE); the first is presented in place of N at the flip, N goes
 * back into its surface, and the rest -- N last -- are presented at even
 * times through the next frame (nv2a_interp_tick, from the executor's waits
 * and its walk). So the title still waits only for its own frames, and what
 * is shown runs about one frame behind it.
 *
 * Pairs: draws of N and N-1 with the same key, in their order among draws
 * with that key. The key is the program, the position array, the index
 * count, texture stage 0 and the colour target's shape. A draw without a pair, and a
 * frame with too few pairs (a load), one the title marks as a camera cut
 * (NV_HOST_FX_INTERP_CUT in its stream), or one whose camera jumped
 * (camera_jumped: a cut the title does not mark; RECOMP_INTERP_AUTOCUT=0
 * off) is drawn as N. Constants
 * that are the same in both, or all zero in one of them, are N's. A pair
 * whose own matrices jumped (pair_jumped: turned > 30 degrees, moved by over
 * half its translation -- a character put somewhere else between two shots,
 * a fast spin) is drawn as N too (RECOMP_INTERP_JUMPS=0: blended).
 *
 * Guest memory the frame read (vertex arrays, textures) is read again when
 * the stream runs again; what the title has rewritten since is drawn as it is
 * now.
 *
 * RECOMP_INTERP_LOG=1: an [INTERP] line every 10 s (frames, draws, pairs,
 * replay time, pictures shown late or dropped). RECOMP_INTERP_MODE (tests):
 * 1 = every picture is N (pacing only, nothing drawn again), 2 = drawn again
 * with N's own constants (must come out as N), 3 = the first picture is N-1
 * drawn again as it was (must come out as N-1). RECOMP_INTERP_TRACE=<file>:
 * when that file appears, the next interpolated frame's draws are listed
 * ([ITRACE]: key parts and the constants that differ from the frame before).
 * RECOMP_INTERP_CTL_DIR=<dir> (tests): files only_<prog hex> / skip_<prog
 * hex> there, looked for at every frame, limit blending to those vertex
 * programs or leave them out.
 */
#include "nv2a_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
int nv2a_pb_exec_draw_words(uint32_t method, const uint32_t *p, uint32_t n);
void nv2a_pb_merge_flush(void);

#define INTERP_MAX_N      4             /* two frames' pictures in 8 slots */
#define INTERP_MAX_DRAWS  40000u        /* a frame with more is shown as is */
#define INTERP_MAX_WORDS  (64u << 20)   /* ditto, stream words (256 MB) */
#define INTERP_MIN_PAIRED 0.5           /* fewer draws paired: a cut */

/* Stream entries: a tag in the top 3 bits of the first word. */
#define TAG_METHOD 1u                   /* subch << 16 | method, param */
#define TAG_WORDS  2u                   /* method, n, n words */
#define TAG_FLUSH  3u                   /* the walk's merge flush */

typedef float VshConsts[NV2A_VSH_CONSTANTS][4];

typedef struct {
    uint32_t *words;
    size_t    nwords, capwords;
    uint64_t *key;
    uint64_t (*loose)[2];               /* keys for draws left over: without
                                         * index count and texture; and
                                         * program and target only */
    uint32_t (*part)[6];                /* for traces: prog, attr0, n, prim,
                                         * tex0, target */
    uint32_t (*ix)[2];                  /* each draw's index list: start, count */
    VertexAttr (*attr)[NV_VERTEX_ATTRS]; /* and its vertex arrays */
    uint8_t  *inl;                      /* drawn from INLINE_ARRAY / immediate */
    uint16_t *prog;                     /* s_prog entry, or PROG_NONE */
    uint64_t *vh;                       /* RECOMP_INTERP_VCHECK: vertex bytes */
    float   (*spos)[3];                 /* first vertex on the target: x, y
                                         * (0..1 of the clip), valid (0/1) */
    uint32_t *idx;                      /* in here */
    size_t    nidx, capidx;
    VshConsts *consts;
    int32_t  *pair;                     /* N's draw j -> N-1's, or -1 */
    uint32_t  ndraws, capdraws;
    int       valid;                    /* recorded from its start */
    int       overflow;
    Nv2aPbState *start_pb;
    void     *start_vsh;
} Frame;

static int s_n = -1;
static int s_log = -1;
static int s_mode;                      /* RECOMP_INTERP_MODE */
static int s_unique;                    /* RECOMP_INTERP_UNIQUE (test) */
static int s_strict;                    /* RECOMP_INTERP_STRICT: no loose keys */
static int s_underlay;                  /* RECOMP_INTERP_UNDER=1: replays' clears
                                         * start from the frame before's
                                         * picture. Off: where a title draws
                                         * no background (Conker's night
                                         * sky is the clear colour), every
                                         * in-between showed the frame
                                         * before there, its lamp halos too,
                                         * and the glow picked them up */
static int s_carry = 1;                 /* RECOMP_INTERP_CARRY=0: no
                                         * NV_INTERP_KEEP / _PUT */
static int s_near = 1;                  /* RECOMP_INTERP_NEAR=0: always frame N */
static int s_union = 1;                 /* RECOMP_INTERP_UNION=0: no index unions */
static double s_autocut = 0.5;          /* RECOMP_INTERP_AUTOCUT=0: no cut test */
static uint8_t s_all_rows[NV2A_VSH_CONSTANTS];
static int s_teleport = 1;              /* RECOMP_INTERP_JUMPS=0: blend them too */
static int s_both;                      /* RECOMP_INTERP_BOTH: 1 (default) middle
                                         * from both while replays leave room
                                         * (delay < 14 ms), 2 all, 3 middle
                                         * always, 0 off */
static int s_texwrap = 1;               /* RECOMP_INTERP_TEXWRAP=0: texture
                                         * matrices blended straight */
static int s_nearest = 1;               /* RECOMP_INTERP_NEAREST=0: by order */
static int s_jumplog;                   /* RECOMP_INTERP_JUMPLOG=1 (test) */
static int s_hold_on = 1;               /* RECOMP_INTERP_HOLD=0: ignored */
static int s_rot = 1;                   /* RECOMP_INTERP_ROT=0: plain lerp */
static float s_spos_max = 0.3f;         /* RECOMP_INTERP_SPOS_MAX (0 = off) */
static uint64_t s_spos_refused;
#define SPOS_MAX_DRAWS 6000u            /* per frame: the rest go without */
static uint32_t s_rot_fixed;            /* matrices rescaled (10 s) */
static float s_pair_max = 1.0f;         /* RECOMP_INTERP_PAIR_MAX: strict
                                         * pairs less alike are refused */
static uint64_t s_pair_refused;
static float s_loose_max = 1.0f;        /* RECOMP_INTERP_LOOSE_MAX: loose pairs
                                         * less alike are refused (draw_dist) */
static int s_vcheck;                    /* RECOMP_INTERP_VCHECK=1 (test) */
static uint32_t s_vdiff, s_vsaid;       /* replayed draws whose vertices
                                         * changed since recorded (10 s) */
ptrdiff_t xbox_GetMemoryOffset(void);   /* xbox_memory_layout.h */

/* RECOMP_INTERP_VCHECK: a hash of the vertex bytes the current draw reads
 * from guest memory (every array, the elements its indices reach); 0 for
 * vertices in the pushbuffer (recorded with the stream). */
static uint64_t vtx_hash(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t h = 1469598103934665603ull;
    uint32_t mn = ~0u, mx = 0, i, a;

    if (g_pb.inline_active || g_pb.imm_count || g_pb.inline_count
        || !g_pb.idx_count)
        return 0;
    for (i = 0; i < g_pb.idx_count; i++) {
        if (g_pb.idx[i] < mn) mn = g_pb.idx[i];
        if (g_pb.idx[i] > mx) mx = g_pb.idx[i];
    }
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *v = &g_pb.attr[a];
        const uint8_t *p, *e;
        if (!v->size || !v->stride || !v->offset)
            continue;
        p = mem + v->offset + (size_t)mn * v->stride;
        e = mem + v->offset + (size_t)mx * v->stride
            + (v->stride < 16 ? v->stride : 16);
        h ^= a * 0x9E3779B97F4A7C15ull;
        for (; p + 8 <= e; p += 8) {
            uint64_t w;
            memcpy(&w, p, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        for (; p < e; p++)
            h = (h ^ *p) * 0x100000001B3ull;
    }
    return h | 1;
}

/* What a vertex program does with its constants, worked out once per
 * program from its instructions (prog_info): which registers are texture
 * matrices -- read by an instruction whose other input comes from a texture
 * coordinate (v9-v12) and whose result reaches oT0-oT3. A title scrolls a
 * texture by moving such a matrix's translation and wraps it back by a whole
 * repeat now and then (Conker: u, v jump by 2 past +-2, 0x003F91D0); the
 * texture looks the same either side, but a straight blend crosses half the
 * texture for a picture. */
#define PROG_MAX 1024u
typedef struct {
    uint64_t hash;
    uint64_t texmat[3];                 /* bit r: c[r] (192 bits) */
    uint64_t reads[3];                  /* registers it reads (relative:
                                         * from the base to the end) */
    int used;
} ProgInfo;
static ProgInfo s_prog[PROG_MAX];
static uint32_t s_nprog;
#define PROG_NONE 0xFFFFu
static Frame s_fr[2];
static int s_cur;                       /* s_fr[s_cur] is being recorded */
static int s_recording;
static int s_rep;                       /* replaying */
static Frame *s_rep_f, *s_rep_prev;     /* run, and its partner frame */
static const int32_t *s_rep_map;        /* s_rep_f's draw -> partner's, or -1 */
static const int32_t *s_rep_donor;      /* unpaired draw -> paired one it
                                         * borrows the camera from, or -1 */
static int32_t *s_donor_f, *s_donor_p;  /* for N's draws, N-1's */
static uint32_t s_donor_fcap, s_donor_pcap;
static int s_borrow = 1;                /* RECOMP_INTERP_BORROW=0: off */
static int s_donors_ok, s_donors_p_ok;  /* this frame's donor lists made */
static uint32_t s_borrowed;             /* draws that borrowed (10 s) */
static VshConsts s_tmp2;
static int32_t *s_inv;                  /* N-1's draws -> N's (replaying N-1) */
static uint32_t s_inv_cap;
static uint32_t s_rep_j;
static float s_rep_t;
static int s_rep_blend;                 /* this frame blends (not a cut) */
static VshConsts s_tmp;
#define CTL_MAX 32
static uint32_t s_only[CTL_MAX], s_skip[CTL_MAX];
static int s_nonly, s_nskip;
static int s_dump_progs;                /* the next frame's programs, once each */
static int s_cut;                       /* this frame is a camera cut */
static Nv2aPbState *s_end_pb;
static void *s_end_vsh;
static size_t s_vsh_size;

/* Pictures waiting to be shown: slot and when (QPC); two frames' worth (the
 * next frame's replays run while the last pictures of this one are due). */
static int s_q_slot[2 * INTERP_MAX_N];
static LONGLONG s_q_at[2 * INTERP_MAX_N];
static int s_base;                      /* this frame's picture slots: 0 or 4 */
static int s_q_shown_base = -1;         /* the slots just queued (flip_after) */
static int s_q_old;                     /* queue entries below: the frame before's */
static double s_lat_ms = -1.0;          /* flip -> first picture, ms (EMA) */
void xbox_Nv2aIdle(DWORD ms);           /* xbox_memory_layout.c */
static void show_due(int all);
static int s_q_n, s_q_next;
static volatile LONG s_q_pending;
static const Nv2aPbBackend *s_be;
static LARGE_INTEGER s_freq;
static LONGLONG s_last_flip, s_period;  /* QPC; s_period: EMA of flip gaps */

static struct {
    uint32_t frames, interpolated, cuts, game_cuts, auto_cuts, overflows;
    uint32_t unpaired_frames;
    uint64_t draws, paired, diverged, dup_draws, loose1, loose2, unions;
    uint64_t teleports, both;
    double replay_ms, replay_max;
    uint32_t shown, late, dropped;
} s_st;
static LONGLONG s_st_at;
/* Gaps between pictures shown, ms: < 5, 5-7, 7-9.5, 9.5-12, 12-17, > 17
 * (at 4 a frame each should be 8.3). */
static uint32_t s_gap[6];
static LONGLONG s_gap_last;

int nv2a_interp_n(void)
{
    if (s_n < 0) {
        const char *e = getenv("RECOMP_FRAME_INTERP");
        int n = e ? atoi(e) : 1;
        if (n < 1)
            n = 1;
        if (n > INTERP_MAX_N)
            n = INTERP_MAX_N;
        s_log = getenv("RECOMP_INTERP_LOG") ? atoi(getenv("RECOMP_INTERP_LOG"))
                                            : 0;
        e = getenv("RECOMP_INTERP_MODE");
        s_mode = e ? atoi(e) : 0;
        e = getenv("RECOMP_INTERP_UNIQUE");
        s_unique = e && *e && *e != '0';
        e = getenv("RECOMP_INTERP_STRICT");
        s_strict = e && *e && *e != '0';
        e = getenv("RECOMP_INTERP_TEXWRAP");
        s_texwrap = !(e && *e == '0');
        s_jumplog = getenv("RECOMP_INTERP_JUMPLOG") != NULL;
        e = getenv("RECOMP_INTERP_PAIR_MAX");
        if (e)
            s_pair_max = (float)atof(e);
        e = getenv("RECOMP_INTERP_LOOSE_MAX");
        if (e)
            s_loose_max = (float)atof(e);
        e = getenv("RECOMP_INTERP_SPOS_MAX");
        if (e)
            s_spos_max = (float)atof(e);
        e = getenv("RECOMP_INTERP_BORROW");
        s_borrow = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_HOLD");
        s_hold_on = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_ROT");
        s_rot = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_NEAREST");
        s_nearest = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_VCHECK");
        s_vcheck = e && *e && *e != '0';
        e = getenv("RECOMP_INTERP_UNDER");
        s_underlay = e && *e && *e != '0';
        e = getenv("RECOMP_INTERP_CARRY");
        s_carry = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_NEAR");
        s_near = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_UNION");
        s_union = !(e && *e == '0');
        e = getenv("RECOMP_INTERP_AUTOCUT");
        if (e)
            s_autocut = atof(e);
        e = getenv("RECOMP_INTERP_JUMPS");
        s_teleport = e ? atoi(e) : 1;
        e = getenv("RECOMP_INTERP_BOTH");
        s_both = e ? atoi(e) : 1;
        memset(s_all_rows, 1, sizeof s_all_rows);
        QueryPerformanceFrequency(&s_freq);
        s_period = s_freq.QuadPart / 30;
        if (n > 1)
            fprintf(stderr, "[INTERP] %d pictures per title frame%c", n, 10);
        s_n = n;
    }
    return s_n;
}

int nv2a_interp_replaying(void)
{
    return s_rep;
}

static unsigned s_rep_id;

unsigned nv2a_interp_replay_id(void)
{
    return s_rep_id;
}

static int s_hold;                      /* NV_HOST_FX_INTERP_HOLD_ON .. _OFF */
static uint32_t s_held;                 /* draws drawn as their frame (10 s) */

void nv2a_interp_hold(int on)
{
    s_hold = on && s_hold_on;
}

void nv2a_interp_note_cut(void)
{
    if (!s_rep)
        s_cut = 1;
}

static int grow_words(Frame *f, size_t more)
{
    if (f->overflow)
        return 0;
    if (f->nwords + more > f->capwords) {
        size_t cap = f->capwords ? f->capwords * 2 : (1u << 20);
        uint32_t *w;
        while (cap < f->nwords + more)
            cap *= 2;
        if (cap > INTERP_MAX_WORDS || !(w = realloc(f->words, cap * 4))) {
            f->overflow = 1;
            return 0;
        }
        f->words = w;
        f->capwords = cap;
    }
    return 1;
}

void nv2a_interp_rec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    Frame *f = &s_fr[s_cur];

    if (!s_recording || !grow_words(f, 2))
        return;
    f->words[f->nwords++] = TAG_METHOD << 29 | (subch & 7u) << 16
                          | (method & 0xFFFFu);
    f->words[f->nwords++] = param;
}

void nv2a_interp_rec_words(uint32_t method, const uint32_t *p, uint32_t n)
{
    Frame *f = &s_fr[s_cur];

    if (!s_recording || !grow_words(f, 2 + (size_t)n))
        return;
    f->words[f->nwords++] = TAG_WORDS << 29 | (method & 0xFFFFu);
    f->words[f->nwords++] = n;
    memcpy(f->words + f->nwords, p, (size_t)n * 4);
    f->nwords += n;
}

void nv2a_interp_rec_flush(void)
{
    Frame *f = &s_fr[s_cur];

    if (!s_recording || !grow_words(f, 1))
        return;
    f->words[f->nwords++] = TAG_FLUSH << 29;
}

static uint64_t fnv(uint64_t h, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

/* What names a draw from one frame to the next; loose (may be NULL): the
 * looser keys draws left over are paired by (Frame.loose). */
static uint64_t draw_key_loose(uint64_t loose[2])
{
    uint64_t h = 1469598103934665603ull, prog = 0, t;
    uint32_t count;

    if ((g_pb.xf_mode & 3) == 2)
        nv2a_vsh_decoded(&count, &prog);
    h = fnv(h, prog);
    /* The target by its shape, not its address: a double-buffered frame
     * draws into the other buffer each time. */
    h = fnv(h, (uint64_t)g_pb.pitch << 32 | g_pb.format);
    h = fnv(h, (uint64_t)g_pb.clip_w << 32 | g_pb.clip_h);
    /* A small target by its address too: Conker's menu room and
     * characters draw many 32x32 lighting maps of one shape, a few each
     * frame in turn, and by shape alone map X paired with map Y (their
     * matrices "jumped", drawn unblended among blended ones). Full-screen
     * ones by shape only: the back buffers take turns. */
    if (g_pb.clip_w < 320 || g_pb.clip_h < 240)
        h = fnv(h, g_pb.color_offset);
    t = h;
    h = fnv(h, g_pb.attr[0].offset);
    if (loose) {
        loose[0] = h;
        loose[1] = t;
    }
    h = fnv(h, (uint64_t)g_pb.idx_count << 32 | g_pb.prim);
    h = fnv(h, g_pb.texs[0].offset);
    return h;
}

static uint64_t draw_key(void)
{
    return draw_key_loose(NULL);
}

static int all_zero(const float *v)
{
    return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f;
}

/* Texture matrix registers of program ins (count instructions), into
 * texmat: a register an instruction reads together with a texture
 * coordinate input (v9-v12 read directly), when that instruction's result
 * reaches oT0-oT3 (o[9]-o[12]), directly or through temps, and the register
 * plays no part in the position (oPos). Not through temps on the input side:
 * Conker's skinned programs feed v9 into the position and then project the
 * position into a texture coordinate with c104-c107, whose translation is
 * the camera's (a "wrap" there moved the picture). Temps are followed whole
 * (a partial write adds to what the temp carried). */
static void texmat_regs(const Nv2aVshIns *ins, uint32_t count,
                        uint64_t texmat[3], uint64_t reads[3])
{
    uint64_t via[16][3];                /* texture matrix candidates in it */
    uint64_t all[16][3];                /* every constant that went into it */
    uint64_t pos[3] = {0, 0, 0};
    uint32_t i, s, w;

    memset(via, 0, sizeof via);
    memset(all, 0, sizeof all);
    texmat[0] = texmat[1] = texmat[2] = 0;
    reads[0] = reads[1] = reads[2] = 0;
    for (i = 0; i < count; i++) {
        const Nv2aVshIns *I = &ins[i];
        uint64_t c[3] = {0, 0, 0}, a[3] = {0, 0, 0}, cc[3] = {0, 0, 0};
        int texin = 0;
        for (s = 0; s < 3; s++) {
            const Nv2aVshSrc *S = &I->src[s];
            if (!(I->need & (1u << s)))
                continue;
            if (S->mux == 3 && I->cidx < NV2A_VSH_CONSTANTS) {
                uint32_t r = I->cidx, e = I->rel ? NV2A_VSH_CONSTANTS : r + 1;
                for (; r < e; r++)
                    reads[r >> 6] |= 1ull << (r & 63);
            }
            if (S->mux == 2) {
                if (I->iidx >= 9 && I->iidx <= 12)
                    texin = 1;
            } else if (S->mux == 1) {
                if (S->reg < 16)
                    for (w = 0; w < 3; w++) {
                        c[w] |= via[S->reg][w];
                        a[w] |= all[S->reg][w];
                    }
            } else if (S->mux == 3 && !I->rel
                       && I->cidx < NV2A_VSH_CONSTANTS) {
                /* Relative reads (a bone palette) are left out: never a
                 * texture matrix, and their range can't be named. */
                cc[I->cidx >> 6] |= 1ull << (I->cidx & 63);
            }
        }
        for (w = 0; w < 3; w++) {
            a[w] |= cc[w];
            if (texin)
                c[w] |= cc[w];
        }
        /* The temp written: the MAC's, or R1 for the ILU beside a MAC. */
        if (I->mac && I->mac_mask && I->tdst < 16) {
            if (I->mac_mask == 0xF) {
                memset(via[I->tdst], 0, sizeof via[I->tdst]);
                memset(all[I->tdst], 0, sizeof all[I->tdst]);
            }
            for (w = 0; w < 3; w++) {
                via[I->tdst][w] |= c[w];
                all[I->tdst][w] |= a[w];
            }
            if (I->tdst == 12)          /* R12 is oPos */
                for (w = 0; w < 3; w++)
                    pos[w] |= a[w];
        }
        if (I->ilu && I->ilu_mask) {
            uint32_t d = I->mac ? 1 : I->tdst;
            if (d < 16)
                for (w = 0; w < 3; w++) {
                    via[d][w] |= c[w];
                    all[d][w] |= a[w];
                }
        }
        if (I->out_o && I->omask) {
            if (I->oaddr >= 9 && I->oaddr <= 12)
                for (w = 0; w < 3; w++)
                    texmat[w] |= c[w];
            else if (I->oaddr == 0)
                for (w = 0; w < 3; w++)
                    pos[w] |= a[w];
        }
    }
    for (w = 0; w < 3; w++)
        texmat[w] &= ~pos[w];
}

/* The s_prog entry of the current program (made on first sight), or
 * PROG_NONE. */
static uint16_t prog_info(void)
{
    const Nv2aVshIns *ins;
    uint32_t count, i, h;
    uint64_t hash = 0;

    if ((g_pb.xf_mode & 3) != 2 || !(ins = nv2a_vsh_decoded(&count, &hash)))
        return PROG_NONE;
    /* Open addressing on the hash; the table never empties. */
    h = (uint32_t)(hash ^ hash >> 29) & (PROG_MAX - 1);
    for (i = 0; i < PROG_MAX; i++, h = (h + 1) & (PROG_MAX - 1)) {
        if (!s_prog[h].used)
            break;
        if (s_prog[h].hash == hash)
            return (uint16_t)h;
    }
    if (i == PROG_MAX || s_nprog >= PROG_MAX * 3 / 4)
        return PROG_NONE;
    s_prog[h].hash = hash;
    s_prog[h].used = 1;
    texmat_regs(ins, count, s_prog[h].texmat, s_prog[h].reads);
    s_nprog++;
    if (s_log && (s_prog[h].texmat[0] | s_prog[h].texmat[1]
                  | s_prog[h].texmat[2])) {
        uint32_t r;
        fprintf(stderr, "[ITEXMAT] program %08X: texture matrices", (uint32_t)hash);
        for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
            if (s_prog[h].texmat[r >> 6] >> (r & 63) & 1)
                fprintf(stderr, " c%u", r);
        fprintf(stderr, "%c", 10);
    }
    return (uint16_t)h;
}

static uint32_t s_wraps, s_wrap_snaps;  /* this 10 s: wraps blended short */

/* Length of x, y, z of a row. */
static float len3(const float *v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* Whether rows m[0..2] look like a rotation, maybe scaled: x, y, z of each
 * longer than 1e-3, lengths within 1.5x of each other, near orthogonal. */
static int rot_rows(const float (*m)[4], float l[3])
{
    int i, j;
    for (i = 0; i < 3; i++)
        if (!((l[i] = len3(m[i])) > 1e-3f) || !isfinite(l[i]))
            return 0;
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++) {
            float a = l[i] > l[j] ? l[i] : l[j], b = l[i] > l[j] ? l[j] : l[i];
            float d = m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2];
            if (a > 1.5f * b || fabsf(d) > 0.1f * l[i] * l[j])
                return 0;
        }
    return 1;
}

/* After a register-by-register blend: a matrix that turned shrinks (by
 * cos(angle/2): 30 % at 90 degrees), so a fast swinging limb or head of a
 * character collapsed for a picture and looked gone. Each row of a
 * rotation (three rows near orthogonal, of like lengths, in both frames)
 * is scaled back to its blended length; its direction stays the blend's
 * (halfway turned). The w (translation) is left alone. */
static void rot_fix(VshConsts out, const VshConsts own, const VshConsts partner,
                    float w, const uint64_t *reads)
{
    uint32_t r, i;
    float la[3], lb[3];

    for (r = 0; r + 2 < NV2A_VSH_CONSTANTS; r++) {
        if (reads && !(reads[r >> 6] >> (r & 63) & 1))
            continue;
        if (!memcmp(own[r], partner[r], 48))
            continue;                   /* all three the same: nothing */
        if (!rot_rows((const float (*)[4])own[r], la)
            || !rot_rows((const float (*)[4])partner[r], lb))
            continue;
        for (i = 0; i < 3; i++) {
            float *o = out[r + i], lo = len3(o), want;
            if (!memcmp(o, own[r + i], 16) || !(lo > 1e-6f))
                continue;               /* kept as it was */
            want = la[i] + (lb[i] - la[i]) * w;
            o[0] *= want / lo;
            o[1] *= want / lo;
            o[2] *= want / lo;
        }
        s_rot_fixed++;
        r += 2;                         /* past this matrix */
    }
}

/* out = own moved w of the way toward partner, register by register; a
 * register zero in either is own's. texmat (NULL: none): the program's
 * texture matrix registers, whose values that moved by about a whole number
 * (a scroll wrapped back by whole repeats) move the short way instead, and
 * that moved by more than half a repeat otherwise are the nearer frame's. */
static void blend(VshConsts out, const VshConsts own, const VshConsts partner,
                  float w, const uint64_t *texmat, const uint8_t *keep,
                  const uint64_t *reads)
{
    uint32_t r, c;

    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        /* A register the program never reads: as it is (only speed). */
        if ((reads && !(reads[r >> 6] >> (r & 63) & 1))
            || !memcmp(own[r], partner[r], sizeof own[r]) || all_zero(own[r])
            || all_zero(partner[r]) || (keep && keep[r])) {
            memcpy(out[r], own[r], sizeof out[r]);
            continue;
        }
        if (texmat && (texmat[r >> 6] >> (r & 63) & 1)) {
            for (c = 0; c < 4; c++) {
                float d = partner[r][c] - own[r][c], v;
                /* Conker's scrolls stay within +-2 (and wrap there). */
                if (fabsf(d) >= 0.5f && fabsf(own[r][c]) <= 4.0f
                    && fabsf(partner[r][c]) <= 4.0f) {
                    float k = d - rintf(d);
                    if (fabsf(k) <= 0.25f) {
                        d = k;          /* wrapped: the short way */
                        s_wraps++;
                    } else {            /* a real jump: the nearer frame */
                        out[r][c] = w < 0.5f ? own[r][c] : partner[r][c];
                        s_wrap_snaps++;
                        continue;
                    }
                }
                v = own[r][c] + d * w;
                out[r][c] = isfinite(v) ? v : own[r][c];
            }
            continue;
        }
        for (c = 0; c < 4; c++) {
            float v = own[r][c] + (partner[r][c] - own[r][c]) * w;
            out[r][c] = isfinite(v) ? v : own[r][c];
        }
    }
    if (s_rot)
        rot_fix(out, own, partner, w, reads);
}

static int mats_jumped(const float (*a)[4], const float (*b)[4]);
static int mats_jumped_rows(const float (*a)[4], const float (*b)[4],
                            uint8_t *keep);

static int32_t s_extra = -1;            /* partner draw whose indices go next */
static uint32_t s_extra_idx[NV_MAX_INDICES];
static VertexAttr s_extra_attr[NV_VERTEX_ATTRS];

/* After the back end drew a replayed draw: the partner frame's index list
 * for the same vertices, if the draw armed it (nv2a_interp_draw). */
void nv2a_interp_after_draw(const Nv2aPbBackend *be)
{
    const Frame *p = s_rep_prev;
    int32_t pj = s_extra;
    uint32_t n, keep;

    if (!s_rep || pj < 0)
        return;
    s_extra = -1;
    n = p->ix[pj][1];
    keep = g_pb.idx_count;
    memcpy(s_extra_idx, g_pb.idx, (size_t)keep * 4);
    memcpy(s_extra_attr, g_pb.attr, sizeof g_pb.attr);
    memcpy(g_pb.idx, p->idx + p->ix[pj][0], (size_t)n * 4);
    memcpy(g_pb.attr, p->attr[pj], sizeof g_pb.attr);
    g_pb.idx_count = n;
    pb_transform_forget();
    be->draw();
    pb_transform_forget();
    memcpy(g_pb.idx, s_extra_idx, (size_t)keep * 4);
    memcpy(g_pb.attr, s_extra_attr, sizeof g_pb.attr);
    g_pb.idx_count = keep;
    s_st.unions++;
}

/* RECOMP_INTERP_CTL_DIR: whether draws of program prog are blended. */
static int ctl_blends(uint32_t prog)
{
    int i;

    for (i = 0; i < s_nskip; i++)
        if (s_skip[i] == prog)
            return 0;
    if (!s_nonly)
        return 1;
    for (i = 0; i < s_nonly; i++)
        if (s_only[i] == prog)
            return 1;
    return 0;
}

static void ctl_read(void)
{
    static const char *dir = (const char *)-1;
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (dir == (const char *)-1)
        dir = getenv("RECOMP_INTERP_CTL_DIR");
    if (!dir)
        return;
    s_nonly = s_nskip = 0;
    snprintf(pat, sizeof pat, "%s/*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        uint32_t v;
        if (sscanf(fd.cFileName, "only_%x", &v) == 1 && s_nonly < CTL_MAX)
            s_only[s_nonly++] = v;
        else if (sscanf(fd.cFileName, "skip_%x", &v) == 1 && s_nskip < CTL_MAX)
            s_skip[s_nskip++] = v;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    {
        /* Say so when the set changes (the glitch scan reads it). */
        static uint64_t said;
        uint64_t sum = (uint64_t)s_nonly << 56 | (uint64_t)s_nskip << 48;
        int i;
        for (i = 0; i < s_nonly; i++)
            sum += s_only[i] * 0x9E3779B1ull;
        for (i = 0; i < s_nskip; i++)
            sum += s_skip[i] * 0x85EBCA77ull;
        if (sum != said) {
            said = sum;
            fprintf(stderr, "[ICTL] only");
            for (i = 0; i < s_nonly; i++)
                fprintf(stderr, " %08X", s_only[i]);
            fprintf(stderr, " skip");
            for (i = 0; i < s_nskip; i++)
                fprintf(stderr, " %08X", s_skip[i]);
            fprintf(stderr, "%c", 10);
        }
    }
}

/* Where the draw's first vertex lands on its target (the program run on
 * it once, as the title's constants are now), as x, y in 0..1 of the
 * clip; out[2] = 1 when known (in front of the camera, finite). Two
 * draws of one mesh pair by this: Conker's bar room has several of one
 * bottle, and as the camera turns one leaves at one side while another
 * comes in at the other -- paired by their constants, one flew across the
 * picture. Not for programs that write constants. */
static void spos_note(float out[3], uint32_t j)
{
    float in[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    uint32_t a;

    out[0] = out[1] = out[2] = 0.0f;
    if (s_spos_max <= 0.0f || j >= SPOS_MAX_DRAWS || (g_pb.xf_mode & 3) != 2
        || nv2a_vsh_cxt_write() || !g_pb.idx_count || !g_pb.clip_w
        || !g_pb.clip_h || g_pb.merge_nparts)
        return;
    for (a = 0; a < NV2A_VSH_INPUTS; a++) {
        if (a < NV_VERTEX_ATTRS)
            pb_fetch_attr(&g_pb.attr[a], g_pb.idx[0], in[a]);
        else
            in[a][0] = in[a][1] = in[a][2] = 0.0f, in[a][3] = 1.0f;
    }
    if (!nv2a_vsh_run((const float (*)[4])in, &o) || !isfinite(o.pos[0])
        || !isfinite(o.pos[1]) || !(o.pos[3] > 0.0f))
        return;
    out[0] = (o.pos[0] - (float)g_pb.clip_x) / (float)g_pb.clip_w;
    out[1] = (o.pos[1] - (float)g_pb.clip_y) / (float)g_pb.clip_h;
    out[2] = isfinite(out[0]) && isfinite(out[1]) ? 1.0f : 0.0f;
}

/* How far draw ia of f and ib of prev moved on their target (0..1 units of
 * the clip), or -1 when not known for both. */
static float spos_dist(const Frame *f, const Frame *prev, uint32_t ia,
                       uint32_t ib)
{
    float dx, dy;
    if (!f->spos || !prev->spos || !f->spos[ia][2] || !prev->spos[ib][2])
        return -1.0f;
    /* Only near the picture: a big mesh's first vertex beside or behind the
     * camera swings far for a small camera move. */
    if (f->spos[ia][0] < -0.25f || f->spos[ia][0] > 1.25f
        || f->spos[ia][1] < -0.25f || f->spos[ia][1] > 1.25f
        || prev->spos[ib][0] < -0.25f || prev->spos[ib][0] > 1.25f
        || prev->spos[ib][1] < -0.25f || prev->spos[ib][1] > 1.25f)
        return -1.0f;
    dx = f->spos[ia][0] - prev->spos[ib][0];
    dy = f->spos[ia][1] - prev->spos[ib][1];
    return sqrtf(dx * dx + dy * dy);
}

void nv2a_interp_draw(void)
{
    if (s_rep) {
        Frame *f = s_rep_f;
        uint32_t j = s_rep_j++;
        if (j >= f->ndraws || draw_key() != f->key[j]) {
            /* The run went another way than the recording (it should not):
             * the rest of the frame draws as it comes. */
            if (j <= f->ndraws)
                s_st.diverged++;
            s_rep_j = f->ndraws + 1;
            return;
        }
        if (s_vcheck && f->vh && f->vh[j]) {
            uint64_t h = vtx_hash();
            if (h != f->vh[j]) {
                s_vdiff++;
                if (s_vsaid++ < 40)
                    fprintf(stderr, "[IVDIFF] flip %u draw %u prog %08X attr0"
                            " %08X stride %u n %u: vertices changed since"
                            " recorded%c", g_pb.flips, j, f->part[j][0],
                            g_pb.attr[0].offset, g_pb.attr[0].stride,
                            g_pb.idx_count, 10);
            }
        }
        s_extra = -1;
        if (s_hold)
            s_held++;
        if (s_rep_blend && s_rep_map[j] >= 0 && ctl_blends(f->part[j][0])
            && !s_hold
            && !(s_teleport == 2
                 && mats_jumped((const float (*)[4])f->consts[j],
                                (const float (*)[4])
                                    s_rep_prev->consts[s_rep_map[j]])
                 && ++s_st.teleports)) {
            int32_t pj = s_rep_map[j];
            /* A matrix that jumped (turned > 30 degrees, moved far) keeps
             * the drawn frame's value; the rest still blends. Conker's
             * characters carry a light's matrix (c159-c161) that changes
             * light every frame: drawing the whole character unblended slid
             * it against the blended room. RECOMP_INTERP_JUMPS=2: the whole
             * draw as its frame (as before), 0: blend everything. */
            static uint8_t keep[NV2A_VSH_CONSTANTS];
            int jumped = s_teleport == 1
                && mats_jumped_rows((const float (*)[4])f->consts[j],
                                    (const float (*)[4])s_rep_prev->consts[pj],
                                    keep);
            if (jumped)
                s_st.teleports++;
            blend(s_tmp, (const float (*)[4])f->consts[j],
                  (const float (*)[4])s_rep_prev->consts[pj],
                  s_rep_t, f->prog[j] != PROG_NONE
                           ? s_prog[f->prog[j]].texmat : NULL,
                  jumped ? keep : NULL,
                  f->prog[j] != PROG_NONE ? s_prog[f->prog[j]].reads : NULL);
            nv2a_vsh_constants_set_all((const float (*)[4])s_tmp);
            /* The same vertices drawn with another index list in the
             * partner frame (a ground's pieces, each frame only what that
             * frame sees): its triangles too, opaque draws only. */
            if (s_union && !g_pb.blend_enable && !g_pb.merge_nparts
                && !f->inl[j] && !s_rep_prev->inl[pj]
                && f->part[j][0] == s_rep_prev->part[pj][0]
                && f->part[j][1] && f->part[j][1] == s_rep_prev->part[pj][1]
                && (f->ix[j][1] != s_rep_prev->ix[pj][1]
                    || memcmp(f->idx + f->ix[j][0],
                              s_rep_prev->idx + s_rep_prev->ix[pj][0],
                              (size_t)f->ix[j][1] * 4))
                && s_rep_prev->ix[pj][1]
                && s_rep_prev->ix[pj][1] <= NV_MAX_INDICES)
                s_extra = pj;
        } else if (s_rep_blend && s_rep_map[j] < 0 && s_rep_donor
                   && s_rep_donor[j] >= 0 && !s_hold
                   && ctl_blends(f->part[j][0])) {
            /* No partner (culled in the other frame, new, gone): it moves
             * with the camera at least -- the registers it shares bit for
             * bit with a paired draw of its program take that draw's
             * blend, the rest stay its own. Without this, the pieces the
             * newer frame culled at the edge of a turn stayed where the
             * older frame saw them, and the sky showed beside them. */
            int32_t d = s_rep_donor[j], pd = s_rep_map[d];
            uint32_t r;
            blend(s_tmp2, (const float (*)[4])f->consts[d],
                  (const float (*)[4])s_rep_prev->consts[pd], s_rep_t,
                  NULL, NULL, NULL);
            for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
                memcpy(s_tmp[r], !memcmp(f->consts[j][r], f->consts[d][r], 16)
                                 ? s_tmp2[r] : f->consts[j][r], 16);
            nv2a_vsh_constants_set_all((const float (*)[4])s_tmp);
            s_borrowed++;
        } else {
            nv2a_vsh_constants_set_all((const float (*)[4])f->consts[j]);
        }
        return;
    }
    if (s_recording) {
        Frame *f = &s_fr[s_cur];
        if (f->overflow)
            return;
        if (f->ndraws == f->capdraws) {
            uint32_t cap = f->capdraws ? f->capdraws * 2 : 1024;
            uint64_t *k;
            VshConsts *c;
            int32_t *p;
            if (cap > INTERP_MAX_DRAWS) {
                f->overflow = 1;
                return;
            }
            k = realloc(f->key, cap * sizeof *k);
            if (k)
                f->key = k;
            c = realloc(f->consts, cap * sizeof *c);
            if (c)
                f->consts = c;
            p = realloc(f->pair, cap * sizeof *p);
            if (p)
                f->pair = p;
            {
                uint32_t (*pt)[6] = realloc(f->part, cap * sizeof *pt);
                uint64_t (*lk)[2] = realloc(f->loose, cap * sizeof *lk);
                uint32_t (*ix)[2] = realloc(f->ix, cap * sizeof *ix);
                VertexAttr (*at)[NV_VERTEX_ATTRS] = realloc(f->attr,
                                                            cap * sizeof *at);
                uint8_t *il = realloc(f->inl, cap);
                uint16_t *pg = realloc(f->prog, cap * sizeof *pg);
                uint64_t *vh = realloc(f->vh, cap * sizeof *vh);
                float (*sp)[3] = realloc(f->spos, cap * sizeof *sp);
                if (sp)
                    f->spos = sp;
                if (pg)
                    f->prog = pg;
                if (vh)
                    f->vh = vh;
                if (pt)
                    f->part = pt;
                if (lk)
                    f->loose = lk;
                if (ix)
                    f->ix = ix;
                if (at)
                    f->attr = at;
                if (il)
                    f->inl = il;
                if (!pt || !lk || !ix || !at || !il || !pg || !vh || !sp)
                    p = NULL;
            }
            if (!k || !c || !p) {
                f->overflow = 1;
                return;
            }
            f->capdraws = cap;
        }
        f->key[f->ndraws] = draw_key_loose(f->loose[f->ndraws]);
        f->prog[f->ndraws] = s_texwrap ? prog_info() : PROG_NONE;
        f->vh[f->ndraws] = s_vcheck ? vtx_hash() : 0;
        spos_note(f->spos[f->ndraws], f->ndraws);
        {
            /* Its index list and arrays, for the union below
             * (nv2a_interp_after_draw). */
            uint32_t n = g_pb.idx_count;
            memcpy(f->attr[f->ndraws], g_pb.attr, sizeof g_pb.attr);
            f->inl[f->ndraws] = (uint8_t)(g_pb.inline_active || g_pb.imm_count
                                          || g_pb.inline_count);
            if (f->nidx + n > f->capidx) {
                size_t cap = f->capidx ? f->capidx * 2 : (1u << 20);
                uint32_t *x;
                while (cap < f->nidx + n)
                    cap *= 2;
                x = realloc(f->idx, cap * 4);
                if (x) {
                    f->idx = x;
                    f->capidx = cap;
                }
            }
            if (f->nidx + n <= f->capidx) {
                memcpy(f->idx + f->nidx, g_pb.idx, (size_t)n * 4);
                f->ix[f->ndraws][0] = (uint32_t)f->nidx;
                f->ix[f->ndraws][1] = n;
                f->nidx += n;
            } else {
                f->ix[f->ndraws][0] = 0;
                f->ix[f->ndraws][1] = 0;
            }
        }
        {
            uint64_t prog = 0;
            uint32_t count;
            if ((g_pb.xf_mode & 3) == 2)
                nv2a_vsh_decoded(&count, &prog);
            if (s_dump_progs && prog) {
                static uint32_t seen[256];
                static int nseen;
                int i;
                for (i = 0; i < nseen && seen[i] != (uint32_t)prog; i++)
                    ;
                if (i == nseen && nseen < 256) {
                    seen[nseen++] = (uint32_t)prog;
                    fprintf(stderr, "[ITRACE] program %08X:%c", (uint32_t)prog,
                            10);
                    nv2a_vsh_dump_current();
                }
            }
            f->part[f->ndraws][0] = (uint32_t)prog;
            f->part[f->ndraws][1] = g_pb.attr[0].offset;
            f->part[f->ndraws][2] = g_pb.idx_count;
            f->part[f->ndraws][3] = g_pb.prim | nv2a_vsh_cxt_write() << 8
                                  | (g_pb.xf_mode & 3) << 12;
            f->part[f->ndraws][4] = g_pb.texs[0].offset;
            f->part[f->ndraws][5] = g_pb.color_offset;
        }
        memcpy(f->consts[f->ndraws], nv2a_vsh_constants_all(),
               sizeof f->consts[0]);
        f->ndraws++;
    }
}

/* Sort (key, index) pairs: the draws of each key in their order. */
typedef struct { uint64_t key; uint32_t i; } KeyIdx;

static int cmp_keyidx(const void *a, const void *b)
{
    const KeyIdx *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    return x->i < y->i ? -1 : x->i > y->i;
}

static KeyIdx *s_ka, *s_kb;

/* Nearest-constants pairing (RECOMP_INTERP_NEAREST, on): the draws of one
 * key in both frames are paired by how alike their constants are, nearest
 * first, not by their order. One mesh drawn several times (Conker's menu
 * room: one model eight times in a row) comes in another order once the
 * camera turns, and by order copy 3 paired with copy 5: matrices "jumped",
 * drawn unblended among blended ones (doubled heads, a candle left behind).
 * Only the registers that vary within the group count (the per-object ones;
 * the camera's are the same for all), each component as
 * |a - b| / (1 + |a| + |b|). Groups bigger than NEAR_MAX x NEAR_MAX pair by
 * order. */
#define NEAR_MAX 64
static uint8_t *s_taken;                /* below: prev's draws paired */
typedef struct { float d; uint16_t a, b; } NearCand;
static NearCand s_near_c[NEAR_MAX * NEAR_MAX];
static uint64_t s_near_groups, s_near_moved;

/* How unlike two draws' constants are: the sum over the components that
 * differ of |a - b| / (1 + |a| + |b|), over 16 (a whole four-row matrix
 * changed completely counts about 1). Small for one object moved; big for
 * two different objects (other bones, another place). */
static float draw_dist(const float (*a)[4], const float (*b)[4],
                       const uint64_t *reads)
{
    uint32_t r, c;
    float d = 0.0f;

    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        if (reads && !(reads[r >> 6] >> (r & 63) & 1)) {
            if (!(reads[r >> 6] >> (r & 63)))
                r |= 63;                /* none left in this word */
            continue;
        }
        if (!memcmp(a[r], b[r], 16))
            continue;
        for (c = 0; c < 4; c++) {
            float e = fabsf(a[r][c] - b[r][c])
                      / (1.0f + fabsf(a[r][c]) + fabsf(b[r][c]));
            d += isfinite(e) ? e : 1.0f;
        }
    }
    return d / 16.0f;
}

/* s_loose_max: above (switches) */
static uint64_t s_loose_refused;

/* N's draw ia paired with N-1's ib by key k (0 strict, 1-2 loose): 1, or 0
 * when a loose pair is too unlike (two different objects that only share
 * a program: a character coming into view paired with another, drawn
 * halfway between them). */
static uint32_t s_dhist[2][8];          /* strict / loose pairs by distance */

/* The frame's yardsticks for refusing pairs (pair_refuse): each program's
 * median strict-pair distance (-1: none), and the median screen move. In a
 * camera turn every ground piece's constants change a lot (point lights in
 * view space swing round), so a fixed limit refused them all: drawn
 * unblended beside blended neighbours, the ground cracked white. A pair is
 * refused only when it is far less alike than its program's typical one. */
static float s_prog_med[PROG_MAX];
static float s_sd_med;

static float pair_limit(const Frame *f, uint32_t ia, float abs_max)
{
    float m = f->prog[ia] != PROG_NONE ? s_prog_med[f->prog[ia]] : -1.0f;
    return m >= 0.0f && 4.0f * m > abs_max ? 4.0f * m : abs_max;
}

static float spos_limit(void)
{
    return 4.0f * s_sd_med > s_spos_max ? 4.0f * s_sd_med : s_spos_max;
}

/* N's draw ia paired with N-1's ib by key k (0 strict, 1-2 loose): 1, or 0
 * when a loose pair is too unlike (pair_limit, spos_limit). Strict pairs
 * are all made, and refused afterwards with the frame's medians
 * (pair_refuse). */
static int pair_set(Frame *f, const Frame *prev, uint32_t ia, uint32_t ib,
                    int k)
{
    if (k) {
        float d = draw_dist((const float (*)[4])f->consts[ia],
                            (const float (*)[4])prev->consts[ib],
                            f->prog[ia] != PROG_NONE
                            ? s_prog[f->prog[ia]].reads : NULL);
        float sd = spos_dist(f, prev, ia, ib);
        int refuse = d > pair_limit(f, ia, s_loose_max)
                     || (s_spos_max > 0.0f && sd > spos_limit());
        if (s_log)
            s_dhist[1][d < 0.01f ? 0 : d < 0.05f ? 1 : d < 0.1f ? 2
                       : d < 0.25f ? 3 : d < 0.5f ? 4 : d < 1.0f ? 5
                       : d < 2.0f ? 6 : 7]++;
        if (s_jumplog)
            fprintf(stderr, "[ILOOSE] flip %u key %d d %.3f sd %.3f prog %08X"
                    " attr0 %08X <- %08X n %u <- %u tex0 %08X target %08X%s%c",
                    g_pb.flips, k, d, sd, f->part[ia][0], f->part[ia][1],
                    prev->part[ib][1], f->part[ia][2], prev->part[ib][2],
                    f->part[ia][4], f->part[ia][5],
                    refuse ? " REFUSED" : "", 10);
        if (refuse) {
            s_loose_refused++;
            return 0;
        }
    }
    f->pair[ia] = (int32_t)ib;
    s_taken[ib] = 1;
    return 1;
}

static int cmp_near(const void *x, const void *y)
{
    float a = ((const NearCand *)x)->d, b = ((const NearCand *)y)->d;
    return a < b ? -1 : a > b;
}

/* Pairs the group s_ka[a0..a1) with s_kb[b0..b1); the draws paired. */
static uint32_t pair_nearest(Frame *f, const Frame *prev, uint32_t a0,
                             uint32_t a1, uint32_t b0, uint32_t b1, int kk)
{
    static uint8_t vary[NV2A_VSH_CONSTANTS];
    static uint16_t rows[NV2A_VSH_CONSTANTS];
    static uint8_t useda[NEAR_MAX], usedb[NEAR_MAX];
    const float (*ref)[4] = (const float (*)[4])f->consts[s_ka[a0].i];
    uint32_t na = a1 - a0, nb = b1 - b0, nr = 0, n = 0, r, x, y, c, paired = 0;

    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        vary[r] = 0;
        for (x = a0 + 1; x < a1 && !vary[r]; x++)
            vary[r] = memcmp(f->consts[s_ka[x].i][r], ref[r], 16) != 0;
        for (x = b0 + 1; x < b1 && !vary[r]; x++)
            vary[r] = memcmp(prev->consts[s_kb[x].i][r],
                             prev->consts[s_kb[b0].i][r], 16) != 0;
        if (vary[r])
            rows[nr++] = (uint16_t)r;
    }
    for (x = 0; x < na; x++)
        for (y = 0; y < nb; y++) {
            const float (*p)[4] = (const float (*)[4])f->consts[s_ka[a0 + x].i];
            const float (*q)[4] = (const float (*)[4])prev->consts[s_kb[b0 + y].i];
            float d = 0.0f;
            for (r = 0; r < nr; r++)
                for (c = 0; c < 4; c++) {
                    float u = p[rows[r]][c], v = q[rows[r]][c];
                    float e = fabsf(u - v) / (1.0f + fabsf(u) + fabsf(v));
                    d += isfinite(e) ? e : 1.0f;
                }
            /* Ties (nothing varies) keep the order. */
            s_near_c[n].d = d + (float)(x > y ? x - y : y - x) * 1e-6f;
            s_near_c[n].a = (uint16_t)x;
            s_near_c[n].b = (uint16_t)y;
            n++;
        }
    qsort(s_near_c, n, sizeof *s_near_c, cmp_near);
    memset(useda, 0, na);
    memset(usedb, 0, nb);
    for (x = 0; x < n && paired < na && paired < nb; x++) {
        const NearCand *k = &s_near_c[x];
        if (useda[k->a] || usedb[k->b])
            continue;
        if (!pair_set(f, prev, s_ka[a0 + k->a].i, s_kb[b0 + k->b].i, kk))
            continue;
        useda[k->a] = usedb[k->b] = 1;
        paired++;
        s_near_moved += k->a != k->b;
    }
    s_near_groups++;
    return paired;
}
static uint32_t s_kcap;

/* Whether the registers rows marks jumped between a and b. A row whose
 * x, y, z is a unit vector in both (a rotation's row) turned by more than 30
 * degrees (a camera turning fast does 12 a frame): 1. With all (only the
 * camera's shared rows): a projection-like row (one or two values, 1.5-100)
 * scaled by more than half, 2; a translation (|v| >= 100) moved by more than
 * half the largest translation among those rows, 3. 0 = no jump. Colours,
 * fog and light values are left out: they flicker (It's War's gunfire). */
static int pair_jumped(const float (*a)[4], const float (*b)[4],
                       const uint8_t *rows, int all)
{
    uint32_t r, c;
    float tmax = 0.0f;

    if (all)
        for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
            for (c = 0; rows[r] && c < 4; c++) {
                if (fabsf(a[r][c]) >= 100.0f && fabsf(a[r][c]) > tmax)
                    tmax = fabsf(a[r][c]);
                if (fabsf(b[r][c]) >= 100.0f && fabsf(b[r][c]) > tmax)
                    tmax = fabsf(b[r][c]);
            }
    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        float la, lb, d;
        int nz = 0;
        if (!rows[r] || !memcmp(a[r], b[r], sizeof a[r]) || all_zero(a[r])
            || all_zero(b[r]))
            continue;
        la = a[r][0] * a[r][0] + a[r][1] * a[r][1] + a[r][2] * a[r][2];
        lb = b[r][0] * b[r][0] + b[r][1] * b[r][1] + b[r][2] * b[r][2];
        if (la > 0.9f && la < 1.1f && lb > 0.9f && lb < 1.1f) {
            d = (a[r][0] * b[r][0] + a[r][1] * b[r][1] + a[r][2] * b[r][2])
                / sqrtf(la * lb);
            if (d < 0.866f)
                return 1;
        }
        if (!all)
            continue;
        for (c = 0; c < 4; c++)
            nz += a[r][c] != 0.0f;
        for (c = 0; c < 4; c++) {
            float x = a[r][c], y = b[r][c], ax = fabsf(x), ay = fabsf(y);
            if (!isfinite(x) || !isfinite(y) || (ax <= 1.5f && ay <= 1.5f))
                continue;
            if (ax < 100.0f && ay < 100.0f) {
                if (nz <= 2 && fabsf(y - x) > (ax > ay ? ax : ay) / 2.0f)
                    return 2;
            } else if (fabsf(y - x) > tmax / 2.0f) {
                return 3;
            }
        }
    }
    return 0;
}

/* The rows of a draw's matrices that jumped between a and b, as mats_jumped
 * finds them, marked in keep (cleared first): the number of matrices. */
static int mats_jumped_rows(const float (*a)[4], const float (*b)[4],
                            uint8_t *keep)
{
    uint32_t r, k;
    int n = 0;

    memset(keep, 0, NV2A_VSH_CONSTANTS);
    for (r = 0; r + 2 < NV2A_VSH_CONSTANTS; r++) {
        float ta[3], tb[3], la = 0, lb = 0, d = 0;
        int unit = 1, turned = 0;
        for (k = 0; k < 3 && unit; k++) {
            const float *x = a[r + k], *y = b[r + k];
            float lx = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
            float ly = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
            if (!(lx > 0.9f && lx < 1.1f && ly > 0.9f && ly < 1.1f)) {
                unit = 0;
                break;
            }
            if ((x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / sqrtf(lx * ly)
                < 0.866f)
                turned = 1;
            ta[k] = x[3];
            tb[k] = y[3];
        }
        if (!unit)
            continue;
        if (!turned) {
            for (k = 0; k < 3; k++) {
                la += ta[k] * ta[k];
                lb += tb[k] * tb[k];
                d += (tb[k] - ta[k]) * (tb[k] - ta[k]);
            }
            la = sqrtf(la > lb ? la : lb);
            d = sqrtf(d);
            turned = d > 100.0f && d > la / 2.0f;
        }
        if (turned) {
            keep[r] = keep[r + 1] = keep[r + 2] = 1;
            n++;
        }
        r += 2;                         /* past this matrix */
    }
    return n;
}

/* Whether a draw's matrices jumped between a and b: each run of three rows
 * whose x, y, z are unit vectors in both (a rotation, the translation in w:
 * camera, object, bone) turned by more than 30 degrees in a row, or had its
 * translation (the three w) move by more than half its length and more than
 * 100 units. A character put somewhere else between two shots, a camera cut
 * this draw sees, a fast spin. */
static int mats_jumped(const float (*a)[4], const float (*b)[4])
{
    uint32_t r, k;

    for (r = 0; r + 2 < NV2A_VSH_CONSTANTS; r++) {
        float ta[3], tb[3], la = 0, lb = 0, d = 0;
        int unit = 1, turned = 0;
        for (k = 0; k < 3 && unit; k++) {
            const float *x = a[r + k], *y = b[r + k];
            float lx = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
            float ly = y[0] * y[0] + y[1] * y[1] + y[2] * y[2];
            if (!(lx > 0.9f && lx < 1.1f && ly > 0.9f && ly < 1.1f)) {
                unit = 0;
                break;
            }
            if ((x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / sqrtf(lx * ly)
                < 0.866f)
                turned = 1;
            ta[k] = x[3];
            tb[k] = y[3];
        }
        if (!unit)
            continue;
        if (turned)
            return 1;
        for (k = 0; k < 3; k++) {
            la += ta[k] * ta[k];
            lb += tb[k] * tb[k];
            d += (tb[k] - ta[k]) * (tb[k] - ta[k]);
        }
        la = sqrtf(la > lb ? la : lb);
        d = sqrtf(d);
        if (d > 100.0f && d > la / 2.0f)
            return 2;
        r += 2;                         /* past this matrix */
    }
    return 0;
}

/* Whether the camera jumped between prev and f: the registers nearly every
 * paired draw shares, in both frames, and that changed (the camera's
 * matrices: the same in every draw of a frame), by pair_jumped. Without
 * such registers: no (the title's own cut marker still counts). */
static int camera_jumped(const Frame *f, const Frame *prev)
{
    static uint32_t same[NV2A_VSH_CONSTANTS];
    static uint8_t rows[NV2A_VSH_CONSTANTS];
    uint32_t j, r, n = 0, nrows = 0;
    int32_t j0 = -1;
    int why;

    memset(same, 0, sizeof same);
    /* The reference: a paired draw from the middle of the frame (the first
     * ones are often a shadow picture, drawn from the light). */
    for (j = f->ndraws / 2; j < f->ndraws && j0 < 0; j++)
        if (f->pair[j] >= 0)
            j0 = (int32_t)j;
    for (j = 0; j < f->ndraws && j0 < 0; j++)
        if (f->pair[j] >= 0)
            j0 = (int32_t)j;
    for (j = 0; j < f->ndraws; j++) {
        int32_t pj = f->pair[j];
        if (pj < 0)
            continue;
        n++;
        for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
            if (!memcmp(f->consts[j][r], f->consts[j0][r], 16)
                && !memcmp(prev->consts[pj][r], prev->consts[f->pair[j0]][r],
                           16))
                same[r]++;
    }
    if (j0 < 0 || n < 8)
        return 0;
    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        rows[r] = same[r] * 10 >= n * 8
                  && memcmp(f->consts[j0][r], prev->consts[f->pair[j0]][r], 16);
        nrows += rows[r];
    }
    /* A camera is at least a 3-row view matrix (Uga Buga shares 2 rows of
     * something else, which jumped in half its frames). */
    why = nrows >= 3
          ? pair_jumped((const float (*)[4])prev->consts[f->pair[j0]],
                        (const float (*)[4])f->consts[j0], rows, 1) : 0;

    if (s_log >= 2) {
        /* RECOMP_INTERP_LOG=2: the camera's change every frame. */
        const float (*a)[4] = (const float (*)[4])prev->consts[f->pair[j0]];
        const float (*b)[4] = (const float (*)[4])f->consts[j0];
        float rot = 0, tr = 0, tmax = 0, sc = 0;
        uint32_t c;
        for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
            for (c = 0; rows[r] && c < 4; c++) {
                float x = a[r][c], y = b[r][c], m = fabsf(x) > fabsf(y)
                                                    ? fabsf(x) : fabsf(y);
                if (m <= 1.5f) {
                    if (fabsf(y - x) > rot) rot = fabsf(y - x);
                } else if (m < 100.0f) {
                    if (fabsf(y - x) / m > sc) sc = fabsf(y - x) / m;
                } else {
                    if (m > tmax) tmax = m;
                    if (fabsf(y - x) > tr) tr = fabsf(y - x);
                }
            }
        static uint8_t all[NV2A_VSH_CONSTANTS];
        uint32_t changed = 0, jumped = 0;
        if (!all[0])
            memset(all, 1, sizeof all);
        for (j = 0; j < f->ndraws; j++) {
            int32_t pj = f->pair[j];
            if (pj < 0 || !memcmp(f->consts[j], prev->consts[pj],
                                   sizeof f->consts[j]))
                continue;
            changed++;
            jumped += pair_jumped((const float (*)[4])prev->consts[pj],
                                  (const float (*)[4])f->consts[j], all, 0) != 0;
        }
        fprintf(stderr, "[ICAM] flip %u: rows %u, turn %.3f, scale %.3f, move"
                " %.1f of %.1f%s; pairs jumped %u of %u changed%c", g_pb.flips,
                nrows, rot, sc, tr, tmax, why ? " CUT" : "", jumped, changed,
                10);
    }
    if (why && s_log)
        fprintf(stderr, "[ICUT] flip %u: camera jumped (%s, %u shared"
                " registers)%c", g_pb.flips, why == 1 ? "turned"
                : why == 2 ? "projection" : why == 3 ? "moved"
                : "a third of the draws", nrows, 10);
    return why != 0;
}

static uint8_t *s_taken;                /* prev's draws already paired */
static uint32_t s_taken_cap;

/* One pass of pairing on the draws still without a pair, by key k: 0 the
 * strict key, 1-2 the loose ones. The draws paired. */
static uint32_t pair_pass(Frame *f, const Frame *prev, int k);

/* f->pair from prev; the number of draws paired. */
typedef struct { uint16_t prog; float d; } ProgDist;

static int cmp_progdist(const void *x, const void *y)
{
    const ProgDist *a = x, *b = y;
    if (a->prog != b->prog)
        return a->prog < b->prog ? -1 : 1;
    return a->d < b->d ? -1 : a->d > b->d;
}

static int cmp_float(const void *x, const void *y)
{
    float a = *(const float *)x, b = *(const float *)y;
    return a < b ? -1 : a > b;
}

/* After the strict pass: the frame's medians (s_prog_med, s_sd_med), then
 * the strict pairs far less alike than their program's typical one
 * (RECOMP_INTERP_PAIR_MAX, at least, 1.0), or whose first vertex moved far
 * more across the picture than the frame's typical one
 * (RECOMP_INTERP_SPOS_MAX, at least, 0.3), unpaired: one mesh in two unlike
 * places (a model reused, a teleport, identical bottles leaving and coming
 * in) flew through the picture. The draws unpaired. */
static uint32_t pair_refuse(Frame *f, const Frame *prev)
{
    static ProgDist *pd;
    static float *dd, *sds;
    static uint32_t cap;
    uint32_t j, n = 0, ns = 0, a, b, refused = 0;

    for (j = 0; j < PROG_MAX; j++)
        s_prog_med[j] = -1.0f;
    s_sd_med = 0.0f;
    if (cap < f->ndraws) {
        ProgDist *x = realloc(pd, f->ndraws * sizeof *x);
        float *y = realloc(dd, f->ndraws * sizeof *y);
        float *z = realloc(sds, f->ndraws * sizeof *z);
        if (x) pd = x;
        if (y) dd = y;
        if (z) sds = z;
        if (!x || !y || !z)
            return 0;
        cap = f->ndraws;
    }
    for (j = 0; j < f->ndraws; j++) {
        int32_t pj = f->pair[j];
        float sd;
        dd[j] = 0.0f;
        if (pj < 0)
            continue;
        dd[j] = draw_dist((const float (*)[4])f->consts[j],
                          (const float (*)[4])prev->consts[pj],
                          f->prog[j] != PROG_NONE ? s_prog[f->prog[j]].reads
                                                  : NULL);
        if (s_log)
            s_dhist[0][dd[j] < 0.01f ? 0 : dd[j] < 0.05f ? 1
                       : dd[j] < 0.1f ? 2 : dd[j] < 0.25f ? 3
                       : dd[j] < 0.5f ? 4 : dd[j] < 1.0f ? 5
                       : dd[j] < 2.0f ? 6 : 7]++;
        if (f->prog[j] != PROG_NONE) {
            pd[n].prog = f->prog[j];
            pd[n++].d = dd[j];
        }
        if ((sd = spos_dist(f, prev, j, (uint32_t)pj)) >= 0.0f)
            sds[ns++] = sd;
    }
    qsort(pd, n, sizeof *pd, cmp_progdist);
    for (a = 0; a < n; a = b) {
        for (b = a; b < n && pd[b].prog == pd[a].prog; b++)
            ;
        s_prog_med[pd[a].prog] = pd[a + (b - a) / 2].d;
    }
    if (ns) {
        qsort(sds, ns, sizeof *sds, cmp_float);
        s_sd_med = sds[ns / 2];
    }
    for (j = 0; j < f->ndraws; j++) {
        int32_t pj = f->pair[j];
        float sd;
        int why;
        if (pj < 0)
            continue;
        sd = spos_dist(f, prev, j, (uint32_t)pj);
        why = dd[j] > pair_limit(f, j, s_pair_max) ? 1
            : s_spos_max > 0.0f && sd > spos_limit() ? 2 : 0;
        if (!why)
            continue;
        if (s_jumplog)
            fprintf(stderr, "[IREFUSE] flip %u draw %u %s: d %.3f (median %.3f)"
                    " sd %.3f (median %.3f) prog %08X attr0 %08X n %u tex0"
                    " %08X target %08X%c", g_pb.flips, j,
                    why == 1 ? "unlike" : "moved", dd[j],
                    f->prog[j] != PROG_NONE ? s_prog_med[f->prog[j]] : -1.0f,
                    sd, s_sd_med, f->part[j][0], f->part[j][1], f->part[j][2],
                    f->part[j][4], f->part[j][5], 10);
        if (why == 1)
            s_pair_refused++;
        else
            s_spos_refused++;
        s_taken[pj] = 0;
        f->pair[j] = -1;
        refused++;
    }
    return refused;
}

static uint32_t pair_frames(Frame *f, const Frame *prev)
{
    uint32_t i, paired;

    for (i = 0; i < f->ndraws; i++)
        f->pair[i] = -1;
    if (!prev->ndraws || !f->ndraws)
        return 0;
    if (s_taken_cap < prev->ndraws) {
        uint8_t *t = realloc(s_taken, prev->ndraws);
        if (!t)
            return 0;
        s_taken = t;
        s_taken_cap = prev->ndraws;
    }
    memset(s_taken, 0, prev->ndraws);
    paired = pair_pass(f, prev, 0);
    paired -= pair_refuse(f, prev);
    if (!s_strict) {
        uint32_t p1 = pair_pass(f, prev, 1);
        uint32_t p2 = pair_pass(f, prev, 2);
        s_st.loose1 += p1;
        s_st.loose2 += p2;
        paired += p1 + p2;
    }
    return paired;
}

static uint32_t pair_pass(Frame *f, const Frame *prev, int k)
{
    uint32_t i, a, b, na = 0, nb = 0, paired = 0;
    if (s_kcap < f->ndraws || s_kcap < prev->ndraws) {
        uint32_t cap = f->ndraws > prev->ndraws ? f->ndraws : prev->ndraws;
        KeyIdx *x = realloc(s_ka, cap * sizeof *x), *y;
        if (!x)
            return 0;
        s_ka = x;
        y = realloc(s_kb, cap * sizeof *y);
        if (!y)
            return 0;
        s_kb = y;
        s_kcap = cap;
    }
    for (i = 0; i < f->ndraws; i++) {
        if (f->pair[i] >= 0)
            continue;
        s_ka[na].key = k ? f->loose[i][k - 1] : f->key[i];
        s_ka[na++].i = i;
    }
    for (i = 0; i < prev->ndraws; i++) {
        if (s_taken[i])
            continue;
        s_kb[nb].key = k ? prev->loose[i][k - 1] : prev->key[i];
        s_kb[nb++].i = i;
    }
    qsort(s_ka, na, sizeof *s_ka, cmp_keyidx);
    qsort(s_kb, nb, sizeof *s_kb, cmp_keyidx);
    for (a = b = 0; a < na && b < nb; ) {
        uint32_t a1, b1;
        if (s_ka[a].key < s_kb[b].key) {
            a++;
            continue;
        }
        if (s_ka[a].key > s_kb[b].key) {
            b++;
            continue;
        }
        /* A group: the draws of one key in both frames. */
        for (a1 = a; a1 < na && s_ka[a1].key == s_ka[a].key; a1++)
            ;
        for (b1 = b; b1 < nb && s_kb[b1].key == s_kb[b].key; b1++)
            ;
        if (!k && (a1 - a > 1 || b1 - b > 1))
            s_st.dup_draws += a1 - a;
        if (s_nearest && (a1 - a > 1 || b1 - b > 1)
            && a1 - a <= NEAR_MAX && b1 - b <= NEAR_MAX && !s_unique)
            paired += pair_nearest(f, prev, a, a1, b, b1, k);
        else if (!(s_unique && (a1 - a > 1 || b1 - b > 1)))
            for (; a < a1 && b < b1; a++, b++)
                paired += pair_set(f, prev, s_ka[a].i, s_kb[b].i, k);
        a = a1;
        b = b1;
    }
    return paired;
}

static void run_stream(const Frame *f)
{
    size_t i = 0;
    uint32_t e = 0;

    while (i < f->nwords) {
        uint32_t w = f->words[i];
        if (!(++e & 63) && s_q_pending)
            show_due(0);                /* the frame before's last pictures */
        switch (w >> 29) {
        case TAG_METHOD:
            nv2a_pb_exec_method((w >> 16) & 7u, w & 0xFFFFu, f->words[i + 1]);
            i += 2;
            break;
        case TAG_WORDS: {
            uint32_t m = w & 0xFFFFu, n = f->words[i + 1], k;
            const uint32_t *p = f->words + i + 2;
            if (!nv2a_pb_exec_draw_words(m, p, n))
                for (k = 0; k < n; k++)
                    nv2a_pb_exec_method(0, m, p[k]);
            i += 2 + n;
            break;
        }
        case TAG_FLUSH:
            nv2a_pb_merge_flush();
            i++;
            break;
        default:
            return;                     /* cannot happen */
        }
    }
    nv2a_pb_merge_flush();
}

/* Generations of the surfaces carried between frames (NV_INTERP_KEEP):
 * taken at every flip, numbered; s_gen_seq[g] is the flip count generation g
 * was taken at (0: none). A frame starts as the frame before ended. */
static uint32_t s_keep_seq, s_gen_seq[3];
static int s_keep_gen;

static void put_seq(uint32_t seq)
{
    int g;
    for (g = 0; g < 3 && seq; g++)
        if (s_gen_seq[g] == seq) {
            s_be->interp(NV_INTERP_PUT, g);
            return;
        }
}

/* Frame f run again from its start (start_seq: the generation it started
 * from, put back first), each paired draw's constants moved w of the way
 * toward its partner's in frame partner (map: f's draw -> partner's). */
static void replay(Frame *f, Frame *partner, const int32_t *map, float w,
                   uint32_t start_seq, const int32_t *donor)
{
    put_seq(start_seq);
    s_rep_donor = donor;
    memcpy(&g_pb, f->start_pb, sizeof g_pb);
    nv2a_vsh_state_load(f->start_vsh);
    pb_transform_forget();
    s_rep_id++;
    s_hold = 0;
    s_rep_f = f;
    s_rep_prev = partner;
    s_rep_map = map;
    s_rep_j = 0;
    s_rep_t = w;
    s_rep = 1;
    run_stream(f);
    s_rep = 0;
    s_hold = 0;
    pb_transform_forget();
}

static LONGLONG qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void stats_line(LONGLONG now)
{
    if (!s_log)
        return;
    if (!s_st_at) {
        s_st_at = now + s_freq.QuadPart * 10;
        return;
    }
    if (now < s_st_at)
        return;
    s_st_at = now + s_freq.QuadPart * 10;
    fprintf(stderr, "[INTERP] 10 s: %u frames, %u interpolated, %u cuts"
            " (%u the title's, %u seen), %u too big, %u without the frame"
            " before; %.1f draws a frame, %.1f %%"
            " paired (%.1f %% by the loose keys), %.1f %% of one key drawn"
            " more than once, %llu runs went another way, %llu index unions,"
            " %llu draws jumped (as N), %llu middles from both, %u texture"
            " wraps (%u snapped); replays %.2f"
            " ms a frame"
            " (most %.2f); %u pictures shown, %u late, %u dropped%c",
            s_st.frames, s_st.interpolated, s_st.cuts, s_st.game_cuts,
            s_st.auto_cuts, s_st.overflows,
            s_st.unpaired_frames,
            s_st.frames ? (double)s_st.draws / s_st.frames : 0.0,
            s_st.draws ? 100.0 * (double)s_st.paired / (double)s_st.draws : 0.0,
            s_st.draws ? 100.0 * (double)(s_st.loose1 + s_st.loose2)
                         / (double)s_st.draws : 0.0,
            s_st.draws ? 100.0 * (double)s_st.dup_draws / (double)s_st.draws
                       : 0.0,
            (unsigned long long)s_st.diverged,
            (unsigned long long)s_st.unions,
            (unsigned long long)s_st.teleports,
            (unsigned long long)s_st.both, s_wraps, s_wrap_snaps,
            s_st.interpolated ? s_st.replay_ms / s_st.interpolated : 0.0,
            s_st.replay_max, s_st.shown, s_st.late, s_st.dropped, 10);
    if (s_vcheck)
        fprintf(stderr, "[INTERP] %u replayed draws read vertices changed since"
                " they were recorded%c", s_vdiff, 10);
    fprintf(stderr, "[INTERP] nearest pairing: %llu groups, %llu draws paired"
            " out of order; %llu strict and %llu loose pairs refused as too"
            " unlike, %llu as moved across the picture; %u held draws; %u"
            " unpaired draws moved with the camera; %u"
            " turned matrices kept their size%c",
            (unsigned long long)s_near_groups,
            (unsigned long long)s_near_moved,
            (unsigned long long)s_pair_refused,
            (unsigned long long)s_loose_refused,
            (unsigned long long)s_spos_refused, s_held, s_borrowed,
            s_rot_fixed, 10);
    s_borrowed = 0;
    s_pair_refused = s_spos_refused = 0;
    s_held = 0;
    fprintf(stderr, "[INTERP] pair distances <.01 <.05 <.1 <.25 <.5 <1 <2"
            " more: strict %u %u %u %u %u %u %u %u, loose %u %u %u %u %u %u"
            " %u %u%c", s_dhist[0][0], s_dhist[0][1], s_dhist[0][2],
            s_dhist[0][3], s_dhist[0][4], s_dhist[0][5], s_dhist[0][6],
            s_dhist[0][7], s_dhist[1][0], s_dhist[1][1], s_dhist[1][2],
            s_dhist[1][3], s_dhist[1][4], s_dhist[1][5], s_dhist[1][6],
            s_dhist[1][7], 10);
    memset(s_dhist, 0, sizeof s_dhist);
    s_near_groups = s_near_moved = s_loose_refused = 0;
    s_rot_fixed = 0;
    s_vdiff = 0;
    fprintf(stderr, "[INTERP] gaps between pictures (ms) <5: %u, 5-7: %u, 7-9.5:"
            " %u, 9.5-12: %u, 12-17: %u, >17: %u%c", s_gap[0], s_gap[1],
            s_gap[2], s_gap[3], s_gap[4], s_gap[5], 10);
    fflush(stderr);
    memset(&s_st, 0, sizeof s_st);
    memset(s_gap, 0, sizeof s_gap);
    s_wraps = s_wrap_snaps = 0;
}

static void note_shown(void)
{
    LONGLONG now = qpc();
    if (s_gap_last) {
        double ms = (double)(now - s_gap_last) * 1000.0 / (double)s_freq.QuadPart;
        s_gap[ms < 5.0 ? 0 : ms < 7.0 ? 1 : ms < 9.5 ? 2 : ms < 12.0 ? 3
              : ms < 17.0 ? 4 : 5]++;
    }
    s_gap_last = now;
}

static void show_due(int all)
{
    while (s_q_next < s_q_n) {
        LONGLONG now = qpc();
        if (!all && now < s_q_at[s_q_next])
            return;
        if (now - s_q_at[s_q_next] > s_freq.QuadPart / 500)
            s_st.late++;
        s_be->interp(NV_INTERP_SHOW, s_q_slot[s_q_next]);
        note_shown();
        s_st.shown++;
        s_q_next++;
    }
    s_q_n = s_q_next = s_q_old = 0;
    InterlockedExchange(&s_q_pending, 0);
}

/* The frame before's pictures still waiting go out now, before anything of
 * this frame (a frame that flips early -- a late one catching up -- would
 * otherwise show the frame before's last picture after its own first: at a
 * cut, the old shot for a picture). */
static void flush_old(void)
{
    while (s_q_next < s_q_old && s_q_next < s_q_n) {
        s_st.late++;
        s_be->interp(NV_INTERP_SHOW, s_q_slot[s_q_next]);
        note_shown();
        s_st.shown++;
        s_q_next++;
    }
    if (s_q_next >= s_q_n) {
        s_q_n = s_q_next = 0;
        InterlockedExchange(&s_q_pending, 0);
    }
    s_q_old = 0;
}

void nv2a_interp_tick(void)
{
    if (!s_q_pending)
        return;
    show_due(0);
}

/* Until QPC time t: pictures due meanwhile go out, the GPU's handshakes go
 * on (xbox_Nv2aIdle). */
static void wait_until(LONGLONG t)
{
    for (;;) {
        LONGLONG now = qpc();
        if (s_q_pending)
            show_due(0);
        if (now >= t)
            return;
        if (t - now > s_freq.QuadPart * 3 / 2000)
            xbox_Nv2aIdle(1);
        else
            YieldProcessor();
    }
}

int nv2a_interp_due_ms(void)
{
    LONGLONG d;

    if (!s_q_pending || s_q_next >= s_q_n)
        return -1;
    d = s_q_at[s_q_next] - qpc();
    return d <= 0 ? 0 : (int)(d * 1000 / s_freq.QuadPart);
}

/* RECOMP_INTERP_TRACE: frame f's draws against prev's. */
static void trace_frame(const Frame *f, const Frame *prev)
{
    uint32_t j, r, c;

    fprintf(stderr, "[ITRACE] frame: %u draws, %u before%c", f->ndraws,
            prev->ndraws, 10);
    for (j = 0; j < f->ndraws; j++) {
        const uint32_t *pt = f->part[j];
        int32_t pj = f->pair[j];
        uint32_t ndiff = 0;
        char line[8192];
        int len;
        len = snprintf(line, sizeof line, "[ITRACE] %4u pair %5d prog %08X"
                       " attr0 %08X n %5u prim %X tex0 %08X target %08X:",
                       j, pj, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]);
        if (pj >= 0) {
            int why = mats_jumped((const float (*)[4])f->consts[j],
                                  (const float (*)[4])prev->consts[pj]);
            if (why)
                len += snprintf(line + len, sizeof line - len, " JUMPED(%s)",
                                why == 1 ? "turned" : "moved");
        }
        if (pj >= 0 && f->key[j] != prev->key[pj])
            len += snprintf(line + len, sizeof line - len, " LOOSE (before:"
                            " attr0 %08X n %u tex0 %08X)", prev->part[pj][1],
                            prev->part[pj][2], prev->part[pj][4]);
        if (pj >= 0) {
            const float (*a)[4] = (const float (*)[4])prev->consts[pj];
            const float (*b)[4] = (const float (*)[4])f->consts[j];
            for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
                if (!memcmp(a[r], b[r], sizeof a[r]))
                    continue;
                ndiff++;
                if (len < (int)sizeof line - 120) {
                    len += snprintf(line + len, sizeof line - len, " c%u", r);
                    for (c = 0; c < 4; c++)
                        len += snprintf(line + len, sizeof line - len,
                                        "%s%.4g>%.4g", c ? "," : "(",
                                        a[r][c], b[r][c]);
                    len += snprintf(line + len, sizeof line - len, ")");
                }
            }
        }
        fprintf(stderr, "%s [%u differ]%c", line, ndiff, 10);
    }
    {
        /* The frame before's draws this frame has no pair for: what it
         * stopped drawing (culled, gone). */
        uint8_t *taken = calloc(prev->ndraws ? prev->ndraws : 1, 1);
        for (j = 0; taken && j < f->ndraws; j++)
            if (f->pair[j] >= 0)
                taken[f->pair[j]] = 1;
        for (j = 0; taken && j < prev->ndraws; j++) {
            const uint32_t *pt = prev->part[j];
            if (taken[j])
                continue;
            fprintf(stderr, "[ITRACE] gone %4u prog %08X attr0 %08X n %5u prim"
                    " %X tex0 %08X target %08X%c", j, pt[0], pt[1], pt[2],
                    pt[3], pt[4], pt[5], 10);
        }
        free(taken);
    }
    fflush(stderr);
}

/* For each draw of a without a partner (map[j] < 0), the nearest paired
 * draw of the same program in draw order (within 256) that shares with it,
 * bit for bit, a register its own pair changed (the camera's): out[j], or
 * -1. */
static void find_donors(const Frame *a, const int32_t *map, const Frame *b,
                        int32_t *out)
{
    uint32_t j, k, r;

    for (j = 0; j < a->ndraws; j++) {
        out[j] = -1;
        if (map[j] >= 0)
            continue;
        for (k = 1; k <= 256 && out[j] < 0; k++) {
            int s2;
            for (s2 = 0; s2 < 2 && out[j] < 0; s2++) {
                int64_t d = s2 ? (int64_t)j + k : (int64_t)j - k;
                const float (*x)[4], (*y)[4];
                if (d < 0 || d >= a->ndraws || map[d] < 0
                    || a->part[d][0] != a->part[j][0])
                    continue;
                x = (const float (*)[4])a->consts[d];
                y = (const float (*)[4])b->consts[map[d]];
                for (r = 0; r < NV2A_VSH_CONSTANTS; r++)
                    if (memcmp(x[r], y[r], 16)
                        && !memcmp(x[r], a->consts[j][r], 16)) {
                        out[j] = (int32_t)d;
                        break;
                    }
            }
        }
    }
}

static void flip_before(const Nv2aPbBackend *be);

void nv2a_interp_flip_before(const Nv2aPbBackend *be)
{
    flip_before(be);
    if (s_be && s_q_old)
        flush_old();                    /* before this frame's own present */
}

static void flip_before(const Nv2aPbBackend *be)
{
    Frame *f = &s_fr[s_cur], *prev = &s_fr[s_cur ^ 1];
    LONGLONG now = qpc(), t0;
    uint32_t paired, k;
    int n, inv_ok;

    if (nv2a_interp_n() < 2 || !be->interp)
        return;
    s_be = be;
    /* Pictures of the frame before still waiting stay queued (they are due
     * while this frame is drawn again); compacted to the front. */
    if (s_q_pending && s_q_next < s_q_n) {
        int i, m = s_q_n - s_q_next;
        for (i = 0; i < m; i++) {
            s_q_slot[i] = s_q_slot[s_q_next + i];
            s_q_at[i] = s_q_at[s_q_next + i];
        }
        s_q_n = m;
        s_q_next = 0;
        s_q_old = m;
    } else {
        s_q_n = s_q_next = s_q_old = 0;
        InterlockedExchange(&s_q_pending, 0);
    }
    if (s_last_flip) {
        LONGLONG gap = now - s_last_flip;
        if (gap > s_freq.QuadPart / 10)
            gap = s_freq.QuadPart / 10;
        s_period += (gap - s_period) / 8;
    }
    s_last_flip = now;
    stats_line(now);
    /* The surfaces carried from frame to frame, as this frame leaves them:
     * the next one's start. */
    s_keep_seq++;
    s_keep_gen = (s_keep_gen + 1) % 3;
    s_gen_seq[s_keep_gen] = s_carry && be->interp(NV_INTERP_KEEP, s_keep_gen)
                            ? s_keep_seq : 0;
    if (!s_recording)
        return;
    s_st.frames++;
    s_st.draws += f->ndraws;
    if (f->overflow) {
        s_st.overflows++;
        return;
    }
    if (!f->valid || !prev->valid || prev->overflow) {
        s_st.unpaired_frames++;
        return;
    }
    paired = pair_frames(f, prev);
    s_st.paired += paired;
    s_rep_blend = f->ndraws && paired >= INTERP_MIN_PAIRED * f->ndraws;
    if (!s_rep_blend)
        s_st.cuts++;
    if (s_cut) {
        s_rep_blend = 0;
        s_st.game_cuts++;
    } else if (s_rep_blend && s_autocut > 0.0
               && camera_jumped(f, prev)) {
        s_rep_blend = 0;
        s_st.auto_cuts++;
    }
    if (s_mode == 2)
        s_rep_blend = 0;
    {
        /* RECOMP_INTERP_JUMPLOG=1 (test): a line per paired draw whose
         * matrices jumped (drawn as its own frame among blended ones). */
        static int jl = -1;
        uint32_t j;
        if (jl < 0)
            jl = getenv("RECOMP_INTERP_JUMPLOG") != NULL;
        for (j = 0; jl && s_rep_blend && j < f->ndraws; j++) {
            int32_t pj = f->pair[j], why;
            if (pj < 0)
                continue;
            why = mats_jumped((const float (*)[4])f->consts[j],
                              (const float (*)[4])prev->consts[pj]);
            if (why)
                fprintf(stderr, "[IJUMP] flip %u draw %u/%u pair %d %s prog"
                        " %08X attr0 %08X n %u tex0 %08X target %08X%s%c",
                        g_pb.flips, j, f->ndraws, pj,
                        why == 1 ? "turned" : "moved", f->part[j][0],
                        f->part[j][1], f->part[j][2], f->part[j][4],
                        f->part[j][5], f->key[j] != prev->key[pj]
                        ? " (loose pair)" : "", 10);
        }
    }
    ctl_read();
    {
        static const char *tr = (const char *)-1;
        static int armed = 1;
        if (tr == (const char *)-1)
            tr = getenv("RECOMP_INTERP_TRACE");
        {
            /* RECOMP_INTERP_TRACE_FLIPS=<a>-<b>: every frame in that range. */
            static int lo = -2, hi = -1;
            if (lo == -2) {
                const char *e = getenv("RECOMP_INTERP_TRACE_FLIPS");
                lo = -1;
                if (e)
                    sscanf(e, "%d-%d", &lo, &hi);
            }
            if (lo >= 0 && (int)g_pb.flips >= lo && (int)g_pb.flips <= hi) {
                fprintf(stderr, "[ITRACE] flip %u%c", g_pb.flips, 10);
                trace_frame(f, prev);
            }
        }
        if (tr) {
            FILE *t = fopen(tr, "rb");
            if (t) {
                fclose(t);
                if (armed) {
                    trace_frame(f, prev);
                    s_dump_progs = 1;
                } else {
                    s_dump_progs = 0;
                }
                armed = 0;
            } else {
                armed = 1;
                s_dump_progs = 0;
            }
        }
    }
    /* As many pictures as make n x 30 a second at the title's own rate: a
     * 60 Hz part (menus, the opening movies) gets half as many. */
    n = (int)((double)s_n * (double)s_period * 30.0 / (double)s_freq.QuadPart
              + 0.5);
    if (n > s_n)
        n = s_n;
    if (n < 2)
        return;
    if (!be->interp(NV_INTERP_SAVE, s_base))
        return;                         /* nothing shown this frame */
    if (!s_end_pb) {
        s_end_pb = malloc(sizeof *s_end_pb);
        s_end_vsh = malloc(s_vsh_size);
        if (!s_end_pb || !s_end_vsh)
            return;
    }
    memcpy(s_end_pb, &g_pb, sizeof g_pb);
    nv2a_vsh_state_save(s_end_vsh);
    t0 = qpc();
    be->interp(NV_INTERP_UNDER, s_underlay);
    /* A picture nearer the frame before is that frame drawn again, moved
     * toward this one (RECOMP_INTERP_NEAR, on): what this frame culled at
     * its edge, and the frame before still drew, stays in the picture. The
     * others are this frame moved back. The map from the frame before's
     * draws to this one's is made for every frame that blends, whatever n
     * is: the middle picture from both frames uses it at n = 2 too (a menu
     * at 60 Hz), where an older frame's map paired wrong draws. */
    inv_ok = 0;
    if (s_rep_blend && prev->start_pb) {
        uint32_t i;
        if (s_inv_cap < prev->ndraws) {
            int32_t *v = realloc(s_inv, prev->ndraws * sizeof *v);
            if (v) {
                s_inv = v;
                s_inv_cap = prev->ndraws;
            }
        }
        if (s_inv_cap >= prev->ndraws) {
            for (i = 0; i < prev->ndraws; i++)
                s_inv[i] = -1;
            for (i = 0; i < f->ndraws; i++)
                if (f->pair[i] >= 0)
                    s_inv[f->pair[i]] = (int32_t)i;
            inv_ok = 1;
        }
    }
    {
        /* Who unpaired draws borrow the camera from (find_donors). */
        int ok = s_borrow && s_rep_blend;
        if (ok && s_donor_fcap < f->ndraws) {
            int32_t *v = realloc(s_donor_f, f->ndraws * sizeof *v);
            if (v) { s_donor_f = v; s_donor_fcap = f->ndraws; } else ok = 0;
        }
        if (ok && inv_ok && s_donor_pcap < prev->ndraws) {
            int32_t *v = realloc(s_donor_p, prev->ndraws * sizeof *v);
            if (v) { s_donor_p = v; s_donor_pcap = prev->ndraws; } else ok = 0;
        }
        if (ok) {
            find_donors(f, f->pair, prev, s_donor_f);
            if (inv_ok)
                find_donors(prev, s_inv, f, s_donor_p);
        }
        if (!ok || !inv_ok) {
            /* None this frame: replay() is given NULL below. */
        }
        s_donors_ok = ok;
        s_donors_p_ok = ok && inv_ok;
    }
    for (k = 1; k < (uint32_t)n; k++) {
        float t = (float)k / (float)n;
        int older = s_near && s_rep_blend && n > 2 && 2 * k < (uint32_t)n
                  && inv_ok;
        if (s_mode == 3 && k == 1) {
            older = 1;                  /* test: N-1 again, as it was */
            s_rep_blend = 0;
        }
        if (s_mode != 1 && s_both && s_rep_blend && s_lat_ms < (s_both == 3 ? 99.0 : 14.0)
            && (s_both == 2 || 2 * k == (uint32_t)n) && inv_ok) {
            /* The middle picture from both frames, the nearer of the two
             * per pixel (RECOMP_INTERP_BOTH): each fills what the other
             * culled past its edge. */
            replay(prev, f, s_inv, t, s_keep_seq - 2, s_donors_p_ok ? s_donor_p : NULL);
            if (be->interp(NV_INTERP_SAVE, s_base + (int)k)
                && be->interp(NV_INTERP_DEPTH, 0)) {
                replay(f, prev, f->pair, 1.0f - t, s_keep_seq - 1, s_donors_ok ? s_donor_f : NULL);
                /* Ties to the frame nearer in time: N-1's picture before
                 * the middle, N's from it on. */
                if (be->interp(NV_INTERP_BOTH, (s_base + (int)k)
                               | (2 * k < (uint32_t)n ? NV_INTERP_PREFER_OLD
                                                      : 0))) {
                    s_st.both++;
                    continue;
                }
            }
            replay(f, prev, f->pair, 1.0f - t, s_keep_seq - 1, s_donors_ok ? s_donor_f : NULL);
        } else if (s_mode != 1) {
            if (older)
                replay(prev, f, s_inv, t, s_keep_seq - 2, s_donors_p_ok ? s_donor_p : NULL);
            else
                replay(f, prev, f->pair, 1.0f - t, s_keep_seq - 1, s_donors_ok ? s_donor_f : NULL);
        }
        if (s_mode == 3 && k == 1)
            s_rep_blend = 1;
        if (!be->interp(NV_INTERP_SAVE, s_base + (int)k))
            break;
    }
    be->interp(NV_INTERP_UNDER, 0);
    put_seq(s_keep_seq);                /* the real end, for the next frame */
    memcpy(&g_pb, s_end_pb, sizeof g_pb);
    nv2a_vsh_state_load(s_end_vsh);
    {
        double ms = (double)(qpc() - t0) * 1000.0 / (double)s_freq.QuadPart;
        s_st.replay_ms += ms;
        if (ms > s_st.replay_max)
            s_st.replay_max = ms;
        /* The delay from the flip to the first picture: a little more than
         * drawing them takes, the same every frame, so every picture is a
         * period / n after the one before even when drawing takes most of
         * the period (the mansion: 17 ms of 33). */
        if (s_lat_ms < 0.0)
            s_lat_ms = ms * 1.05 + 0.7;
        else
            s_lat_ms += (ms * 1.05 + 0.7 - s_lat_ms) / 16.0;
        if (s_lat_ms > 28.0)
            s_lat_ms = 28.0;
    }
    if (k < (uint32_t)n) {
        be->interp(NV_INTERP_LOAD, s_base);  /* shown as it is */
        return;
    }
    s_st.interpolated++;
    /* The first in-between goes out at flip + the delay, in N's place (the
     * back end's own present follows); the rest, N last, a period / n
     * apart. The next frame's slots are the other four. */
    wait_until(now + (LONGLONG)(s_lat_ms * (double)s_freq.QuadPart / 1000.0));
    if (s_q_old)
        flush_old();
    be->interp(NV_INTERP_LOAD, s_base + 1);
    {
        LONGLONG step = s_period / n, at = qpc();
        for (k = 2; k <= (uint32_t)n && s_q_n < 2 * INTERP_MAX_N; k++) {
            s_q_slot[s_q_n] = s_base + (k < (uint32_t)n ? (int)k : 0);
            s_q_at[s_q_n] = at + step * (LONGLONG)(k - 1);
            s_q_n++;
        }
    }
    s_q_shown_base = s_base;
    s_base ^= 4;
}

void nv2a_interp_flip_after(void)
{
    Frame *f;

    if (nv2a_interp_n() < 2 || !s_be)
        return;
    note_shown();                       /* the frame's own present */
    if (s_q_shown_base >= 0) {
        /* N back into its surface (the next frame may read it). */
        s_be->interp(NV_INTERP_LOAD, s_q_shown_base);
        s_q_shown_base = -1;
    }
    if (s_q_n > s_q_next)
        InterlockedExchange(&s_q_pending, 1);
    /* Frame N+1 starts here. */
    s_cut = 0;
    s_hold = 0;
    s_cur ^= 1;
    f = &s_fr[s_cur];
    f->nwords = 0;
    f->nidx = 0;
    f->ndraws = 0;
    f->overflow = 0;
    if (!s_vsh_size)
        s_vsh_size = nv2a_vsh_state_size();
    if (!f->start_pb) {
        f->start_pb = malloc(sizeof *f->start_pb);
        f->start_vsh = malloc(s_vsh_size);
    }
    f->valid = f->start_pb && f->start_vsh;
    if (f->valid) {
        memcpy(f->start_pb, &g_pb, sizeof g_pb);
        nv2a_vsh_state_save(f->start_vsh);
    }
    s_recording = 1;
}
