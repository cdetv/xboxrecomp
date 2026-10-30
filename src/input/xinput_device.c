/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput, plus PlayStation pads through SDL3
 *             (sdl_pads.c) as host pads 4-7
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "xbox_kbm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#include "sdl_pads.h"

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* ---- which XInput slot answers for which Xbox port ------------------------
 *
 * Xbox port n is the n-th connected XInput pad, not XInput slot n. Windows
 * hands out slots in connection order and keeps a slot taken by a virtual
 * pad that has come and gone, so the only pad on a machine can sit in slot 1
 * with slot 0 empty (Parsec's virtual pad on a PC with Steam, ViGEm and
 * Virtual Desktop gamepad drivers): read as slot 0, nothing answered.
 *
 * Rescanned once a second, and at once when the mapped slot stops
 * answering, because XInputGetState on an empty slot is slow enough not to
 * be done for every report. */
#define HOST_SLOTS XBOX_INPUT_HOST_PADS
#define NO_SLOT    HOST_SLOTS
static DWORD     g_slot_of_port[XBOX_MAX_CONTROLLERS];
static ULONGLONG g_next_scan;

/* A host pad: XInput user index 0-3, or 4-7 for PlayStation pad 0-3
 * (sdl_pads.c), read in XInput's shape. */
static DWORD host_get_state(DWORD slot, XINPUT_STATE *st)
{
    if (slot < XBOX_MAX_CONTROLLERS)
        return XInputGetState(slot, st);
    return xbox_SdlPadGetState((int)(slot - XBOX_MAX_CONTROLLERS), st);
}

/* RECOMP_PAD_FOLLOW=1: port 0 is the pad last used, not the first one
 * connected. Made for a single-player title played from more than one
 * place: a PC with DS4Windows' virtual pad (a PlayStation pad paired over
 * Bluetooth) took slot 0, and Parsec's pad for the same player, connected
 * later, became port 1, which a single-player game never reads
 * (2026-10-20). Pressing a button, a trigger or a stick on another pad
 * makes it port 0 once the current one has nothing held; the rest follow in
 * slot order. Off by default: with several players, ports must stay put. */
static DWORD g_active_slot = NO_SLOT;

