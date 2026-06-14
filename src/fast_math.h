#pragma once
/**
 * fast_math.h — Shared sine lookup table with linear interpolation.
 *
 * Provide two fast sine approximations:
 *   fastSinRadians(phase)   — phase in [0, 2π)
 *   fastSinNormalized(phase) — phase in [0, 1)
 *
 * Both use the same underlying LUT and linear interpolation.  The LUT is
 * built on first use (static local, zero-overhead singleton pattern).
 *
 * Usage:
 *   // Once at startup:
 *   fastMathInit();
 *
 *   // In audio loop:
 *   float s = fastSinNormalized(phase);
 *   float s = fastSinRadians(phase);
 */

#ifndef FAST_MATH_LUT_SIZE
#define FAST_MATH_LUT_SIZE 256
#endif

#include <cmath>
#include <cstddef>

// ---------------------------------------------------------------------------
// LUT storage and builder (internal)
// ---------------------------------------------------------------------------
namespace FastMathDetail {
    extern float s_sineLUT[FAST_MATH_LUT_SIZE];

    inline void buildSineLUT() {
        for (int i = 0; i < FAST_MATH_LUT_SIZE; ++i)
            s_sineLUT[i] = sinf(2.0f * (float)3.14159265358979323846 * i / FAST_MATH_LUT_SIZE);
    }

    inline bool& initialized() {
        static bool _inited = false;
        return _inited;
    }

    /** Thread-safe init-once.  Call once at startup. */
    inline void ensureInit() {
        if (!initialized()) {
            buildSineLUT();
            initialized() = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/** Call once at startup to build the LUT. */
inline void fastMathInit() {
    FastMathDetail::ensureInit();
}

/**
 * Fast sine via linear interpolation over the LUT.
 * @param phase Phase in [0, 2π).  0 = 0°, π = 180°, 2π = 360°.
 */
inline float fastSinRadians(float phase) {
    FastMathDetail::ensureInit();
    float idx  = phase * (FAST_MATH_LUT_SIZE / (2.0f * (float)3.14159265358979323846));
    int   i0   = (int)idx & (FAST_MATH_LUT_SIZE - 1);
    int   i1   = (i0 + 1) & (FAST_MATH_LUT_SIZE - 1);
    float frac = idx - (float)(int)idx;
    return FastMathDetail::s_sineLUT[i0] + frac * (FastMathDetail::s_sineLUT[i1] - FastMathDetail::s_sineLUT[i0]);
}

/**
 * Fast sine via linear interpolation over the LUT.
 * @param phase Phase in [0, 1).  0 = 0°, 0.5 = 180°, 1.0 = 360°.
 */
inline float fastSinNormalized(float phase) {
    FastMathDetail::ensureInit();
    float idx  = phase * (float)FAST_MATH_LUT_SIZE;
    int   i0   = (int)idx & (FAST_MATH_LUT_SIZE - 1);
    int   i1   = (i0 + 1) & (FAST_MATH_LUT_SIZE - 1);
    float frac = idx - (float)i0;
    return FastMathDetail::s_sineLUT[i0] + frac * (FastMathDetail::s_sineLUT[i1] - FastMathDetail::s_sineLUT[i0]);
}
