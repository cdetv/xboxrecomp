/**
 * Keyboard and mouse as player 1's pad. See xbox_kbm.h for the interface.
 *
 * Four things make a keyboard and mouse a usable pad, beyond "key down =
 * button down":
 *
 * - A press lasts at least MIN_PRESS_MS. The title reads its pad once a
 *   frame (33 ms at 30 fps); a mouse click is often shorter than that and
 *   would be missed now and then. A wheel notch is a press of that length,
 *   with as long a gap before the next, so a quick flick of several notches
 *   arrives as that many presses.
 *
 * - The mouse is a turn owed, not a speed. A stick held to one side keeps
 *   the camera turning at a speed the title caps; a mouse moved fast asks
 *   for more than that. So each count of movement adds MOUSE_MS_PER_COUNT
 *   (times the sensitivity) to a balance of "milliseconds at full tilt", and
 *   the stick pays it off: tilt = balance / FRAME_MS, at most full, the
 *   balance going down by tilt times the time that passes. A small movement
 *   is a small tilt for about a frame; a big one is full tilt until the turn
 *   is done, so a flick turns as far as the mouse went, only no faster than
 *   the title allows. The balance is capped (TURN_CAP_*_MS) so a wild swing
 *   does not keep the camera spinning, and the other way cancels it.
 *   (The first version used the mouse's speed: fast movement was simply cut
 *   at full tilt and the camera turned far less than the mouse moved.)
 *   Better still is a game turning its own camera by the movement
 *   (xbox_KbmMouseDirect / xbox_KbmTakeMouse): no top speed, no lag; the
 *   stick is then only the fallback where the game cannot.
 *
 * - Tilt is asked for in the title's own range. A title ignores small tilt
 *   (dead zone) and treats tilt past some point as full; the profile says
 *   where both are (xbox_KbmStick), so a slow mouse turns slowly instead of
 *   not at all.
 *
 * - A binding can toggle (XBOX_KBM_TOGGLE): one press holds the output, the
 *   next lets it go (a head-cam held with the left trigger). Changing the
 *   profile or losing the focus lets every toggle go.
 *
 * Held keys come from the window thread one byte at a time and need no
 * lock, like the window's own key table (fb_present.c). Profiles are
 * swapped under a lock the USB thread takes once per report.
 */

#include "xbox_kbm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define MIN_PRESS_MS     70     /* about two frames at 30 fps */
#define FRAME_MS         33.0f  /* a turn owed is paid over about a frame */
#define TURN_CAP_X_MS    1000.0f  /* most full-tilt time owed, left/right */
#define TURN_CAP_Y_MS    400.0f   /* and up/down (the title stops at a limit) */
#define WHEEL_LSTICK_MS  150    /* hold before the mouse steers the left
                                 * stick: a tap stays a tap */
#define WHEEL_KEEP_MS    120    /* left stick kept after letting go, so the
                                 * title reads the choice with the release */
#define WHEEL_RADIUS     100.0f /* mouse counts to the left stick's edge */

typedef struct {
    xbox_KbmBinding b[256];
    int n;
    xbox_KbmMouse mouse;
    int set;
    unsigned char was[256];   /* toggles: the input last report */
    unsigned char latch[XBOX_KBM_OUTPUTS];   /* toggled on */
} Profile;

static SRWLOCK       s_lock = SRWLOCK_INIT;
static Profile       s_prof[XBOX_KBM_PROFILES];
static volatile LONG s_sel;
static volatile LONG s_configured;
static volatile LONG s_enabled = 1;
static volatile LONG s_active = 1;
static volatile LONG s_unlatch;   /* focus lost: toggles let go */
static volatile LONG s_direct;    /* the game takes the mouse itself */
static volatile LONG s_wheel_on;  /* a binding has the mouse on the left stick */
static void (*volatile s_wake)(void);

static volatile unsigned char s_down[XBOX_KBM_INPUTS];
static volatile ULONGLONG     s_down_at[XBOX_KBM_INPUTS];
static volatile LONG          s_wheel_pending[2];
static volatile LONG          s_mdx, s_mdy;
static volatile unsigned char s_force[XBOX_KBM_OUTPUTS];   /* held by the game */

/* Milliseconds of full tilt one mouse count is worth at sensitivity 1
 * (RECOMP_KBM_MOUSE_MS, for tuning). 0.5: at Conker's ~175 degrees a
 * second, a full turn is ~4000 counts, 13 cm at 800 dpi. */
