// Debug free camera (F5). Two sides:
//  - window (wotm_freecam.inc, presentation thread): keys and mouse move the pose on every draw, using real time;
//  - EE thread (wotm_overrides.cpp): the pose goes into view 0's _cs after Cameras::Update and on entry to viewUpdate,
//    so the game itself sees the new camera (matrices, culling, LOD, particles, sound, camera-relative controls).
// The pose uses the game's convention (mathfRotationFromVector): heading around Z, 0 = +Y, positive toward +X; pitch up;
// no roll. The rows of the _cs matrix are right, forward and up.
#pragma once
#include <atomic>
#include <cmath>
#include <mutex>

#include "wotm_detmath.hpp"   // the axes go into the game camera (orbit, F5): same bits on every machine

struct WotmFreeCamPose {
    float pos[3] = {0, 0, 0};
    float heading = 0.f;   // _cs rot.z
    float pitch = 0.f;     // _cs rot.x
};

// Pose axes (rows of the _cs matrix, as mathfRotMatrixRPH with roll 0)
inline void wotmFreeCamAxes(const WotmFreeCamPose &p, float right[3], float fwd[3], float up[3]) {
    const float sh = wotm::detmath::sinf(p.heading), ch = wotm::detmath::cosf(p.heading), sp = wotm::detmath::sinf(p.pitch),
                cp = wotm::detmath::cosf(p.pitch);
    right[0] = ch; right[1] = -sh; right[2] = 0.f;
    fwd[0] = cp * sh; fwd[1] = cp * ch; fwd[2] = sp;
    up[0] = -sp * sh; up[1] = -sp * ch; up[2] = cp;
}

extern std::atomic<bool> g_wotmFreeCam;      // free camera on
extern std::atomic<bool> g_wotmFreeCamPad;   // while it's on: the pad goes back to the game (Tab) and the camera stays put on the keyboard
extern std::mutex g_wotmFreeCamM;
extern WotmFreeCamPose g_wotmFreeCamPose;    // guarded by g_wotmFreeCamM
