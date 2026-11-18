/**
 * PlayStation pads through SDL3 (Windows)
 *
 * XInput only knows Xbox pads, so a DualShock 4 or DualSense reached the
 * title only through DS4Windows or Steam Input, which turn it into a
 * virtual Xbox 360 pad. SDL3's HIDAPI drivers read both directly, over USB
 * and Bluetooth. These pads join XInput's four as host pads 4-7
 * (xinput_device.c), in XInput's own shape, so everything above that layer
 * (port assignment, the host's menus) treats them as one more controller.
 *
 * SDL is driven from one thread of its own: it opens and closes pads as
 * they come and go and copies each one's state into a snapshot under a
 * lock. Everyone else reads the snapshots, so SDL is never called from the
 * USB model's, the executor's or the game's threads.
 *
 * Built only when the game project hands the toolkit an SDL3 target
 * (XBOXRECOMP_SDL3_TARGET, src/input/CMakeLists.txt); without one the
 * stubs at the end report no pads.
 */

#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>
#include "sdl_pads.h"

#if defined(XBOX_INPUT_SDL3)

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include "gyro_motion.h"

typedef struct {
    SDL_Gamepad  *pad;          /* SDL thread only */
    SDL_JoystickID id;          /* SDL thread only */
    BOOL          connected;
    XINPUT_STATE  state;
    char          name[96];
    LONG          rumble_sent;  /* SDL thread only: last speeds given SDL */
    ULONGLONG     rumble_at;    /* SDL thread only: when */
    /* Gyro (SDL thread only): the library's object (calibration, gravity),
     * the last accelerometer sample (g), the last gyro sample's sensor time
     * (ns), the sensor's rate, soft smoothing's average (deg/s), a hand
     * calibration's end (tick) and the automatic calibration as set. */
    GyroMotion   *motion;
    float         accel[3];
    Uint64        gyro_ts;
    float         gyro_hz;
    float         smooth[2];
    ULONGLONG     cal_until;
    int           cal_auto;
    unsigned      samples;      /* for RECOMP_GYRO_LOG */
    /* Gyro, under g_lock: has one; degrees turned since the last take (yaw
     * right positive, pitch front-up positive) and when that was; the last
     * speed (deg/s, same signs) and state flags, for a host's meter. */
    BOOL          gyro;
    double        turn[2];
    ULONGLONG     taken_at;
    float         speed[2];
    int           gyro_flags;
} SdlPad;

static SdlPad  g_pads[SDL_PADS_MAX];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_mask;    /* bit n: pad n connected */

/* Gyro requests from other threads: bit n = calibrate pad n by hand; the
 * automatic calibration on (1, default) or off. */
static volatile LONG g_cal_req;
static volatile LONG g_gyro_auto = 1;
#define GYRO_CAL_MS   2000      /* a hand calibration: the pad lies still */
#define GYRO_STALE_MS 250       /* turns older than this are thrown away */
#define RAD2DEG       57.29577951f

/* The speeds the title wants on pad n, low << 16 | high. XInput's motors
 * run until told otherwise, SDL's only for a given time: the SDL thread
 * hands SDL each change, and repeats a running one before it runs out. If
 * the process dies without stopping them, they stop by themselves. */
static volatile LONG g_rumble[SDL_PADS_MAX];
#define RUMBLE_MS   2000
#define RUMBLE_KEEP 1000

/* Only PlayStation and Nintendo Switch pads (a Switch Pro Controller, or a
 * pair of Joy-Cons, which SDL joins into one by default): an Xbox pad is
 * XInput's already, and SDL's own XInput, Raw Input, WGI and DirectInput
 * back ends are off (sdl_thread), so the same pad never shows up twice.
 * Buttons go by position (read_pad), so a Switch pad's bottom button (its
 * "B") is the Xbox A, as the hands expect. */
static BOOL wanted(SDL_JoystickID id)
{
    SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
    return t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5
        || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
        || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
}