static BOOL pad_follow(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_PAD_FOLLOW");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* A deliberate touch: a button, a trigger or a stick well past its dead
 * zone (a resting stick drifts, and must not steal port 0). */
static BOOL pad_touched(const XINPUT_GAMEPAD *g)
{
    return g->wButtons || g->bLeftTrigger > 30 || g->bRightTrigger > 30
        || g->sThumbLX > 16000 || g->sThumbLX < -16000
        || g->sThumbLY > 16000 || g->sThumbLY < -16000
        || g->sThumbRX > 16000 || g->sThumbRX < -16000
        || g->sThumbRY > 16000 || g->sThumbRY < -16000;
}

/* Bit n: host pad n answered the last scan. */
static volatile LONG g_slots_connected;

static void scan_slots(void)
{
    static volatile LONG scanning;
    DWORD i, n = 0;
    LONG mask = 0;
    /* The USB model's thread and the host's menus both ask; one scan at a
     * time, the other keeps what the last one found. */
    if (InterlockedExchange(&scanning, 1))
        return;
    /* PlayStation pads start with the first scan: a title that reads its
     * pads through the USB model never calls xbox_InputInit. */
    xbox_SdlPadsInit();
    for (i = 0; i < HOST_SLOTS; i++) {
        XINPUT_STATE s;
        if (host_get_state(i, &s) == ERROR_SUCCESS) {
            if (n < XBOX_MAX_CONTROLLERS)
                g_slot_of_port[n++] = i;
            mask |= 1L << i;
        }
    }
    InterlockedExchange(&g_slots_connected, mask);
    for (i = n; i < XBOX_MAX_CONTROLLERS; i++)
        g_slot_of_port[i] = NO_SLOT;
    /* The pad last used goes first; the others keep their order. */
    for (i = 1; pad_follow() && i < n; i++)
        if (g_slot_of_port[i] == g_active_slot) {
            memmove(&g_slot_of_port[1], &g_slot_of_port[0],
                    i * sizeof(g_slot_of_port[0]));
            g_slot_of_port[0] = g_active_slot;
            break;
        }
    g_next_scan = GetTickCount64() + 1000;
    InterlockedExchange(&scanning, 0);
}

/* RECOMP_PAD_FOLLOW: before port 0 is read, look at the other connected
 * pads (slots that answered the last scan, so no slow empty-slot calls) and
 * hand port 0 to one that is being touched while port 0's pad is not. */
static void follow_active_pad(void)
{
    XINPUT_STATE s;
    DWORD i, cur = g_slot_of_port[0];

    if (cur == NO_SLOT)
        return;
    if (host_get_state(cur, &s) == ERROR_SUCCESS && pad_touched(&s.Gamepad))
        return;
    for (i = 1; i < XBOX_MAX_CONTROLLERS; i++) {
        DWORD slot = g_slot_of_port[i];
        if (slot == NO_SLOT)
            break;
        if (host_get_state(slot, &s) == ERROR_SUCCESS
            && pad_touched(&s.Gamepad)) {
            g_active_slot = slot;
            fprintf(stderr, "[INPUT] host pad %lu is now port 0"
                    " (last used)\n", (unsigned long)slot);
            scan_slots();
            return;
        }
    }
}

static BOOL keyboard_enabled(void);

/* ---- fixed assignment (xbox_InputAssign) ----------------------------------
 *
 * Made for local multiplayer and for swapping between the keyboard and a
 * pad: the host's settings menu says which pad drives which port and which
 * port the keyboard is. A pad's slot is the XInput user index Windows gave
 * it; XBOX_INPUT_ANY keeps the old "the pad last used" for a port, among the
 * pads no other port was given, so one player's setup needs no choosing. */
static volatile LONG g_assign_on;
static volatile LONG g_source[XBOX_MAX_CONTROLLERS] = {
    XBOX_INPUT_NONE, XBOX_INPUT_NONE, XBOX_INPUT_NONE, XBOX_INPUT_NONE,
};
static volatile LONG g_kb_port;            /* -1: no port */
static DWORD g_any_slot = NO_SLOT;         /* XBOX_INPUT_ANY's pad now */

void xbox_InputAssign(DWORD port, int source)
{
    if (port >= XBOX_MAX_CONTROLLERS)
        return;
    if (source < XBOX_INPUT_ANY || source >= HOST_SLOTS)
        source = XBOX_INPUT_NONE;
    InterlockedExchange(&g_source[port], source);
    if (!InterlockedExchange(&g_assign_on, 1))
        fprintf(stderr, "[INPUT] fixed port assignment on\n");
    if (source < 0)
        fprintf(stderr, "[INPUT] port %lu: %s\n", (unsigned long)port + 1,
                source == XBOX_INPUT_ANY ? "any controller" : "no input");
    else
        fprintf(stderr, "[INPUT] port %lu: %s controller %d\n",
                (unsigned long)port + 1,
                source < XBOX_MAX_CONTROLLERS ? "XInput" : "PlayStation",
                source % XBOX_MAX_CONTROLLERS + 1);
}

int xbox_InputAssigned(DWORD port)
{
    return port < XBOX_MAX_CONTROLLERS ? (int)g_source[port] : XBOX_INPUT_NONE;
}

void xbox_InputSetKeyboardPort(int port)
{
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS)
        port = -1;
    if (InterlockedExchange(&g_kb_port, port) != port)
        fprintf(stderr, "[INPUT] keyboard and mouse: %s%d\n",
                port < 0 ? "no port" : "port ", port < 0 ? 0 : port + 1);
}

