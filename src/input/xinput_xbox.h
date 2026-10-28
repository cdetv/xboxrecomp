/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system.
 * Maps Xbox controller ports to XInput slots.
 */
void xbox_InputInit(void);

/**
 * Get the state of a controller.
 * Port: 0-3 (Xbox controller ports)
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * Take the pads from the title while a host menu is open: 1 = every port
 * reports nothing pressed; 0 = give them back, each once all its buttons
 * are released.
 */
void xbox_InputSetBlocked(int on);

/**
 * Set controller vibration.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * Check if a controller is connected.
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

/* ---- which host device drives which Xbox port ------------------------------
 *
 * Until the first xbox_InputAssign, ports follow connection order (and
 * RECOMP_PAD_FOLLOW) and the keyboard is port 0's, as they always have. The
 * first call switches to fixed assignment: each port is driven by the host
 * pad it was given, the keyboard and mouse by the port they were given, and
 * nothing else. A pad is given to one port at a time; giving it to another
 * is the caller's to undo (both are settings the caller saves). */
#define XBOX_INPUT_NONE  (-1)   /* nothing: the port reports no device */
#define XBOX_INPUT_ANY   (-2)   /* the pad last used that no port was given */

/** source: an XInput user index 0-3, XBOX_INPUT_ANY or XBOX_INPUT_NONE. */
void xbox_InputAssign(DWORD dwPort, int source);
int  xbox_InputAssigned(DWORD dwPort);
/** The port the keyboard and mouse drive, or -1 for none. */
void xbox_InputSetKeyboardPort(int port);
int  xbox_InputKeyboardPort(void);
/** 1 once xbox_InputAssign has been called. */
int  xbox_InputAssignActive(void);
/** Ports something drives, bit n = port n: the USB model plugs a pad into
 *  each of them and pulls the rest. */
unsigned xbox_InputPortsInUse(void);
/** Host pad user index 0-3 connected (as of the last scan, under a second
 *  old), and the index driving a port now (-1: none). */
BOOL xbox_InputHostPadConnected(DWORD slot);
int  xbox_InputHostPadOfPort(DWORD dwPort);
/** A connected host pad's name as Windows' game controller list shows it
 *  (UTF-8), for a host menu: 1 if found, else 0 and out = "". Looked up
 *  again whenever the set of connected pads changes. */
int  xbox_InputHostPadName(DWORD slot, char *out, int size);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
