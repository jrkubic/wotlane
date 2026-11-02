// ABOUTME: Deterministic float sinf/cosf/atanf/atan2f for native code whose results reach the game (the orbit camera writes its pose
// ABOUTME: into guest RAM): the same bits on every compiler and CPU, unlike the system libm (Microsoft's and Apple's differ).
#pragma once

// Defined in wotm_detmath.cpp, compiled on its own with the strict float rules: as inline functions, a caller built with fast math
// (ps2_runtime.cpp includes wotm_freecam.h) could hand the linker a contracted copy that every other caller would then share.
// sinf/cosf equal the retail game's values bit for bit (wotm_trig_goldens.hpp); all four stay within 1 ulp of the exact result.
namespace wotm::detmath
{
    float sinf(float x);
    float cosf(float x);
    float atanf(float x);
    float atan2f(float y, float x);
}