static float mouse_ms(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_KBM_MOUSE_MS");
        v = e && atof(e) > 0.0 ? (float)atof(e) : 0.5f;
    }
    return v;
}

/* Milliseconds a turn of the spinning left stick (XBOX_KBM_LSTICK_SPIN_*;
 * RECOMP_KBM_SPIN_MS, for tuning). 300: about as fast as a thumb goes
 * round, and under half a turn a frame even at 30 fps, so the title can
 * tell the direction. */
static float spin_ms(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_KBM_SPIN_MS");
        v = e && atof(e) >= 50.0 ? (float)atof(e) : 300.0f;
    }
    return v;
}

/* Milliseconds from the performance counter: GetTickCount64 moves in 16 ms
 * steps, as coarse as the reports are frequent. */
static ULONGLONG now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / (freq.QuadPart / 1000));
}

static void wake(void)
{
    void (*w)(void) = s_wake;
    if (w)
        w();
}

void xbox_KbmSetProfile(int slot, const xbox_KbmBinding *b, int n,
                        const xbox_KbmMouse *mouse)
{
    int first;

    if (slot < 0 || slot >= XBOX_KBM_PROFILES)
        return;
    if (n < 0)
        n = 0;
    if (n > 256)
        n = 256;
    AcquireSRWLockExclusive(&s_lock);
    memcpy(s_prof[slot].b, b, (size_t)n * sizeof(*b));
    s_prof[slot].n = n;
    s_prof[slot].mouse = *mouse;
    s_prof[slot].set = 1;
    memset(s_prof[slot].was, 0, sizeof(s_prof[slot].was));
    memset(s_prof[slot].latch, 0, sizeof(s_prof[slot].latch));
    ReleaseSRWLockExclusive(&s_lock);
    first = !InterlockedExchange(&s_configured, 1);
    if (first) {
        fprintf(stderr, "[KBM] keyboard and mouse bound (profile %d, %d "
                "bindings)\n", slot, n);
        fflush(stderr);
        wake();
    }
}

void xbox_KbmSelectProfile(int slot)
{
    if (slot < 0 || slot >= XBOX_KBM_PROFILES)
        return;
    if (InterlockedExchange(&s_sel, slot) != slot) {
        InterlockedExchange(&s_unlatch, 1);
        fprintf(stderr, "[KBM] profile %d in use\n", slot);
        fflush(stderr);
    }
}

void xbox_KbmSetEnabled(int on)
{
    if (InterlockedExchange(&s_enabled, on ? 1 : 0) != (on ? 1 : 0)) {
        fprintf(stderr, "[KBM] keyboard and mouse %s\n", on ? "on" : "off");
        fflush(stderr);
        wake();
    }
}

void xbox_KbmSetActive(int on)
{
    if (InterlockedExchange(&s_active, on ? 1 : 0) != (on ? 1 : 0))
        wake();
}

int xbox_KbmConfigured(void)
{
    return s_configured && s_enabled;
}

int xbox_KbmWantsMouse(void)
{
    return s_configured && s_enabled && s_active;
}

void xbox_KbmSetWake(void (*fn)(void))
{
    s_wake = fn;
}

void xbox_KbmKey(unsigned input, int down)
{
    if (input >= XBOX_KBM_INPUTS)
        return;
    /* Auto-repeat sends more downs: the press began at the first. */
    if (down && !s_down[input])
        s_down_at[input] = now_ms();
    s_down[input] = (unsigned char)(down ? 1 : 0);
}

void xbox_KbmWheel(int notches)
{
    if (notches > 0)
        InterlockedExchangeAdd(&s_wheel_pending[0], notches);
    else if (notches < 0)
        InterlockedExchangeAdd(&s_wheel_pending[1], -notches);
}

void xbox_KbmMouseMove(long dx, long dy)
{
    InterlockedExchangeAdd(&s_mdx, dx);
    InterlockedExchangeAdd(&s_mdy, dy);
}

void xbox_KbmMouseDirect(int axes)
{
    InterlockedExchange(&s_direct, axes & (XBOX_KBM_DIRECT_X
                                           | XBOX_KBM_DIRECT_Y));
}

int xbox_KbmTakeMouse(long *dx, long *dy)
{
    LONG axes = s_direct;

    *dx = *dy = 0;
    if (!s_configured || !s_enabled || !s_active || s_wheel_on) {
        if (!s_wheel_on) {
            if (axes & XBOX_KBM_DIRECT_X)
                InterlockedExchange(&s_mdx, 0);
            if (axes & XBOX_KBM_DIRECT_Y)
                InterlockedExchange(&s_mdy, 0);
        }
        return 0;
    }
    if (axes & XBOX_KBM_DIRECT_X)
        *dx = InterlockedExchange(&s_mdx, 0);
    if (axes & XBOX_KBM_DIRECT_Y)
        *dy = InterlockedExchange(&s_mdy, 0);
    return axes != 0;
}

