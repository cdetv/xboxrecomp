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
 * frame with too few pairs (a camera cut, a load), is drawn as N. Constants
 * that are the same in both, or all zero in one of them, are N's.
 *
 * Guest memory the frame read (vertex arrays, textures) is read again when
 * the stream runs again; what the title has rewritten since is drawn as it is
 * now.
 *
 * RECOMP_INTERP_LOG=1: an [INTERP] line every 10 s (frames, draws, pairs,
 * replay time, pictures shown late or dropped). RECOMP_INTERP_MODE (tests):
 * 1 = every picture is N (pacing only, nothing drawn again), 2 = drawn again
 * with N's own constants (must come out as N). RECOMP_INTERP_TRACE=<file>:
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

#define INTERP_MAX_N      8
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
    uint32_t (*part)[6];                /* for traces: prog, attr0, n, prim,
                                         * tex0, target */
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
static Frame s_fr[2];
static int s_cur;                       /* s_fr[s_cur] is being recorded */
static int s_recording;
static int s_rep;                       /* replaying */
static Frame *s_rep_f, *s_rep_prev;
static uint32_t s_rep_j;
static float s_rep_t;
static int s_rep_blend;                 /* this frame blends (not a cut) */
static VshConsts s_tmp;
#define CTL_MAX 32
static uint32_t s_only[CTL_MAX], s_skip[CTL_MAX];
static int s_nonly, s_nskip;
static Nv2aPbState *s_end_pb;
static void *s_end_vsh;
static size_t s_vsh_size;

/* Pictures waiting to be shown: slot and when (QPC). */
static int s_q_slot[INTERP_MAX_N];
static LONGLONG s_q_at[INTERP_MAX_N];
static int s_q_n, s_q_next;
static volatile LONG s_q_pending;
static const Nv2aPbBackend *s_be;
static LARGE_INTEGER s_freq;
static LONGLONG s_last_flip, s_period;  /* QPC; s_period: EMA of flip gaps */

static struct {
    uint32_t frames, interpolated, cuts, overflows, unpaired_frames;
    uint64_t draws, paired, diverged, dup_draws;
    double replay_ms, replay_max;
    uint32_t shown, late, dropped;
} s_st;
static LONGLONG s_st_at;