static void pad_added(SDL_JoystickID id)
{
    SDL_Gamepad *gp;
    int n;

    if (!wanted(id))
        return;
    for (n = 0; n < SDL_PADS_MAX; n++)
        if (g_pads[n].pad && g_pads[n].id == id)
            return;
    for (n = 0; n < SDL_PADS_MAX && g_pads[n].pad; n++)
        ;
    if (n == SDL_PADS_MAX)
        return;
    gp = SDL_OpenGamepad(id);
    if (!gp) {
        fprintf(stderr, "[INPUT] SDL: could not open \"%s\": %s\n",
                SDL_GetGamepadNameForID(id), SDL_GetError());
        return;
    }
    /* The gyro needs the accelerometer too (gravity: player space and the
     * automatic calibration). Over Bluetooth a PlayStation pad sends them
     * only in "enhanced reports", which SDL turns on by default. */
    {
        GyroMotion *m = NULL;
        float hz = 0.0f;
        if (SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO)
            && SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL)
            && SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true)
            && SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true))
            m = gyro_motion_new();
        if (m)
            hz = SDL_GetGamepadSensorDataRate(gp, SDL_SENSOR_GYRO);
        InterlockedAnd(&g_cal_req, ~(1L << n));
        g_pads[n].motion = m;
        g_pads[n].gyro_hz = hz;
        g_pads[n].gyro_ts = 0;
        g_pads[n].cal_until = 0;
        g_pads[n].cal_auto = 1;
        g_pads[n].samples = 0;
        memset(g_pads[n].accel, 0, sizeof(g_pads[n].accel));
        memset(g_pads[n].smooth, 0, sizeof(g_pads[n].smooth));
    }
    AcquireSRWLockExclusive(&g_lock);
    g_pads[n].pad = gp;
    g_pads[n].id = id;
    g_pads[n].connected = TRUE;
    g_pads[n].rumble_sent = 0;
    memset(&g_pads[n].state, 0, sizeof(g_pads[n].state));
    snprintf(g_pads[n].name, sizeof(g_pads[n].name), "%s",
             SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "");
    g_pads[n].gyro = g_pads[n].motion != NULL;
    g_pads[n].turn[0] = g_pads[n].turn[1] = 0.0;
    g_pads[n].speed[0] = g_pads[n].speed[1] = 0.0f;
    g_pads[n].gyro_flags = 0;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedOr(&g_mask, 1L << n);
    fprintf(stderr, "[INPUT] SDL: controller %d: \"%s\" (%s)\n",
            n + 1, g_pads[n].name,
            SDL_GetGamepadConnectionState(gp) == SDL_JOYSTICK_CONNECTION_WIRELESS
                ? "wireless" : "wired");
    if (g_pads[n].motion)
        fprintf(stderr, "[INPUT] SDL: controller %d gyro on (%.0f samples a "
                "second)\n", n + 1, g_pads[n].gyro_hz);
    else
        fprintf(stderr, "[INPUT] SDL: controller %d has no gyro\n", n + 1);
}

static void pad_removed(SDL_JoystickID id)
{
    int n;
    for (n = 0; n < SDL_PADS_MAX; n++)
        if (g_pads[n].pad && g_pads[n].id == id) {
            InterlockedAnd(&g_mask, ~(1L << n));
            AcquireSRWLockExclusive(&g_lock);
            g_pads[n].connected = FALSE;
            g_pads[n].gyro = FALSE;
            ReleaseSRWLockExclusive(&g_lock);
            SDL_CloseGamepad(g_pads[n].pad);
            g_pads[n].pad = NULL;
            gyro_motion_free(g_pads[n].motion);
            g_pads[n].motion = NULL;
            fprintf(stderr, "[INPUT] SDL: controller %d gone\n", n + 1);
        }
}

/* SDL's -32768..32767 with down positive, to XInput's up positive. */
static SHORT stick_y(Sint16 v)
{
    return v == -32768 ? 32767 : (SHORT)-v;
}