int xbox_KbmHeld(unsigned input)
{
    if (!s_configured || !s_enabled || !s_active
        || input >= XBOX_KBM_WHEEL_UP)
        return 0;
    return s_down[input] || (s_down_at[input]
                             && now_ms() - s_down_at[input] < MIN_PRESS_MS);
}

void xbox_KbmForce(unsigned output, int on)
{
    if (output < XBOX_KBM_OUTPUTS)
        s_force[output] = (unsigned char)(on ? 1 : 0);
}

/* Focus lost: nothing stays held, toggles included. A short press already
 * counted keeps its minimum length, which is harmless. */
void xbox_KbmReleaseAll(void)
{
    int i;
    for (i = 0; i < XBOX_KBM_INPUTS; i++)
        s_down[i] = 0;
    InterlockedExchange(&s_wheel_pending[0], 0);
    InterlockedExchange(&s_wheel_pending[1], 0);
    InterlockedExchange(&s_mdx, 0);
    InterlockedExchange(&s_mdy, 0);
    InterlockedExchange(&s_unlatch, 1);
}

/* ---- USB thread ----------------------------------------------------------- */

/* A wheel notch as a press: MIN_PRESS_MS down, then as long up. */
static ULONGLONG s_wheel_start[2];

static int wheel_held(int i, ULONGLONG t)
{
    if (s_wheel_start[i] && t - s_wheel_start[i] < MIN_PRESS_MS)
        return 1;
    if (s_wheel_start[i] && t - s_wheel_start[i] < 2 * MIN_PRESS_MS)
        return 0;
    s_wheel_start[i] = 0;
    if (s_wheel_pending[i] > 0) {
        InterlockedDecrement(&s_wheel_pending[i]);
        s_wheel_start[i] = t;
        return 1;
    }
    return 0;
}

static int input_held(unsigned input, ULONGLONG t)
{
    if (input == XBOX_KBM_WHEEL_UP)
        return wheel_held(0, t);
    if (input == XBOX_KBM_WHEEL_DOWN)
        return wheel_held(1, t);
    if (input >= XBOX_KBM_INPUTS)
        return 0;
    return s_down[input] || (s_down_at[input] && t - s_down_at[input]
                                                 < MIN_PRESS_MS);
}

/* 0-1 of the title's range (signed) to a raw axis value. */
static SHORT axis(float v, const xbox_KbmStick *st)
{
    float a = fabsf(v), raw;
    if (a < 0.001f)
        return 0;
    if (a > 1.0f)
        a = 1.0f;
    raw = (st->dead + a * (st->full - st->dead)) * 32767.0f;
    if (raw > 32767.0f)
        raw = 32767.0f;
    return (SHORT)(v < 0.0f ? -raw : raw);
}

/* The turn owed, per axis, in milliseconds at full tilt (signed), and the
 * tilt last reported, which has been paying it off since s_turn_t. */
static float s_owe[2], s_tilt[2];
static ULONGLONG s_turn_t;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Pay off the time since the last report at the tilt it gave, add the new
 * movement (already in milliseconds), and choose the tilt until the next. */
static void mouse_turn(ULONGLONG t, float add_x, float add_y, float *tx,
                       float *ty)
{
    static const float cap[2] = { TURN_CAP_X_MS, TURN_CAP_Y_MS };
    float add[2], el = s_turn_t ? (float)(t - s_turn_t) : 0.0f;
    int i;

    add[0] = add_x;
    add[1] = add_y;
    s_turn_t = t;
    for (i = 0; i < 2; i++) {
        float paid = s_tilt[i] * el;
        /* Never pay past zero: the stick does not turn back. */
        if ((s_owe[i] > 0.0f && paid > s_owe[i])
            || (s_owe[i] < 0.0f && paid < s_owe[i]))
            paid = s_owe[i];
        s_owe[i] = clampf(s_owe[i] - paid + add[i], -cap[i], cap[i]);
        s_tilt[i] = clampf(s_owe[i] / FRAME_MS, -1.0f, 1.0f);
    }
    *tx = s_tilt[0];
    *ty = s_tilt[1];
}