int xbox_InputKeyboardPort(void) { return (int)g_kb_port; }
int xbox_InputAssignActive(void) { return g_assign_on ? 1 : 0; }

unsigned xbox_InputPortsInUse(void)
{
    unsigned m = 0;
    DWORD i;
    if (!g_assign_on)
        return 1u;
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_source[i] != XBOX_INPUT_NONE)
            m |= 1u << i;
    if (g_kb_port >= 0 && (xbox_KbmConfigured() || keyboard_enabled()))
        m |= 1u << g_kb_port;
    return m;
}

BOOL xbox_InputHostPadConnected(DWORD slot)
{
    if (slot >= HOST_SLOTS)
        return FALSE;
    if (GetTickCount64() >= g_next_scan)
        scan_slots();
    return (g_slots_connected >> slot) & 1 ? TRUE : FALSE;
}

int xbox_InputHostPadOfPort(DWORD port)
{
    DWORD s;
    if (port >= XBOX_MAX_CONTROLLERS)
        return -1;
    s = g_assign_on ? (g_source[port] == XBOX_INPUT_ANY ? g_any_slot
                       : g_source[port] >= 0 ? (DWORD)g_source[port] : NO_SLOT)
        : g_slot_of_port[port];
    return s < HOST_SLOTS && ((g_slots_connected >> s) & 1) ? (int)s : -1;
}

/* ---- pad names, for a host menu -------------------------------------------
 *
 * XInput has no names, only user indices, and "controller 2" tells a player
 * nothing (user 2026-10-28). Windows' own list (joy.cpl, "USB game
 * controllers") is DirectInput's: its instance name per device, whose
 * product GUID holds the USB vendor and product IDs. XInput's
 * XInputGetCapabilitiesEx (xinput1_4.dll ordinal 108, undocumented, as SDL
 * uses it) gives a pad's IDs, which picks its name from that list. Two pads
 * of one model get the same name; virtual pads (Parsec, ViGEm, DS4Windows)
 * mostly say Xbox 360. */
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

typedef struct {
    XINPUT_CAPABILITIES caps;
    WORD  vendor, product, version, unk1;
    DWORD unk2;
} XInputCapsEx;
typedef DWORD (WINAPI *XInputCapsExFn)(DWORD, DWORD, DWORD, XInputCapsEx *);

#define MAX_DI_PADS 16
typedef struct {
    int   n;
    DWORD id[MAX_DI_PADS];          /* product GUID Data1: vendor | product << 16 */
    char  name[MAX_DI_PADS][96];
} DiPads;

static BOOL CALLBACK di_pad(LPCDIDEVICEINSTANCEW d, LPVOID ctx)
{
    DiPads *p = (DiPads *)ctx;
    if (p->n >= MAX_DI_PADS)
        return DIENUM_STOP;
    p->id[p->n] = d->guidProduct.Data1;
    if (!WideCharToMultiByte(CP_UTF8, 0, d->tszInstanceName, -1,
                             p->name[p->n], sizeof(p->name[0]), NULL, NULL))
        p->name[p->n][0] = '\0';
    p->n++;
    return DIENUM_CONTINUE;
}

static char g_pad_name[HOST_SLOTS][96];
static LONG g_names_for = -1;       /* the connected set they were found for */

