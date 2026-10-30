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

typedef struct {
    SDL_Gamepad  *pad;          /* SDL thread only */
    SDL_JoystickID id;          /* SDL thread only */
    BOOL          connected;
    XINPUT_STATE  state;
    char          name[96];
} SdlPad;

static SdlPad  g_pads[SDL_PADS_MAX];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_mask;    /* bit n: pad n connected */

/* Only PlayStation pads: an Xbox pad is XInput's already, and SDL's own
 * XInput, Raw Input, WGI and DirectInput back ends are off (sdl_thread),
 * so the same pad never shows up twice. */
static BOOL wanted(SDL_JoystickID id)
{
    SDL_GamepadType t = SDL_GetGamepadTypeForID(id);
    return t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
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
    AcquireSRWLockExclusive(&g_lock);
    g_pads[n].pad = gp;
    g_pads[n].id = id;
    g_pads[n].connected = TRUE;
    memset(&g_pads[n].state, 0, sizeof(g_pads[n].state));
    snprintf(g_pads[n].name, sizeof(g_pads[n].name), "%s",
             SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "");
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedOr(&g_mask, 1L << n);
    fprintf(stderr, "[INPUT] SDL: PlayStation controller %d: \"%s\" (%s)\n",
            n + 1, g_pads[n].name,
            SDL_GetGamepadConnectionState(gp) == SDL_JOYSTICK_CONNECTION_WIRELESS
                ? "wireless" : "wired");
}

static void pad_removed(SDL_JoystickID id)
{
    int n;
    for (n = 0; n < SDL_PADS_MAX; n++)
        if (g_pads[n].pad && g_pads[n].id == id) {
            InterlockedAnd(&g_mask, ~(1L << n));
            AcquireSRWLockExclusive(&g_lock);
            g_pads[n].connected = FALSE;
            ReleaseSRWLockExclusive(&g_lock);
            SDL_CloseGamepad(g_pads[n].pad);
            g_pads[n].pad = NULL;
            fprintf(stderr, "[INPUT] SDL: PlayStation controller %d gone\n",
                    n + 1);
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
 * alone (Windows and Steam use it). */
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
        fprintf(stderr, "[INPUT] SDL: no PlayStation controllers (%s)\n",
                SDL_GetError());
        return 0;
    }
    fprintf(stderr, "[INPUT] SDL %d.%d.%d: PlayStation controllers on\n",
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
        }
        for (n = 0; n < SDL_PADS_MAX; n++) {
            XINPUT_GAMEPAD g;
            if (!g_pads[n].pad)
                continue;
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

#endif
