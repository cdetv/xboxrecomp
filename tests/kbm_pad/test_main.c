/* Keyboard and mouse as player 1's pad (src/input/kbm_pad.c): bindings to
 * buttons and sticks, the shortest press, wheel notches as presses, walking
 * tilt, the mouse as right-stick speed in the title's own stick range, and
 * the weapon-wheel mode where the mouse steers the left stick. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "xbox_kbm.h"

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int fails;

static void check(int ok, const char *what, long got)
{
    printf("%s: %s (%ld)\n", ok ? "PASS" : "FAIL", what, got);
    fails += !ok;
}

static XBOX_INPUT_STATE read_pad(void)
{
    XBOX_INPUT_STATE st;
    xbox_KbmState(&st);
    return st;
}

/* Reads the pad every 5 ms for ms, as the USB thread does. */
static void reports(int ms)
{
    XBOX_INPUT_STATE st;
    for (int i = 0; i < ms; i += 5) {
        xbox_KbmState(&st);
        Sleep(5);
    }
}

/* The raw value asked for v (0-1) of a stick's range. */
static long raw(float v, const xbox_KbmStick *s)
{
    return (long)((s->dead + v * (s->full - s->dead)) * 32767.0f);
}

int main(void)
{
    XBOX_INPUT_STATE st;
    xbox_KbmMouse m = { 1.0f, 1.0f, 0, 0.45f,
                        { 15.0f / 110.0f, 87.0f / 110.0f },
                        { 15.0f / 99.0f, 74.0f / 99.0f } };
    xbox_KbmBinding b[] = {
        { 'W', XBOX_KBM_LSTICK_UP }, { 'D', XBOX_KBM_LSTICK_RIGHT },
        { VK_SHIFT, XBOX_KBM_WALK }, { VK_SPACE, XBOX_KBM_A },
        { VK_ESCAPE, XBOX_KBM_START }, { XBOX_KBM_WHEEL_UP, XBOX_KBM_Y },
        { 'Q', XBOX_KBM_Y }, { 'Q', XBOX_KBM_MOUSE_LSTICK },
        { 'L', XBOX_KBM_RSTICK_RIGHT },
    };
    long v;

    check(xbox_KbmState(&st) == 0, "nothing bound: not in use", 0);
    xbox_KbmSetProfile(0, b, (int)(sizeof(b) / sizeof(b[0])), &m);
    xbox_KbmSelectProfile(0);
    check(xbox_KbmState(&st) == 1 && !st.Gamepad.sThumbLY, "bound, at rest",
          st.Gamepad.sThumbLY);

    xbox_KbmKey('W', 1);
    st = read_pad();
    check(st.Gamepad.sThumbLY == 32767, "W: left stick all the way up",
          st.Gamepad.sThumbLY);
    xbox_KbmKey('D', 1);
    st = read_pad();
    check(st.Gamepad.sThumbLX == 32767 && st.Gamepad.sThumbLY == 32767,
          "W+D: both axes full (the title shapes diagonals)",
          st.Gamepad.sThumbLX);
    xbox_KbmKey('D', 0);
    Sleep(100);                     /* past D's shortest press */
    xbox_KbmKey(VK_SHIFT, 1);
    st = read_pad();
    v = raw(0.45f, &m.left);
    check(labs(st.Gamepad.sThumbLY - v) <= 1, "Shift+W: walking tilt",
          st.Gamepad.sThumbLY);
    xbox_KbmKey('D', 1);
    st = read_pad();
    v = raw(0.45f / sqrtf(2.0f), &m.left);
    check(labs(st.Gamepad.sThumbLX - v) <= 1 && labs(st.Gamepad.sThumbLY - v) <= 1,
          "Shift+W+D: walking diagonal as fast as straight", st.Gamepad.sThumbLX);
    xbox_KbmKey('D', 0);
    xbox_KbmKey('W', 0);
    xbox_KbmKey(VK_SHIFT, 0);

    xbox_KbmKey(VK_SPACE, 1);
    xbox_KbmKey(VK_SPACE, 0);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255,
          "a tap is still held right after", st.Gamepad.bAnalogButtons[0]);
    Sleep(100);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0,
          "... and let go after the shortest press", st.Gamepad.bAnalogButtons[0]);
    xbox_KbmKey(VK_ESCAPE, 1);
    st = read_pad();
    check((st.Gamepad.wButtons & XBOX_GAMEPAD_START) != 0, "Esc: Start",
          st.Gamepad.wButtons);
    xbox_KbmKey(VK_ESCAPE, 0);
    Sleep(100);

    xbox_KbmWheel(2);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255, "wheel: first notch",
          st.Gamepad.bAnalogButtons[3]);
    Sleep(100);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 0, "wheel: gap", 0);
    Sleep(60);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255, "wheel: second notch",
          st.Gamepad.bAnalogButtons[3]);
    Sleep(300);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 0, "wheel: done", 0);

    /* Mouse: 3 counts per ms is full tilt at sensitivity 1. The pad is
     * read every 5 ms here, as the USB thread does; the speed is taken over
     * the last 40 ms or a little more (whole report intervals). */
    reports(100);
    xbox_KbmMouseMove(200, 0);
    st = read_pad();
    check(st.Gamepad.sThumbRX == raw(1.0f, &m.right),
          "mouse fast right: right stick at the title's full", st.Gamepad.sThumbRX);
    reports(100);
    xbox_KbmMouseMove(-12, 0);
    st = read_pad();
    check(st.Gamepad.sThumbRX < -raw(0.05f, &m.right)
          && st.Gamepad.sThumbRX > -raw(0.11f, &m.right),
          "mouse slow left: about a tenth, past the dead zone",
          st.Gamepad.sThumbRX);
    reports(100);
    xbox_KbmMouseMove(0, -60);
    st = read_pad();
    check(st.Gamepad.sThumbRY > raw(0.35f, &m.right)
          && st.Gamepad.sThumbRY < raw(0.55f, &m.right)
          && st.Gamepad.sThumbRX == 0,
          "mouse forward: right stick up, about half", st.Gamepad.sThumbRY);
    reports(100);
    st = read_pad();
    check(st.Gamepad.sThumbRX == 0 && st.Gamepad.sThumbRY == 0,
          "mouse still: stick centred", st.Gamepad.sThumbRY);
    m.invert_y = 1;
    xbox_KbmSetProfile(0, b, (int)(sizeof(b) / sizeof(b[0])), &m);
    reports(100);
    xbox_KbmMouseMove(0, -60);
    st = read_pad();
    check(st.Gamepad.sThumbRY < 0, "inverted: forward looks down",
          st.Gamepad.sThumbRY);
    m.invert_y = 0;
    xbox_KbmSetProfile(0, b, (int)(sizeof(b) / sizeof(b[0])), &m);
    Sleep(100);
    xbox_KbmKey('L', 1);
    st = read_pad();
    check(st.Gamepad.sThumbRX == raw(1.0f, &m.right), "look key: full",
          st.Gamepad.sThumbRX);
    xbox_KbmKey('L', 0);

    /* Weapon wheel: Q held is Y; after a moment the mouse is the left
     * stick (a position, kept after the release for the title to read). */
    xbox_KbmKey('Q', 1);
    xbox_KbmMouseMove(0, -500);
    st = read_pad();
    check(st.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] == 255 && st.Gamepad.sThumbLY == 0
          && st.Gamepad.sThumbRY == 0, "wheel key: Y, the stick not yet", 0);
    Sleep(200);
    st = read_pad();
    check(st.Gamepad.sThumbLY == raw(1.0f, &m.left) && st.Gamepad.sThumbRY == 0,
          "wheel key held: mouse up = left stick up, no looking",
          st.Gamepad.sThumbLY);
    xbox_KbmKey('Q', 0);
    st = read_pad();
    check(st.Gamepad.sThumbLY == raw(1.0f, &m.left), "released: choice kept",
          st.Gamepad.sThumbLY);
    Sleep(200);
    st = read_pad();
    check(st.Gamepad.sThumbLY == 0, "then centred", st.Gamepad.sThumbLY);

    xbox_KbmKey('W', 1);
    xbox_KbmSetActive(0);
    check(xbox_KbmState(&st) == 1 && st.Gamepad.sThumbLY == 0,
          "menu open: in use, nothing pressed", st.Gamepad.sThumbLY);
    xbox_KbmSetActive(1);
    xbox_KbmSetEnabled(0);
    check(xbox_KbmState(&st) == 0, "turned off: not in use", 0);
    return fails ? 1 : 0;
}