static void find_pad_names(void)
{
    static XInputCapsExFn caps_ex;
    static int tried;
    IDirectInput8W *di = NULL;
    DiPads list;
    DWORD i;
    int k;

    memset(g_pad_name, 0, sizeof(g_pad_name));
    /* PlayStation pads: SDL has their names. */
    for (i = XBOX_MAX_CONTROLLERS; i < HOST_SLOTS; i++)
        if ((g_slots_connected >> i) & 1)
            xbox_SdlPadName((int)(i - XBOX_MAX_CONTROLLERS), g_pad_name[i],
                            sizeof(g_pad_name[i]));
    if (!tried) {
        HMODULE x = GetModuleHandleA("xinput1_4.dll");
        tried = 1;
        if (!x)
            x = LoadLibraryA("xinput1_4.dll");
        if (x)
            caps_ex = (XInputCapsExFn)(void (*)(void))GetProcAddress(x, (LPCSTR)108);
        if (!caps_ex)
            fprintf(stderr, "[INPUT] no XInputGetCapabilitiesEx: controllers"
                    " go by number\n");
    }
    list.n = 0;
    if (!caps_ex || FAILED(DirectInput8Create(GetModuleHandleA(NULL),
                                              DIRECTINPUT_VERSION,
                                              &IID_IDirectInput8W,
                                              (void **)&di, NULL)))
        return;
    IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, di_pad, &list,
                              DIEDFL_ATTACHEDONLY);
    IDirectInput8_Release(di);
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XInputCapsEx c;
        DWORD id;
        if (!((g_slots_connected >> i) & 1))
            continue;
        memset(&c, 0, sizeof(c));
        if (caps_ex(1, i, 0, &c) != ERROR_SUCCESS)
            continue;
        id = (DWORD)c.vendor | ((DWORD)c.product << 16);
        for (k = 0; k < list.n; k++)
            if (list.id[k] == id && list.name[k][0]) {
                snprintf(g_pad_name[i], sizeof(g_pad_name[i]), "%s",
                         list.name[k]);
                break;
            }
        fprintf(stderr, "[INPUT] XInput slot %lu: %04X:%04X \"%s\"\n",
                (unsigned long)i, c.vendor, c.product, g_pad_name[i]);
    }
}

int xbox_InputHostPadName(DWORD slot, char *out, int size)
{
    if (size > 0)
        out[0] = '\0';
    if (slot >= HOST_SLOTS || size <= 0
        || !xbox_InputHostPadConnected(slot))
        return 0;
    if (g_names_for != g_slots_connected) {
        g_names_for = g_slots_connected;
        find_pad_names();
    }
    snprintf(out, (size_t)size, "%s", g_pad_name[slot]);
    return out[0] != '\0';
}

/* For a host's own menus: the pad read directly, never a slow empty
 * XInput slot (only ones the last scan found). */
DWORD xbox_InputHostPadRead(DWORD slot, XINPUT_STATE *st)
{
    if (!xbox_InputHostPadConnected(slot))
        return ERROR_DEVICE_NOT_CONNECTED;
    return host_get_state(slot, st);
}

int xbox_InputFirstHostPad(void)
{
    DWORD i;
    if (GetTickCount64() >= g_next_scan)
        scan_slots();
    for (i = 0; i < HOST_SLOTS; i++)
        if ((g_slots_connected >> i) & 1)
            return (int)i;
    return -1;
}

/* Given to a port by number (so not XBOX_INPUT_ANY's to take). */
static BOOL slot_given(DWORD slot)
{
    DWORD i;
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_source[i] == (LONG)slot)
            return TRUE;
    return FALSE;
}

static BOOL slot_free_pad(DWORD slot)
{
    return ((g_slots_connected >> slot) & 1) && !slot_given(slot);
}

/* XBOX_INPUT_ANY: the pad it had while that is still free and connected;
 * with RECOMP_PAD_FOLLOW, another free pad being touched while it is not
 * takes over (as follow_active_pad does for port 0); with none yet, the
 * first free pad. */
static DWORD any_slot(void)
{
    XINPUT_STATE s;
    DWORD i, cur = g_any_slot;

    if (cur != NO_SLOT && !slot_free_pad(cur))
        cur = NO_SLOT;
    if (cur != NO_SLOT && pad_follow()
        && !(host_get_state(cur, &s) == ERROR_SUCCESS && pad_touched(&s.Gamepad)))
        for (i = 0; i < HOST_SLOTS; i++)
            if (i != cur && slot_free_pad(i)
                && host_get_state(i, &s) == ERROR_SUCCESS
                && pad_touched(&s.Gamepad)) {
                fprintf(stderr, "[INPUT] any controller: host pad %lu"
                        " (last used)\n", (unsigned long)i);
                cur = i;
                break;
            }
    for (i = 0; cur == NO_SLOT && i < HOST_SLOTS; i++)
        if (slot_free_pad(i))
            cur = i;
    g_any_slot = cur;
    return cur;
}

