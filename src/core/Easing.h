#pragma once

// CSS-style cubic-bezier easing, lifted from resolve-multi-transform's
// AnimEngine.h because it is already the right answer and a second
// implementation would only be a second thing to get wrong.
//
// Resolve's OFX host supports no parametric (curve) parameters, so the host
// cannot own this animation. The plugin does.

#include "Common.h"

namespace bl {

/** @brief A cubic-bezier easing curve.
 *
 * Control points are P0=(0,0), P1=(x1,y1), P2=(x2,y2), P3=(1,1). Progress is the
 * x axis, eased output the y axis. y may legitimately leave [0,1] -- that is
 * what produces overshoot and anticipation, and is deliberately not clamped
 * here. Whether the RESULT may leave 0..1 is the caller's business, and for a
 * trim it may not.
 */
struct Easing
{
    float x1 = 0.0f, y1 = 0.0f, x2 = 1.0f, y2 = 1.0f;
};

enum EasePreset
{
    kEaseLinear = 0,
    kEaseSmooth,      ///< ease in and out
    kEaseIn,
    kEaseOut,
    kEaseBackOut,     ///< overshoots and settles
    kEaseBackIn,      ///< pulls back before moving
    kEaseCustom,
    kEasePresetCount
};

/** @brief Build a curve from four values a human can reason about.
 *
 * Raw bezier handles are precise and unusable as a UI: nobody thinks in terms of
 * "x1 = 0.42". These four are the controls every animation tool actually
 * exposes, and they map exactly onto the bezier.
 *
 * The 0.55 scale on the y handles is what makes 100% land on the classic CSS
 * "back" easing, cubic-bezier(.68, -.55, .27, 1.55).
 */
BL_HD inline Easing MakeEasing(float easeInPct, float easeOutPct,
                               float anticipationPct, float overshootPct)
{
    Easing e;
    e.x1 =        easeInPct  * 0.01f;
    e.x2 = 1.0f - easeOutPct * 0.01f;
    e.y1 =       -anticipationPct * 0.01f * 0.55f;
    e.y2 = 1.0f + overshootPct    * 0.01f * 0.55f;
    return e;
}

/** @brief The four human controls for a preset, as percentages. */
BL_HD inline void PresetValues(int preset, float& easeIn, float& easeOut,
                               float& anticipation, float& overshoot)
{
    easeIn = easeOut = anticipation = overshoot = 0.0f;
    switch (preset)
    {
        case kEaseSmooth:  easeIn = 42.0f; easeOut = 42.0f; break;
        case kEaseIn:      easeIn = 42.0f;                  break;
        case kEaseOut:                     easeOut = 42.0f; break;
        case kEaseBackOut: easeOut = 27.0f; overshoot = 100.0f; break;
        case kEaseBackIn:  easeIn  = 32.0f; anticipation = 100.0f; break;
        default: break;   // Linear, and Custom which keeps whatever is set
    }
}

namespace detail {

BL_HD inline float BezA(float p1, float p2) { return 1.0f - 3.0f * p2 + 3.0f * p1; }
BL_HD inline float BezB(float p1, float p2) { return 3.0f * p2 - 6.0f * p1; }
BL_HD inline float BezC(float p1)           { return 3.0f * p1; }

BL_HD inline float BezEval(float u, float p1, float p2)
{
    return ((BezA(p1, p2) * u + BezB(p1, p2)) * u + BezC(p1)) * u;
}

BL_HD inline float BezSlope(float u, float p1, float p2)
{
    return (3.0f * BezA(p1, p2) * u + 2.0f * BezB(p1, p2)) * u + BezC(p1);
}

/** @brief Solve x(u) = targetX for u.
 *
 * The bezier is parameterised by u, not by time, so every evaluation needs this
 * inversion first -- the same thing a browser does for CSS cubic-bezier().
 *
 * Newton first, because it converges in a couple of iterations for any sane
 * curve. Bisection second, because Newton diverges where the slope goes flat and
 * a render action is a bad place for a loop that might not terminate. Bisection
 * is slower and cannot fail.
 */
BL_HD inline float SolveBezierParam(float targetX, float x1, float x2)
{
    float u = targetX;

    for (int i = 0; i < 8; ++i)
    {
        const float err = BezEval(u, x1, x2) - targetX;
        if (err > -1e-6f && err < 1e-6f) return u;

        const float slope = BezSlope(u, x1, x2);
        if (slope > -1e-6f && slope < 1e-6f) break;

        u -= err / slope;
    }

    float lo = 0.0f, hi = 1.0f;
    u = Clamp(targetX, 0.0f, 1.0f);
    for (int i = 0; i < 24; ++i)
    {
        const float x = BezEval(u, x1, x2);
        if (x > targetX - 1e-6f && x < targetX + 1e-6f) break;
        if (x > targetX) hi = u; else lo = u;
        u = 0.5f * (lo + hi);
    }
    return u;
}

} // namespace detail

/** @brief Apply an easing curve to progress in [0,1].
 *
 * @return the eased value, which MAY leave [0,1] where the curve anticipates or
 *         overshoots. That is the point of those controls, so it is not clamped.
 */
BL_HD inline float ApplyEasing(float p, const Easing& e)
{
    p = Saturate(p);
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;

    // Linear is both the common case and exactly the one where the solver is
    // pointless.
    if (e.x1 == e.y1 && e.x2 == e.y2) return p;

    return detail::BezEval(detail::SolveBezierParam(p, e.x1, e.x2), e.y1, e.y2);
}

} // namespace bl