static BYTE trigger(Sint16 v)
{
    return v <= 0 ? 0 : (BYTE)((v * 255 + 16383) / 32767);
}

/* XInput's layout for an Xbox pad, by position: Cross is A, Circle B,
 * Square X, Triangle Y, L1/R1 the shoulders (the title's White/Black),
 * Share/Create is Back and Options Start. The touchpad click is Back too:
 * Share sits where it is hard to reach in a hurry. The PS button is left
 * alone (Windows and Steam use it). On a Switch pad the same positions: its
 * B (bottom) is A, A (right) B, Y (left) X, X (top) Y, L/R the shoulders,
 * ZL/ZR the triggers (all or nothing), - Back, + Start; Home and Capture
 * are left alone. */
static void read_pad(SDL_Gamepad *gp, XINPUT_GAMEPAD *g)
{
    static const struct { SDL_GamepadButton b; WORD bit; } map[] = {
        { SDL_GAMEPAD_BUTTON_DPAD_UP,        XINPUT_GAMEPAD_DPAD_UP },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      XINPUT_GAMEPAD_DPAD_LEFT },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     XINPUT_GAMEPAD_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_START,          XINPUT_GAMEPAD_START },
        { SDL_GAMEPAD_BUTTON_BACK,           XINPUT_GAMEPAD_BACK },
        { SDL_GAMEPAD_BUTTON_TOUCHPAD,       XINPUT_GAMEPAD_BACK },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK,     XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_SOUTH,          XINPUT_GAMEPAD_A },
        { SDL_GAMEPAD_BUTTON_EAST,           XINPUT_GAMEPAD_B },
        { SDL_GAMEPAD_BUTTON_WEST,           XINPUT_GAMEPAD_X },
        { SDL_GAMEPAD_BUTTON_NORTH,          XINPUT_GAMEPAD_Y },
    };
    size_t i;

    memset(g, 0, sizeof(*g));
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (SDL_GetGamepadButton(gp, map[i].b))
            g->wButtons |= map[i].bit;
    g->bLeftTrigger  = trigger(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    g->bRightTrigger = trigger(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    g->sThumbLX = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
    g->sThumbLY = stick_y(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    g->sThumbRX = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX);
    g->sThumbRY = stick_y(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
}

/* Soft smoothing (GyroWiki, "soft tiered smoothing"): turns slower than
 * RECOMP_GYRO_SMOOTH degrees a second (default 3; 0 = off) are averaged
 * over about 1/8 s, which takes the hand's shake out of a held aim; from
 * half of it up the share averaged fades out, so real turns keep no lag. */
static float gyro_smooth_dps(void)
{
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("RECOMP_GYRO_SMOOTH");
        v = e && *e ? (float)atof(e) : 3.0f;
        if (v < 0.0f)
            v = 0.0f;
    }
    return v;
}

static int gyro_log(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_GYRO_LOG");
        v = e && *e ? atoi(e) : 0;
    }
    return v;
}

static void soft_smooth(SdlPad *p, float v[2], float dt)
{
    float top = gyro_smooth_dps(), low = top * 0.5f, w, k;
    float mag = sqrtf(v[0] * v[0] + v[1] * v[1]);
    int i;
    if (top <= 0.0f)
        return;
    w = (mag - low) / (top - low);
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    k = 1.0f - expf(-dt / 0.06f);
    for (i = 0; i < 2; i++) {
        p->smooth[i] += (v[i] * (1.0f - w) - p->smooth[i]) * k;
        v[i] = v[i] * w + p->smooth[i];
    }
}

/* One sensor sample (SDL thread). SDL sends the gyro (rad/s) and the
 * accelerometer (m/s^2) of a report as two events; each gyro sample goes to
 * the library with the latest accelerometer sample, over the time since
 * the previous gyro sample by the pad's own clock, so the turn adds up to
 * what the hand did whatever the frame rate. */