static DWORD slot_of_port(DWORD port)
{
    LONG src;
    if (GetTickCount64() >= g_next_scan)
        scan_slots();
    if (!g_assign_on)
        return g_slot_of_port[port];
    src = g_source[port];
    if (src == XBOX_INPUT_ANY)
        return any_slot();
    /* An empty slot is not asked: XInputGetState on one is slow. */
    if (src >= 0 && ((g_slots_connected >> src) & 1))
        return (DWORD)src;
    return NO_SLOT;
}

/* The port the keyboard and mouse drive: port 0 until assignment says. */
static BOOL keyboard_port(DWORD port)
{
    return g_assign_on ? (LONG)port == g_kb_port : port == 0;
}

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * The keys are the ones a Dreamcast or Saturn emulator would pick, which is
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* A thumb axis from two keys, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (key_down(negative)) v -= 32767;
    if (key_down(positive)) v += 32767;
    return (SHORT)v;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    WORD b = 0;

    memset(pState, 0, sizeof(*pState));

    if (key_down(VK_UP))     b |= XBOX_GAMEPAD_DPAD_UP;
    if (key_down(VK_DOWN))   b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (key_down(VK_LEFT))   b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (key_down(VK_RIGHT))  b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (key_down(VK_RETURN)) b |= XBOX_GAMEPAD_START;
    if (key_down(VK_BACK))   b |= XBOX_GAMEPAD_BACK;
    if (key_down(VK_SHIFT))  b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (key_down(VK_CONTROL))b |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A]        = key_down('Z') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B]        = key_down('X') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X]        = key_down('A') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y]        = key_down('S') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]    = key_down('Q') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]    = key_down('E') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = key_down('1') ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = key_down('3') ? 255 : 0;

    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    pState->Gamepad.sThumbLX = axis_from_keys(VK_NUMPAD4, VK_NUMPAD6);
    pState->Gamepad.sThumbLY = axis_from_keys(VK_NUMPAD2, VK_NUMPAD8);
    pState->Gamepad.sThumbRX = axis_from_keys('J', 'L');
    pState->Gamepad.sThumbRY = axis_from_keys('K', 'I');

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

void xbox_InputInit(void)
{
    scan_slots();
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        g_controller_connected[i] = g_slot_of_port[i] != NO_SLOT;
}

/* ---- input taken by the host (a settings overlay) -------------------------
 *
 * While blocked, every connected port reports nothing pressed, so presses
 * meant for a host menu never reach the title. Unblocking does not let the
 * pads through at once: a port stays at rest until all its buttons and
 * triggers are let go, or the button that closed the menu would arrive as
 * a fresh press (Start on top of a menu closed with Start pauses the game). */
static volatile LONG g_input_blocked;
static BOOL          g_port_held[XBOX_MAX_CONTROLLERS];

void xbox_InputSetBlocked(int on)
{
    DWORD i;
    if (on)
        for (i = 0; i < XBOX_MAX_CONTROLLERS; i++)
            g_port_held[i] = TRUE;
    InterlockedExchange(&g_input_blocked, on ? 1 : 0);
}

/* Anything held: a digital button, an analog button or trigger past a
 * light touch. Sticks do not count -- one resting off centre would keep
 * the pad blocked for ever. */