/* The weapon-wheel mode: the mouse as a position on the left stick. */
static float s_wx, s_wy;
static ULONGLONG s_wheel_since, s_wheel_until;

/* The spinning left stick: its angle (turns, 0 = up) and when it was last
 * moved; 0 while no spin output is held, so each spin starts at the top. */
static float s_spin_a;
static ULONGLONG s_spin_t;

int xbox_KbmState(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    unsigned char out[XBOX_KBM_OUTPUTS];
    xbox_KbmMouse m;
    ULONGLONG t = now_ms();
    long dx, dy;
    float lx, ly, rx, ry, vx, vy;
    int i, sel, wheel_mode, wheel_show;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));
    pState->dwPacketNumber = ++packet;
    if (!s_configured || !s_enabled)
        return 0;
    if (!s_active) {
        InterlockedExchange(&s_mdx, 0);
        InterlockedExchange(&s_mdy, 0);
        s_wx = s_wy = 0.0f;
        s_wheel_since = s_wheel_until = 0;
        s_owe[0] = s_owe[1] = s_tilt[0] = s_tilt[1] = 0.0f;
        s_turn_t = 0;
        s_spin_a = 0.0f;
        s_spin_t = 0;
        return 1;
    }

    memset(out, 0, sizeof(out));
    sel = (int)s_sel;
    /* Exclusive: toggles write the profile's own state. */
    AcquireSRWLockExclusive(&s_lock);
    if (!s_prof[sel].set) {
        ReleaseSRWLockExclusive(&s_lock);
        return 1;
    }
    if (InterlockedExchange(&s_unlatch, 0))
        for (i = 0; i < XBOX_KBM_PROFILES; i++)
            memset(s_prof[i].latch, 0, sizeof(s_prof[i].latch));
    m = s_prof[sel].mouse;
    for (i = 0; i < s_prof[sel].n; i++) {
        const xbox_KbmBinding *bd = &s_prof[sel].b[i];
        unsigned o = bd->output & ~XBOX_KBM_TOGGLE;
        int held;
        if (o >= XBOX_KBM_OUTPUTS)
            continue;
        held = input_held(bd->input, t);
        if (bd->output & XBOX_KBM_TOGGLE) {
            if (held && !s_prof[sel].was[i])
                s_prof[sel].latch[o] ^= 1;
            s_prof[sel].was[i] = (unsigned char)held;
        } else if (held) {
            out[o] = 1;
        }
    }
    for (i = 0; i < XBOX_KBM_OUTPUTS; i++)
        out[i] |= s_prof[sel].latch[i] | s_force[i];
    ReleaseSRWLockExclusive(&s_lock);

    /* Analog buttons fully pressed: a title reading a pressure never sees a
     * press at 1. */
    for (i = 0; i < 8; i++)
        if (out[XBOX_KBM_A + i])
            pState->Gamepad.bAnalogButtons[i] = 255;
    if (out[XBOX_KBM_DPAD_UP])    b |= XBOX_GAMEPAD_DPAD_UP;
    if (out[XBOX_KBM_DPAD_DOWN])  b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (out[XBOX_KBM_DPAD_LEFT])  b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (out[XBOX_KBM_DPAD_RIGHT]) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (out[XBOX_KBM_START])      b |= XBOX_GAMEPAD_START;
    if (out[XBOX_KBM_BACK])       b |= XBOX_GAMEPAD_BACK;
    if (out[XBOX_KBM_LTHUMB])     b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (out[XBOX_KBM_RTHUMB])     b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Left stick from keys: full tilt as the raw edge (the title clamps and
     * shapes its own diagonals), walking as a tilt in the title's range, the
     * same on a diagonal as straight. */
    lx = (float)(out[XBOX_KBM_LSTICK_RIGHT] - out[XBOX_KBM_LSTICK_LEFT]);
    ly = (float)(out[XBOX_KBM_LSTICK_UP] - out[XBOX_KBM_LSTICK_DOWN]);
    if (out[XBOX_KBM_WALK] && (lx != 0.0f || ly != 0.0f)) {
        float k = m.walk / sqrtf(lx * lx + ly * ly);
        pState->Gamepad.sThumbLX = axis(lx * k, &m.left);
        pState->Gamepad.sThumbLY = axis(ly * k, &m.left);
    } else {
        pState->Gamepad.sThumbLX = (SHORT)(lx * 32767.0f);
        pState->Gamepad.sThumbLY = (SHORT)(ly * 32767.0f);
    }

    /* A spin output held (both: none) takes the left stick round its edge
     * at full tilt, one turn per spin_ms. */
    i = out[XBOX_KBM_LSTICK_SPIN_CW] - out[XBOX_KBM_LSTICK_SPIN_CCW];
    if (i) {
        float a;
        if (s_spin_t)
            s_spin_a += (float)(t - s_spin_t) / spin_ms();
        s_spin_a -= floorf(s_spin_a);
        s_spin_t = t;
        a = s_spin_a * 6.2831853f;
        pState->Gamepad.sThumbLX = (SHORT)((float)i * sinf(a) * 32767.0f);
        pState->Gamepad.sThumbLY = (SHORT)(cosf(a) * 32767.0f);
    } else {
        s_spin_a = 0.0f;
        s_spin_t = 0;
    }

    /* The mouse's movement is ours unless the game takes it (direct mouse
     * look); a weapon wheel takes it back while its key is held. */
    wheel_mode = out[XBOX_KBM_MOUSE_LSTICK];
    InterlockedExchange(&s_wheel_on, wheel_mode);
    dx = !(s_direct & XBOX_KBM_DIRECT_X) || wheel_mode
         ? InterlockedExchange(&s_mdx, 0) : 0;
    dy = !(s_direct & XBOX_KBM_DIRECT_Y) || wheel_mode
         ? InterlockedExchange(&s_mdy, 0) : 0;

    /* Weapon-wheel mode: the mouse moves a point round the left stick. */
    if (wheel_mode) {
        float r = WHEEL_RADIUS / (m.sens_x > 0.05f ? m.sens_x : 0.05f);
        float len;
        if (!s_wheel_since)
            s_wheel_since = t;
        s_wx += (float)dx / r;
        s_wy -= (float)dy / r;
        len = sqrtf(s_wx * s_wx + s_wy * s_wy);
        if (len > 1.0f) {
            s_wx /= len;
            s_wy /= len;
        }
        s_wheel_until = t + WHEEL_KEEP_MS;
        dx = dy = 0;
    } else if (s_wheel_since && t >= s_wheel_until) {
        s_wx = s_wy = 0.0f;
        s_wheel_since = 0;
    }
    wheel_show = s_wheel_since && t - s_wheel_since >= WHEEL_LSTICK_MS
                 && (s_wx != 0.0f || s_wy != 0.0f);
    if (wheel_show) {
        pState->Gamepad.sThumbLX = axis(s_wx, &m.left);
        pState->Gamepad.sThumbLY = axis(s_wy, &m.left);
    }

    /* Right stick: look keys at full tilt, else the turn the mouse owes. */
    mouse_turn(t, (float)dx * mouse_ms() * m.sens_x,
               (float)(m.invert_y ? dy : -dy) * mouse_ms() * m.sens_y,
               &vx, &vy);
    rx = (float)(out[XBOX_KBM_RSTICK_RIGHT] - out[XBOX_KBM_RSTICK_LEFT]);
    ry = (float)(out[XBOX_KBM_RSTICK_UP] - out[XBOX_KBM_RSTICK_DOWN]);
    if (rx == 0.0f)
        rx = vx;
    if (ry == 0.0f)
        ry = vy;
    pState->Gamepad.sThumbRX = axis(rx, &m.right);
    pState->Gamepad.sThumbRY = axis(ry, &m.right);
    return 1;
}

#else /* !_WIN32: the window side is Windows-only so far */

void xbox_KbmSetProfile(int slot, const xbox_KbmBinding *b, int n,
                        const xbox_KbmMouse *mouse)
{ (void)slot; (void)b; (void)n; (void)mouse; }
void xbox_KbmSelectProfile(int slot) { (void)slot; }
void xbox_KbmSetEnabled(int on) { (void)on; }
void xbox_KbmSetActive(int on) { (void)on; }
int  xbox_KbmConfigured(void) { return 0; }
int  xbox_KbmWantsMouse(void) { return 0; }
void xbox_KbmSetWake(void (*wake)(void)) { (void)wake; }
void xbox_KbmKey(unsigned input, int down) { (void)input; (void)down; }
void xbox_KbmWheel(int notches) { (void)notches; }
void xbox_KbmMouseMove(long dx, long dy) { (void)dx; (void)dy; }
void xbox_KbmReleaseAll(void) {}
int  xbox_KbmHeld(unsigned input) { (void)input; return 0; }
void xbox_KbmForce(unsigned output, int on) { (void)output; (void)on; }
int  xbox_KbmState(XBOX_INPUT_STATE *pState)
{
    memset(pState, 0, sizeof(*pState));
    return 0;
}

#endif
