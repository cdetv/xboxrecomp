/**
 * PlayStation pads through SDL3 (Windows; sdl_pads.c). Internal to
 * xbox_input: xinput_device.c makes them host pads 4-7.
 *
 * Needs <windows.h> and <xinput.h> first.
 */
#ifndef XBOX_SDL_PADS_H
#define XBOX_SDL_PADS_H

#define SDL_PADS_MAX 4

/** Starts SDL's thread (once). Without SDL3 in the build: nothing. */
void     xbox_SdlPadsInit(void);
/** Bit n: PlayStation pad n is connected. */
unsigned xbox_SdlPadsConnected(void);
/** Pad n's state in XInput's shape (an Xbox pad's layout); the packet
 *  number moves whenever the state does. */
DWORD    xbox_SdlPadGetState(int n, XINPUT_STATE *st);
/** SDL's name for pad n (UTF-8): 1 if connected. */
int      xbox_SdlPadName(int n, char *out, int size);

#endif