static BOOL any_held(const XBOX_INPUT_STATE *s)
{
    int i;
    if (s->Gamepad.wButtons)
        return TRUE;
    for (i = 0; i < 8; i++)
        if (s->Gamepad.bAnalogButtons[i] > 30)
            return TRUE;
    return FALSE;
}

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState);

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD rc = input_get_state(dwPort, pState);

    if (rc != ERROR_SUCCESS || dwPort >= XBOX_MAX_CONTROLLERS)
        return rc;
    if (!InterlockedCompareExchange(&g_input_blocked, 0, 0)
        && g_port_held[dwPort] && !any_held(pState))
        g_port_held[dwPort] = FALSE;
    if (InterlockedCompareExchange(&g_input_blocked, 0, 0)
        || g_port_held[dwPort]) {
        DWORD packet = pState->dwPacketNumber;
        memset(pState, 0, sizeof(*pState));
        pState->dwPacketNumber = packet;
    }
    return ERROR_SUCCESS;
}

static DWORD input_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state = { 0 };
    DWORD result, slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    slot = slot_of_port(dwPort);
    if (dwPort == 0 && pad_follow() && !g_assign_on) {
        follow_active_pad();
        slot = g_slot_of_port[0];
    }
    result = slot == NO_SLOT ? ERROR_DEVICE_NOT_CONNECTED
                             : host_get_state(slot, &xi_state);
    if (result != ERROR_SUCCESS) {
        if (slot != NO_SLOT)
            g_next_scan = 0;            /* unplugged: look again next time */
        g_controller_connected[dwPort] = FALSE;
        /* No pad: keyboard and mouse alone are the player. */
        if (keyboard_port(dwPort) && xbox_KbmState(pState))
            return ERROR_SUCCESS;
        if (keyboard_port(dwPort) && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    /* The bound keyboard and mouse (kbm_pad.c) when the game has set them
     * up, else the stand-in below. Merged the same way: a button pressed
     * on either is pressed, the greater pressure wins, and a stick the
     * keyboard or mouse moves is theirs. */
    if (keyboard_port(dwPort) && (xbox_KbmConfigured() || keyboard_enabled())) {
        XBOX_INPUT_STATE kb;
        int i;
        if (!xbox_KbmState(&kb))
            keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    DWORD slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    slot = slot_of_port(dwPort);
    if (slot == NO_SLOT)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (slot >= XBOX_MAX_CONTROLLERS)
        return ERROR_SUCCESS;           /* PlayStation pad: no rumble yet */
    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(slot, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result, slot;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    slot = slot_of_port(dwPort);
    if (slot == NO_SLOT)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (slot >= XBOX_MAX_CONTROLLERS) {
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        return ERROR_SUCCESS;
    }
    result = XInputGetCapabilities(slot, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

/* Host overlays are Windows-only so far, and so is fixed assignment:
 * ports here are SDL's controllers in order. */
void xbox_InputSetBlocked(int on) { (void)on; }
void xbox_InputAssign(DWORD dwPort, int source) { (void)dwPort; (void)source; }
int  xbox_InputAssigned(DWORD dwPort) { return dwPort == 0 ? XBOX_INPUT_ANY : XBOX_INPUT_NONE; }
void xbox_InputSetKeyboardPort(int port) { (void)port; }
int  xbox_InputKeyboardPort(void) { return 0; }
int  xbox_InputAssignActive(void) { return 0; }
unsigned xbox_InputPortsInUse(void) { return 1u; }
BOOL xbox_InputHostPadConnected(DWORD slot)
{
    return slot < XBOX_MAX_CONTROLLERS && g_pads[slot] ? TRUE : FALSE;
}
int  xbox_InputHostPadOfPort(DWORD dwPort)
{
    return dwPort < XBOX_MAX_CONTROLLERS && g_pads[dwPort] ? (int)dwPort : -1;
}
int  xbox_InputHostPadName(DWORD slot, char *out, int size)
{
    const char *n = slot < XBOX_MAX_CONTROLLERS && g_pads[slot]
                  ? SDL_GameControllerName(g_pads[slot]) : NULL;
    if (size <= 0)
        return 0;
    snprintf(out, (size_t)size, "%s", n ? n : "");
    return out[0] != '\0';
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