int nv2a_interp_n(void)
{
    if (s_n < 0) {
        const char *e = getenv("RECOMP_FRAME_INTERP");
        int n = e ? atoi(e) : 1;
        if (n < 1)
            n = 1;
        if (n > INTERP_MAX_N)
            n = INTERP_MAX_N;
        s_log = getenv("RECOMP_INTERP_LOG") != NULL;
        e = getenv("RECOMP_INTERP_MODE");
        s_mode = e ? atoi(e) : 0;
        s_unique = getenv("RECOMP_INTERP_UNIQUE") != NULL;
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

/* What names a draw from one frame to the next. */
static uint64_t draw_key(void)
{
    uint64_t h = 1469598103934665603ull, prog = 0;
    uint32_t count;

    if ((g_pb.xf_mode & 3) == 2)
        nv2a_vsh_decoded(&count, &prog);
    h = fnv(h, prog);
    h = fnv(h, g_pb.attr[0].offset);
    h = fnv(h, (uint64_t)g_pb.idx_count << 32 | g_pb.prim);
    h = fnv(h, g_pb.texs[0].offset);
    /* The target by its shape, not its address: a double-buffered frame
     * draws into the other buffer each time. */
    h = fnv(h, (uint64_t)g_pb.pitch << 32 | g_pb.format);
    h = fnv(h, (uint64_t)g_pb.clip_w << 32 | g_pb.clip_h);
    return h;
}

static int all_zero(const float *v)
{
    return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f;
}

static void blend(VshConsts out, const VshConsts a, const VshConsts b, float t)
{
    uint32_t r, c;

    for (r = 0; r < NV2A_VSH_CONSTANTS; r++) {
        if (!memcmp(a[r], b[r], sizeof a[r]) || all_zero(a[r])
            || all_zero(b[r])) {
            memcpy(out[r], b[r], sizeof out[r]);
            continue;
        }
        for (c = 0; c < 4; c++) {
            float v = a[r][c] + (b[r][c] - a[r][c]) * t;
            out[r][c] = isfinite(v) ? v : b[r][c];
        }
    }
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
        if (s_rep_blend && f->pair[j] >= 0 && ctl_blends(f->part[j][0])) {
            blend(s_tmp, (const float (*)[4])s_rep_prev->consts[f->pair[j]],
                  (const float (*)[4])f->consts[j], s_rep_t);
            nv2a_vsh_constants_set_all((const float (*)[4])s_tmp);
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
                if (pt)
                    f->part = pt;
                else
                    p = NULL;
            }
            if (!k || !c || !p) {
                f->overflow = 1;
                return;
            }
            f->capdraws = cap;
        }
        f->key[f->ndraws] = draw_key();
        {
            uint64_t prog = 0;
            uint32_t count;
            if ((g_pb.xf_mode & 3) == 2)
                nv2a_vsh_decoded(&count, &prog);
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
static uint32_t s_kcap;

/* f->pair from prev; the number of draws paired. */
static uint32_t pair_frames(Frame *f, const Frame *prev)
{
    uint32_t i, a, b, paired = 0;

    for (i = 0; i < f->ndraws; i++)
        f->pair[i] = -1;
    if (!prev->ndraws || !f->ndraws)
        return 0;
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
        s_ka[i].key = f->key[i];
        s_ka[i].i = i;
    }
    for (i = 0; i < prev->ndraws; i++) {
        s_kb[i].key = prev->key[i];
        s_kb[i].i = i;
    }
    qsort(s_ka, f->ndraws, sizeof *s_ka, cmp_keyidx);
    qsort(s_kb, prev->ndraws, sizeof *s_kb, cmp_keyidx);
    for (a = b = 0; a < f->ndraws && b < prev->ndraws; ) {
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
        for (a1 = a; a1 < f->ndraws && s_ka[a1].key == s_ka[a].key; a1++)
            ;
        for (b1 = b; b1 < prev->ndraws && s_kb[b1].key == s_kb[b].key; b1++)
            ;
        if (a1 - a > 1 || b1 - b > 1)
            s_st.dup_draws += a1 - a;
        if (!(s_unique && (a1 - a > 1 || b1 - b > 1)))
            for (; a < a1 && b < b1; a++, b++) {
                f->pair[s_ka[a].i] = (int32_t)s_kb[b].i;
                paired++;
            }
        a = a1;
        b = b1;
    }
    return paired;
}

static void run_stream(const Frame *f)
{
    size_t i = 0;

    while (i < f->nwords) {
        uint32_t w = f->words[i];
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

static void replay(Frame *f, Frame *prev, float t)
{
    memcpy(&g_pb, f->start_pb, sizeof g_pb);
    nv2a_vsh_state_load(f->start_vsh);
    pb_transform_forget();
    s_rep_f = f;
    s_rep_prev = prev;
    s_rep_j = 0;
    s_rep_t = t;
    s_rep = 1;
    run_stream(f);
    s_rep = 0;
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
    fprintf(stderr, "[INTERP] 10 s: %u frames, %u interpolated, %u cuts, %u"
            " too big, %u without the frame before; %.1f draws a frame, %.1f %%"
            " paired, %.1f %% of one key drawn more than once, %llu runs went"
            " another way; replays %.2f ms a frame"
            " (most %.2f); %u pictures shown, %u late, %u dropped%c",
            s_st.frames, s_st.interpolated, s_st.cuts, s_st.overflows,
            s_st.unpaired_frames,
            s_st.frames ? (double)s_st.draws / s_st.frames : 0.0,
            s_st.draws ? 100.0 * (double)s_st.paired / (double)s_st.draws : 0.0,
            s_st.draws ? 100.0 * (double)s_st.dup_draws / (double)s_st.draws
                       : 0.0,
            (unsigned long long)s_st.diverged,
            s_st.interpolated ? s_st.replay_ms / s_st.interpolated : 0.0,
            s_st.replay_max, s_st.shown, s_st.late, s_st.dropped, 10);
    fflush(stderr);
    memset(&s_st, 0, sizeof s_st);
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
        s_st.shown++;
        s_q_next++;
    }
    InterlockedExchange(&s_q_pending, 0);
}

void nv2a_interp_tick(void)
{
    if (!s_q_pending || s_rep)
        return;
    show_due(0);
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
        char line[1024];
        int len;
        len = snprintf(line, sizeof line, "[ITRACE] %4u pair %5d prog %08X"
                       " attr0 %08X n %5u prim %X tex0 %08X target %08X:",
                       j, pj, pt[0], pt[1], pt[2], pt[3], pt[4], pt[5]);
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
    fflush(stderr);
}

void nv2a_interp_flip_before(const Nv2aPbBackend *be)
{
    Frame *f = &s_fr[s_cur], *prev = &s_fr[s_cur ^ 1];
    LONGLONG now = qpc(), t0;
    uint32_t paired, k;

    if (nv2a_interp_n() < 2 || !be->interp)
        return;
    s_be = be;
    /* What is still waiting from the frame before is too late now, but for
     * its last picture, the frame itself. */
    if (s_q_pending && s_q_next < s_q_n) {
        s_st.dropped += (uint32_t)(s_q_n - 1 - s_q_next);
        s_st.late++;
        be->interp(NV_INTERP_SHOW, s_q_slot[s_q_n - 1]);
        s_st.shown++;
        s_q_next = s_q_n;
    }
    InterlockedExchange(&s_q_pending, 0);
    if (s_last_flip) {
        LONGLONG gap = now - s_last_flip;
        if (gap > s_freq.QuadPart / 10)
            gap = s_freq.QuadPart / 10;
        s_period += (gap - s_period) / 8;
    }
    s_last_flip = now;
    stats_line(now);
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
    if (s_mode == 2)
        s_rep_blend = 0;
    ctl_read();
    {
        static const char *tr = (const char *)-1;
        static int armed = 1;
        if (tr == (const char *)-1)
            tr = getenv("RECOMP_INTERP_TRACE");
        if (tr) {
            FILE *t = fopen(tr, "rb");
            if (t) {
                fclose(t);
                if (armed)
                    trace_frame(f, prev);
                armed = 0;
            } else {
                armed = 1;
            }
        }
    }
    if (!be->interp(NV_INTERP_SAVE, 0))
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
    for (k = 1; k < (uint32_t)s_n; k++) {
        if (s_mode != 1)
            replay(f, prev, (float)k / (float)s_n);
        if (!be->interp(NV_INTERP_SAVE, (int)k))
            break;
    }
    memcpy(&g_pb, s_end_pb, sizeof g_pb);
    nv2a_vsh_state_load(s_end_vsh);
    {
        double ms = (double)(qpc() - t0) * 1000.0 / (double)s_freq.QuadPart;
        s_st.replay_ms += ms;
        if (ms > s_st.replay_max)
            s_st.replay_max = ms;
    }
    if (k < (uint32_t)s_n) {
        be->interp(NV_INTERP_LOAD, 0);  /* shown as it is */
        return;
    }
    s_st.interpolated++;
    /* The first in-between goes out now, in N's place (the back end's own
     * present follows); then the rest, N last, a period / n apart -- or
     * closer, so that N is out a little before the next frame's flip
     * (drawing these took part of this period already). */
    be->interp(NV_INTERP_LOAD, 1);
    now = qpc();
    {
        LONGLONG step = s_period / s_n;
        LONGLONG room = s_period - (now - s_last_flip)
                      - s_freq.QuadPart * 3 / 2000;
        if (room < 0)
            room = 0;
        if (step * (s_n - 1) > room)
            step = room / (s_n - 1);
        s_q_n = 0;
        for (k = 2; k <= (uint32_t)s_n; k++) {
            s_q_slot[s_q_n] = k < (uint32_t)s_n ? (int)k : 0;
            s_q_at[s_q_n] = now + step * (LONGLONG)(k - 1);
            s_q_n++;
        }
    }
    s_q_next = 0;
}

void nv2a_interp_flip_after(void)
{
    Frame *f;

    if (nv2a_interp_n() < 2 || !s_be)
        return;
    if (s_q_n && s_q_next == 0) {
        /* N back into its surface (the next frame may read it), and the
         * queue goes live. */
        s_be->interp(NV_INTERP_LOAD, 0);
        InterlockedExchange(&s_q_pending, 1);
    }
    /* Frame N+1 starts here. */
    s_cur ^= 1;
    f = &s_fr[s_cur];
    f->nwords = 0;
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
