/**
 * PlayStation pads through SDL3 (Windows; sdl_pads.c). Internal to
 * xbox_input: xinput_device.c makes them host pads 4-7.
 *
 * Needs <windows.h> and <xinput.h> first.
 */
#ifndef XBOX_SDL_PADS_H
#define XBOX_SDL_PADS_H

#define SDL_PADS_MAX 4

#ifndef XBOX_GYRO_HAS           /* as in xinput_xbox.h */
#define XBOX_GYRO_HAS         1
#define XBOX_GYRO_CALIBRATING 2
#define XBOX_GYRO_STILL       4
#endif

/** Starts SDL's thread (once). Without SDL3 in the build: nothing. */
void     xbox_SdlPadsInit(void);
/** Bit n: PlayStation pad n is connected. */
unsigned xbox_SdlPadsConnected(void);
/** Pad n's state in XInput's shape (an Xbox pad's layout); the packet
 *  number moves whenever the state does. */
DWORD    xbox_SdlPadGetState(int n, XINPUT_STATE *st);
/** SDL's name for pad n (UTF-8): 1 if connected. */
int      xbox_SdlPadName(int n, char *out, int size);
/** Pad n's motors (XInput's low- and high-frequency speeds, 0-65535),
 *  until the next call; handed to SDL's thread. */
void     xbox_SdlPadRumble(int n, WORD low, WORD high);
/** Every pad's motors off now, from any thread (the process is ending). */
void     xbox_SdlPadsStopRumble(void);
/** Gyro (xinput_xbox.h's xbox_InputGyro* by port): degrees turned since
 *  the last take (1 if pad n has a gyro); its speed now and XBOX_GYRO_*
 *  flags; a hand calibration; the automatic calibration on or off. */
int      xbox_SdlPadGyroTake(int n, float *yaw, float *pitch);
int      xbox_SdlPadGyroRate(int n, float *yaw, float *pitch);
void     xbox_SdlPadGyroCalibrate(int n);
void     xbox_SdlPadGyroAuto(int on);

#endif