static void gyro_sample(const SDL_GamepadSensorEvent *e)
{
    SdlPad *p = NULL;
    float g[3], v[2], dt, pitch, yaw;
    int n, i;

    for (n = 0; n < SDL_PADS_MAX; n++)
        if (g_pads[n].pad && g_pads[n].id == e->which && g_pads[n].motion) {
            p = &g_pads[n];
            break;
        }
    if (!p)
        return;
    if (e->sensor == SDL_SENSOR_ACCEL) {
        for (i = 0; i < 3; i++)
            p->accel[i] = e->data[i] / SDL_STANDARD_GRAVITY;
        return;
    }
    if (e->sensor != SDL_SENSOR_GYRO)
        return;
    dt = p->gyro_ts && e->sensor_timestamp > p->gyro_ts
       ? (float)((double)(e->sensor_timestamp - p->gyro_ts) * 1e-9) : 0.0f;
    p->gyro_ts = e->sensor_timestamp;
    /* The first sample, a clock jump or a long gap (a Bluetooth hiccup):
     * one sample's time, never a big step. */
    if (dt <= 0.0f || dt > 0.05f)
        dt = p->gyro_hz > 0.0f ? 1.0f / p->gyro_hz : 0.004f;
    for (i = 0; i < 3; i++)
        g[i] = e->data[i] * RAD2DEG;
    gyro_motion_add(p->motion, g, p->accel, dt);
    gyro_motion_player(p->motion, &pitch, &yaw);
    v[0] = -yaw;                /* the library's yaw is left positive */
    v[1] = pitch;
    if (p->cal_until)
        v[0] = v[1] = 0.0f;     /* calibrating: the pad should not turn */
    soft_smooth(p, v, dt);
    p->samples++;
    AcquireSRWLockExclusive(&g_lock);
    p->turn[0] += v[0] * dt;
    p->turn[1] += v[1] * dt;
    p->speed[0] = v[0];
    p->speed[1] = v[1];
    ReleaseSRWLockExclusive(&g_lock);
}

/* Once a poll (SDL thread): hand calibrations asked for and ended, the
 * automatic calibration switched, the state for a meter, the log. */
static void gyro_tick(int n)
{
    SdlPad *p = &g_pads[n];
    ULONGLONG now = GetTickCount64();
    float off[3], conf;
    int steady, flags, want;
    static ULONGLONG logged[SDL_PADS_MAX];

    if (!p->motion)
        return;
    if (InterlockedAnd(&g_cal_req, ~(1L << n)) & (1L << n)) {
        gyro_motion_calibrate(p->motion, 1);
        p->cal_until = now + GYRO_CAL_MS;
        fprintf(stderr, "[GYRO] controller %d: calibrating (lay it still)\n",
                n + 1);
    } else if (p->cal_until && now >= p->cal_until) {
        gyro_motion_calibrate(p->motion, 0);
        p->cal_until = 0;
        gyro_motion_state(p->motion, off, &conf, &steady);
        fprintf(stderr, "[GYRO] controller %d: calibrated, bias %.2f %.2f "
                "%.2f deg/s\n", n + 1, off[0], off[1], off[2]);
    }
    want = InterlockedCompareExchange(&g_gyro_auto, 0, 0) != 0;
    if (want != p->cal_auto) {
        gyro_motion_auto(p->motion, want);
        p->cal_auto = want;
    }
    gyro_motion_state(p->motion, off, &conf, &steady);
    flags = (p->cal_until ? 1 : 0) | (steady ? 2 : 0);
    AcquireSRWLockExclusive(&g_lock);
    p->gyro_flags = flags;
    ReleaseSRWLockExclusive(&g_lock);
    if (gyro_log() && now - logged[n] >= 2000) {
        fprintf(stderr, "[GYRO] controller %d: %u samples in %.1f s, speed "
                "yaw %.1f pitch %.1f deg/s, bias %.2f %.2f %.2f, confidence "
                "%.2f%s%s\n", n + 1, p->samples,
                logged[n] ? (double)(now - logged[n]) / 1000.0 : 0.0,
                p->speed[0], p->speed[1], off[0], off[1], off[2], conf,
                steady ? ", still" : "", p->cal_until ? ", calibrating" : "");
        logged[n] = now;
        p->samples = 0;
    }
}

