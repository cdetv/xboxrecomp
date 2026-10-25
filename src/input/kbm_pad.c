/**
 * Keyboard and mouse as player 1's pad. See xbox_kbm.h for the interface.
 *
 * Three things make a keyboard and mouse a usable pad, beyond "key down =
 * button down":
 *
 * - A press lasts at least MIN_PRESS_MS. The title reads its pad once a
 *   frame (33 ms at 30 fps); a mouse click is often shorter than that and
 *   would be missed now and then. A wheel notch is a press of that length,
 *   with as long a gap before the next, so a quick flick of several notches
 *   arrives as that many presses.
 *
 * - The mouse is a speed, not a position. A stick held to one side keeps
 *   the camera turning, so the stick's tilt is the mouse's speed over the
 *   last MOUSE_WINDOW_MS: the title turns by tilt times frame time, which is
 *   then the distance the mouse moved, at any frame rate.
 *
 * - Tilt is asked for in the title's own range. A title ignores small tilt
 *   (dead zone) and treats tilt past some point as full; the profile says
 *   where both are (xbox_KbmStick), so a slow mouse turns slowly instead of
 *   not at all, and a fast one is not wasted past the point of full.
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
#define MOUSE_WINDOW_MS  40     /* speed measured over a bit more than one */
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
} Profile;

static SRWLOCK       s_lock = SRWLOCK_INIT;
static Profile       s_prof[XBOX_KBM_PROFILES];
static volatile LONG s_sel;
static volatile LONG s_configured;
static volatile LONG s_enabled = 1;
static volatile LONG s_active = 1;
static void (*volatile s_wake)(void);

static volatile unsigned char s_down[XBOX_KBM_INPUTS];
static volatile ULONGLONG     s_down_at[XBOX_KBM_INPUTS];
static volatile LONG          s_wheel_pending[2];
static volatile LONG          s_mdx, s_mdy;

/* Mouse counts per millisecond that tilt the right stick all the way at
 * sensitivity 1 (RECOMP_KBM_MOUSE_FULL, for tuning). */
static float mouse_full(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_KBM_MOUSE_FULL");
        v = e && atof(e) > 0.0 ? (float)atof(e) : 3.0f;
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

/* Focus lost: nothing stays held. A short press already counted keeps its
 * minimum length, which is harmless. */
void xbox_KbmReleaseAll(void)
{
    int i;
    for (i = 0; i < XBOX_KBM_INPUTS; i++)
        s_down[i] = 0;
    InterlockedExchange(&s_wheel_pending[0], 0);
    InterlockedExchange(&s_wheel_pending[1], 0);
    InterlockedExchange(&s_mdx, 0);
    InterlockedExchange(&s_mdy, 0);
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

/* The mouse's speed over the last MOUSE_WINDOW_MS, in counts per ms. The
 * window covers whole report intervals: a report every 50 ms carries 50 ms
 * of movement and is divided by 50, not 40. */
#define SAMPLES 64
static struct { ULONGLONG t; long dx, dy; } s_samp[SAMPLES];
static unsigned s_samp_n;

static void mouse_speed(ULONGLONG t, long dx, long dy, float *vx, float *vy)
{
    unsigned i, k;
    long sx = 0, sy = 0;
    ULONGLONG since = t;

    s_samp[s_samp_n % SAMPLES].t = t;
    s_samp[s_samp_n % SAMPLES].dx = dx;
    s_samp[s_samp_n % SAMPLES].dy = dy;
    s_samp_n++;
    for (k = 0; k < SAMPLES - 1 && k < s_samp_n; k++) {
        i = (s_samp_n - 1 - k) % SAMPLES;
        if (t - s_samp[i].t > MOUSE_WINDOW_MS)
            break;
        sx += s_samp[i].dx;
        sy += s_samp[i].dy;
    }
    /* The report before the oldest one counted is where the span starts. */
    if (k < s_samp_n && k < SAMPLES - 1)
        since = s_samp[(s_samp_n - 1 - k) % SAMPLES].t;
    else
        since = t - MOUSE_WINDOW_MS;
    if (t - since < MOUSE_WINDOW_MS)
        since = t - MOUSE_WINDOW_MS;
    *vx = (float)sx / (float)(t - since);
    *vy = (float)sy / (float)(t - since);
}

/* The weapon-wheel mode: the mouse as a position on the left stick. */
static float s_wx, s_wy;
static ULONGLONG s_wheel_since, s_wheel_until;

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
    dx = InterlockedExchange(&s_mdx, 0);
    dy = InterlockedExchange(&s_mdy, 0);
    if (!s_active) {
        s_wx = s_wy = 0.0f;
        s_wheel_since = s_wheel_until = 0;
        return 1;
    }

    memset(out, 0, sizeof(out));
    sel = (int)s_sel;
    AcquireSRWLockShared(&s_lock);
    if (!s_prof[sel].set) {
        ReleaseSRWLockShared(&s_lock);
        return 1;
    }
    m = s_prof[sel].mouse;
    for (i = 0; i < s_prof[sel].n; i++) {
        const xbox_KbmBinding *bd = &s_prof[sel].b[i];
        if (bd->output < XBOX_KBM_OUTPUTS && input_held(bd->input, t))
            out[bd->output] = 1;
    }
    ReleaseSRWLockShared(&s_lock);

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

    /* Weapon-wheel mode: the mouse moves a point round the left stick. */
    wheel_mode = out[XBOX_KBM_MOUSE_LSTICK];
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

    /* Right stick: look keys at full tilt, else the mouse's speed. */
    mouse_speed(t, dx, dy, &vx, &vy);
    rx = (float)(out[XBOX_KBM_RSTICK_RIGHT] - out[XBOX_KBM_RSTICK_LEFT]);
    ry = (float)(out[XBOX_KBM_RSTICK_UP] - out[XBOX_KBM_RSTICK_DOWN]);
    if (rx == 0.0f)
        rx = vx / mouse_full() * m.sens_x;
    if (ry == 0.0f)
        ry = (m.invert_y ? vy : -vy) / mouse_full() * m.sens_y;
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
int  xbox_KbmState(XBOX_INPUT_STATE *pState)
{
    memset(pState, 0, sizeof(*pState));
    return 0;
}

#endif
