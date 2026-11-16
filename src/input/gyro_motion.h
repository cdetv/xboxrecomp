/**
 * A pad's motion sensors through JibbSmart's GamepadMotionHelpers
 * (third_party/gamepadmotionhelpers, MIT): gyro calibration (by hand, and
 * automatic while the pad is held still) and the gravity estimate that
 * "player space" needs. A C front for sdl_pads.c (gyro_motion.cpp is the
 * only C++ in xbox_input). Not thread-safe: one thread per object (the SDL
 * thread).
 *
 * Units here are the library's: gyro in degrees a second, accelerometer in
 * g; axes as SDL's (and PlayStation pads'): +X right, +Y up, +Z toward the
 * player, counter-clockwise positive looking from an axis's positive end.
 */
#ifndef XBOX_GYRO_MOTION_H
#define XBOX_GYRO_MOTION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GyroMotion GyroMotion;

GyroMotion *gyro_motion_new(void);
void gyro_motion_free(GyroMotion *m);
/** One sample: gyro (deg/s), accelerometer (g), seconds since the last. */
void gyro_motion_add(GyroMotion *m, const float gyro[3], const float accel[3],
                     float dt);
/** The last sample, calibrated, in player space: pitch (about the pad's
 *  left-right axis, front up positive) and yaw (about the vertical whatever
 *  way the pad is held, left positive), degrees a second. */
void gyro_motion_player(GyroMotion *m, float *pitch, float *yaw);
/** Automatic calibration (stillness + sensor fusion) on or off. */
void gyro_motion_auto(GyroMotion *m, int on);
/** By hand: 1 = forget the calibration and start averaging (the pad lies
 *  still), 0 = stop and keep the average. */
void gyro_motion_calibrate(GyroMotion *m, int start);
/** The bias taken off (deg/s), the automatic calibration's confidence
 *  (0-1) and whether it sees the pad held still. */
void gyro_motion_state(GyroMotion *m, float offset[3], float *confidence,
                       int *steady);

#ifdef __cplusplus
}
#endif

#endif