static DWORD WINAPI sdl_thread(LPVOID arg)
{
    HANDLE timer;
    LARGE_INTEGER due;
    (void)arg;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Read while the window is in the background, as XInput is. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* HIDAPI only: the other back ends would bring Xbox pads (XInput's
     * already) or a second copy of a PlayStation pad. */
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[INPUT] SDL: no PlayStation / Switch controllers "
                "(%s)\n", SDL_GetError());
        return 0;
    }
    fprintf(stderr, "[INPUT] SDL %d.%d.%d: PlayStation / Switch controllers "
            "on\n",
            SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
            SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
            SDL_VERSIONNUM_MICRO(SDL_GetVersion()));

    /* Every 2 ms: a DualShock 4 reports every 4 ms on USB (faster on
     * Bluetooth). A high-resolution timer, so the wait does not depend on
     * the process's timer resolution (Sleep(2) can be 15.6 ms). */
    timer = CreateWaitableTimerExW(NULL, NULL,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -20000;          /* 100 ns units */

    for (;;) {
        SDL_Event e;
        int n;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                pad_added(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
                pad_removed(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE)
                gyro_sample(&e.gsensor);
        }
        for (n = 0; n < SDL_PADS_MAX; n++) {
            XINPUT_GAMEPAD g;
            LONG want;
            if (!g_pads[n].pad)
                continue;
            gyro_tick(n);
            want = InterlockedCompareExchange(&g_rumble[n], 0, 0);
            if (want != g_pads[n].rumble_sent
                || (want && GetTickCount64() - g_pads[n].rumble_at >= RUMBLE_KEEP)) {
                SDL_RumbleGamepad(g_pads[n].pad, (Uint16)((ULONG)want >> 16),
                                  (Uint16)(want & 0xFFFF), want ? RUMBLE_MS : 0);
                g_pads[n].rumble_sent = want;
                g_pads[n].rumble_at = GetTickCount64();
            }
            read_pad(g_pads[n].pad, &g);
            if (memcmp(&g, &g_pads[n].state.Gamepad, sizeof(g))) {
                AcquireSRWLockExclusive(&g_lock);
                g_pads[n].state.Gamepad = g;
                g_pads[n].state.dwPacketNumber++;
                ReleaseSRWLockExclusive(&g_lock);
            }
        }
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(2);
    }
}

