/**
 * Keyboard and mouse as player 1's pad (kbm_pad.c).
 *
 * The title only knows a controller, so keys, mouse buttons and the wheel
 * are bound to pad outputs (a button, a trigger, a stick direction), and
 * mouse movement becomes right-stick tilt. The game's own code chooses the
 * bindings and hands them over as up to four profiles (single player and
 * multiplayer, say), one of them in use; nothing is bound until it does, and
 * until then the old RECOMP_KEYBOARD stand-in in xinput_device.c is what a
 * key reaches.
 *
 * The result is merged on top of port 0's real pad, so a controller keeps
 * working beside them.
 *
 * Threads: the window thread reports keys and mouse (xbox_KbmKey and the
 * rest), the USB thread reads the pad (xbox_KbmState), anyone may set
 * profiles and choose one.
 */
#ifndef XBOX_KBM_H
#define XBOX_KBM_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a binding drives. The analog buttons first, in XBOX_BUTTON_ order. */
enum {
    XBOX_KBM_NONE = 0,
    XBOX_KBM_A, XBOX_KBM_B, XBOX_KBM_X, XBOX_KBM_Y,
    XBOX_KBM_BLACK, XBOX_KBM_WHITE, XBOX_KBM_LT, XBOX_KBM_RT,
    XBOX_KBM_DPAD_UP, XBOX_KBM_DPAD_DOWN, XBOX_KBM_DPAD_LEFT,
    XBOX_KBM_DPAD_RIGHT, XBOX_KBM_START, XBOX_KBM_BACK,
    XBOX_KBM_LTHUMB, XBOX_KBM_RTHUMB,
    XBOX_KBM_LSTICK_UP, XBOX_KBM_LSTICK_DOWN, XBOX_KBM_LSTICK_LEFT,
    XBOX_KBM_LSTICK_RIGHT,
    XBOX_KBM_RSTICK_UP, XBOX_KBM_RSTICK_DOWN, XBOX_KBM_RSTICK_LEFT,
    XBOX_KBM_RSTICK_RIGHT,
    XBOX_KBM_WALK,            /* held: the left stick from keys at walk tilt */
    XBOX_KBM_MOUSE_LSTICK,    /* held: the mouse steers the left stick, not
                               * the right (a weapon wheel picked with it) */
    XBOX_KBM_OUTPUTS
};

/* Or'ed into a binding's output: a press holds it, the next lets it go. */
#define XBOX_KBM_TOGGLE      0x8000

/* Inputs: Windows virtual-key codes 1-255, mouse buttons among them
 * (VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2), and the
 * wheel, one short press per notch. */
#define XBOX_KBM_WHEEL_UP    256
#define XBOX_KBM_WHEEL_DOWN  257
#define XBOX_KBM_INPUTS      258

typedef struct {
    unsigned short input, output;
} xbox_KbmBinding;

/* A stick as the title reads it: tilt below dead (0-1 of full) counts as
 * none, tilt from full on as all the way. Keys and the mouse ask for 0-1 of
 * the title's own range and get dead + that * (full - dead). 0 and 1 are
 * a plain stick. */
typedef struct {
    float dead, full;
} xbox_KbmStick;

typedef struct {
    float sens_x, sens_y;     /* mouse to right stick, 1 = the default;
                               * 0 = the mouse does not look */
    int   invert_y;           /* mouse forward looks down */
    float walk;               /* left-stick tilt while walking, 0-1 */
    xbox_KbmStick left, right;
} xbox_KbmMouse;

#define XBOX_KBM_PROFILES 16

/* Replace profile slot's bindings (at most 256; one input may drive several
 * outputs through several entries). */
void xbox_KbmSetProfile(int slot, const xbox_KbmBinding *b, int n,
                        const xbox_KbmMouse *mouse);
/* The profile in use. A key held across a change acts by the new one;
 * toggles are let go (a head-cam left on through a cutscene). */
void xbox_KbmSelectProfile(int slot);
/* Keyboard and mouse on at all (they are once a profile is set). */
void xbox_KbmSetEnabled(int on);
/* 0 while a host menu is open or the game is held: the mouse is let go and
 * nothing reaches the pad. */
void xbox_KbmSetActive(int on);

/* For the window: is there a profile, and should the mouse be held in the
 * window now. A change of either calls the wake function (the window posts
 * itself a message, so capturing happens on its own thread). */
int  xbox_KbmConfigured(void);
int  xbox_KbmWantsMouse(void);
void xbox_KbmSetWake(void (*wake)(void));
void xbox_KbmKey(unsigned input, int down);
void xbox_KbmWheel(int notches);
void xbox_KbmMouseMove(long dx, long dy);
void xbox_KbmReleaseAll(void);

/* Mouse look done by the game itself (turning its camera by the mouse's
 * movement, past the stick's top speed), per axis: for the axes given the
 * mouse no longer tilts the right stick, and xbox_KbmTakeMouse hands over
 * their movement since the last call (raw counts, the sensitivity is the
 * game's to apply; 0 for the other axis). 0 and nothing moved while not
 * active, or while a binding has the mouse steer the left stick (a weapon
 * wheel keeps it). */
#define XBOX_KBM_DIRECT_X 1
#define XBOX_KBM_DIRECT_Y 2
void xbox_KbmMouseDirect(int axes);
int  xbox_KbmTakeMouse(long *dx, long *dy);

/* For the pad: player 1's keyboard and mouse as a pad state. 0 when they
 * are off or not active (pState is then at rest). */
int  xbox_KbmState(XBOX_INPUT_STATE *pState);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KBM_H */
