/* A C front for GamepadMotionHelpers (gyro_motion.h). */

#include <new>
#include "gyro_motion.h"
#include "GamepadMotion.hpp"

struct GyroMotion {
    GamepadMotion m;
};

extern "C" GyroMotion *gyro_motion_new(void)
{
    GyroMotion *g = new (std::nothrow) GyroMotion();
    if (g)
        gyro_motion_auto(g, 1);
    return g;
}

extern "C" void gyro_motion_free(GyroMotion *g)
{
    delete g;
}

extern "C" void gyro_motion_add(GyroMotion *g, const float gyro[3],
                                const float accel[3], float dt)
{
    g->m.ProcessMotion(gyro[0], gyro[1], gyro[2],
                       accel[0], accel[1], accel[2], dt);
}

extern "C" void gyro_motion_player(GyroMotion *g, float *pitch, float *yaw)
{
    float x, y;
    g->m.GetPlayerSpaceGyro(x, y);
    *pitch = x;
    *yaw = y;
}

extern "C" void gyro_motion_auto(GyroMotion *g, int on)
{
    g->m.SetCalibrationMode(on
        ? GamepadMotionHelpers::Stillness | GamepadMotionHelpers::SensorFusion
        : GamepadMotionHelpers::Manual);
}

extern "C" void gyro_motion_calibrate(GyroMotion *g, int start)
{
    if (start) {
        g->m.ResetContinuousCalibration();
        g->m.StartContinuousCalibration();
    } else {
        g->m.PauseContinuousCalibration();
    }
}

extern "C" void gyro_motion_state(GyroMotion *g, float offset[3],
                                  float *confidence, int *steady)
{
    g->m.GetCalibrationOffset(offset[0], offset[1], offset[2]);
    *confidence = g->m.GetAutoCalibrationConfidence();
    *steady = g->m.GetAutoCalibrationIsSteady() ? 1 : 0;
}