void xbox_SdlPadsInit(void)
{
    static LONG started;
    HANDLE t;
    if (InterlockedExchange(&started, 1))
        return;
    t = CreateThread(NULL, 0, sdl_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}

unsigned xbox_SdlPadsConnected(void)
{
    return (unsigned)g_mask;
}

DWORD xbox_SdlPadGetState(int n, XINPUT_STATE *st)
{
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
    if (n < 0 || n >= SDL_PADS_MAX)
        return rc;
    AcquireSRWLockShared(&g_lock);
    if (g_pads[n].connected) {
        *st = g_pads[n].state;
        rc = ERROR_SUCCESS;
    }
    ReleaseSRWLockShared(&g_lock);
    return rc;
}

int xbox_SdlPadName(int n, char *out, int size)
{
    if (size <= 0)
        return 0;
    out[0] = '\0';
    if (n < 0 || n >= SDL_PADS_MAX)
        return 0;
    AcquireSRWLockShared(&g_lock);
    if (g_pads[n].connected)
        snprintf(out, (size_t)size, "%s", g_pads[n].name);
    ReleaseSRWLockShared(&g_lock);
    return out[0] != '\0';
}

void xbox_SdlPadRumble(int n, WORD low, WORD high)
{
    if (n < 0 || n >= SDL_PADS_MAX)
        return;
    InterlockedExchange(&g_rumble[n], (LONG)(((ULONG)low << 16) | high));
}

/* Called on the way out, from whichever thread ends the process; SDL's
 * gamepad calls take SDL's joystick lock, so this is safe beside its
 * thread. ExitProcess does not wait for that thread to get there. */
void xbox_SdlPadsStopRumble(void)
{
    int n;
    for (n = 0; n < SDL_PADS_MAX; n++) {
        InterlockedExchange(&g_rumble[n], 0);
        if (g_pads[n].pad && g_pads[n].rumble_sent)
            SDL_RumbleGamepad(g_pads[n].pad, 0, 0, 0);
    }
}

int xbox_SdlPadGyroTake(int n, float *yaw, float *pitch)
{
    int has = 0;
    *yaw = *pitch = 0.0f;
    if (n < 0 || n >= SDL_PADS_MAX)
        return 0;
    AcquireSRWLockExclusive(&g_lock);
    if (g_pads[n].connected && g_pads[n].gyro) {
        ULONGLONG now = GetTickCount64();
        has = 1;
        if (now - g_pads[n].taken_at <= GYRO_STALE_MS) {
            *yaw = (float)g_pads[n].turn[0];
            *pitch = (float)g_pads[n].turn[1];
        }
        g_pads[n].turn[0] = g_pads[n].turn[1] = 0.0;
        g_pads[n].taken_at = now;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return has;
}

int xbox_SdlPadGyroRate(int n, float *yaw, float *pitch)
{
    int flags = 0;
    *yaw = *pitch = 0.0f;
    if (n < 0 || n >= SDL_PADS_MAX)
        return 0;
    AcquireSRWLockShared(&g_lock);
    if (g_pads[n].connected && g_pads[n].gyro) {
        *yaw = g_pads[n].speed[0];
        *pitch = g_pads[n].speed[1];
        flags = XBOX_GYRO_HAS | (g_pads[n].gyro_flags & 1 ? XBOX_GYRO_CALIBRATING : 0)
              | (g_pads[n].gyro_flags & 2 ? XBOX_GYRO_STILL : 0);
    }
    ReleaseSRWLockShared(&g_lock);
    return flags;
}

void xbox_SdlPadGyroCalibrate(int n)
{
    if (n >= 0 && n < SDL_PADS_MAX)
        InterlockedOr(&g_cal_req, 1L << n);
}

void xbox_SdlPadGyroAuto(int on)
{
    InterlockedExchange(&g_gyro_auto, on ? 1 : 0);
}

#else /* !XBOX_INPUT_SDL3 */

void xbox_SdlPadsInit(void) {}
unsigned xbox_SdlPadsConnected(void) { return 0; }
DWORD xbox_SdlPadGetState(int n, XINPUT_STATE *st)
{
    (void)n; (void)st;
    return ERROR_DEVICE_NOT_CONNECTED;
}
int xbox_SdlPadName(int n, char *out, int size)
{
    (void)n;
    if (size > 0)
        out[0] = '\0';
    return 0;
}
void xbox_SdlPadRumble(int n, WORD low, WORD high)
{
    (void)n; (void)low; (void)high;
}
void xbox_SdlPadsStopRumble(void) {}
int xbox_SdlPadGyroTake(int n, float *yaw, float *pitch)
{
    (void)n;
    *yaw = *pitch = 0.0f;
    return 0;
}
int xbox_SdlPadGyroRate(int n, float *yaw, float *pitch)
{
    (void)n;
    *yaw = *pitch = 0.0f;
    return 0;
}
void xbox_SdlPadGyroCalibrate(int n) { (void)n; }
void xbox_SdlPadGyroAuto(int on) { (void)on; }

#endif
