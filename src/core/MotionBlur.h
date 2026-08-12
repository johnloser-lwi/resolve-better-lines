#pragma once

// Motion blur, by the same principle as resolve-multi-transform's MotionBlur.h:
// the blur is ANALYTIC, not temporal. Every sample is the scene evaluated at a
// different fractional time, so no neighbouring frames are fetched and there is
// no dependence on the host's temporal clip access. That is only possible
// because the plugin owns its animation and can evaluate at any sub-frame time
// -- the payoff for not relying on host keyframes.
//
// The cost profile is NOT the same as the sibling's, and the difference decides
// the design. Multi Transform's shutter samples are cheap: each one re-samples
// the same source image through a different matrix, so N samples is N texture
// fetches per pixel and nothing else. This plugin GENERATES geometry, so each
// sample is a full scene rebuild -- evaluate points, flatten, trim, dash, emit
// primitives, bin the tile grid. Sixteen samples is sixteen of those.
//
// So adaptive sampling is load-bearing here rather than a refinement, and the
// static-frame collapse below is what keeps an always-on blur affordable.

#include "Common.h"

namespace bl {

/// Ceiling on shutter samples. Also bounds the per-render scene storage, which
/// is one full primitive list, tile grid and band grid per sample.
///
/// 64 matches the sibling plugin's ceiling. It is a genuinely different amount
/// of work here -- 64 geometry rebuilds rather than 64 texture fetches -- but a
/// ceiling exists to stop the arithmetic overflowing its storage, not to enforce
/// taste. The DEFAULT is low (8) and adaptive sampling collapses the common
/// cases; someone who wants 64 on a deliberate whip-pan should be able to ask.
constexpr int kMaxBlurSamples = 64;

struct BlurParams
{
    bool  enabled         = false;
    float shutterAngle    = 180.0f;  ///< degrees; 180 is the film convention
    float shutterPhase    = 0.0f;    ///< degrees; 0 centres the shutter on the frame
    int   samples         = 8;       ///< upper bound when adaptive, exact count otherwise
    bool  adaptive        = true;    ///< scale the count with how far the line actually moves
    float pixelsPerSample = 2.0f;    ///< adaptive density: larger is cheaper and coarser
};

/** @brief Time of shutter sample @p k of @p n, in frames.
 *
 * Samples sit at the MIDPOINT of each sub-interval rather than at its edges.
 * Edge sampling would put two samples at the shutter's extremes and so
 * double-weight them, which shows as faint hard lines at the head and tail of a
 * fast move. Lifted from the sibling plugin, where that artefact was found.
 */
BL_HD inline float BlurSampleTime(float t, const BlurParams& b, int k, int n)
{
    if (n <= 1) return t;

    const float shutterFrames = b.shutterAngle / 360.0f;
    const float phaseFrames   = b.shutterPhase / 360.0f;

    return t + phaseFrames + shutterFrames * ((static_cast<float>(k) + 0.5f)
                                              / static_cast<float>(n) - 0.5f);
}

/** @brief How many samples a given amount of on-screen movement deserves.
 *
 * Roughly one sample per @p pixelsPerSample of travel, so quality stays constant
 * instead of paying a fixed worst-case cost on every frame. A line standing
 * still needs one sample however high the slider is set -- which matters far
 * more here than in a transform plugin, because a wasted sample costs a whole
 * geometry rebuild rather than one texture fetch.
 *
 * @param motionPx  greatest distance anything drawn travels across the shutter
 */
inline int BlurSampleCount(const BlurParams& b, float motionPx)
{
    if (!b.enabled || b.shutterAngle <= 1e-4f || b.samples <= 1) return 1;

    const int maxSamples = b.samples < kMaxBlurSamples ? b.samples : kMaxBlurSamples;
    if (maxSamples <= 1) return 1;

    // Below a hundredth of a pixel nothing is moving, and averaging N identical
    // scenes is an expensive way to compute the frame unchanged. Applied even
    // when adaptive is off: a fixed 32-sample setting otherwise costs 32 scene
    // builds on a frame that is standing still.
    if (motionPx < 0.01f) return 1;

    if (!b.adaptive) return maxSamples;

    const float perSample = b.pixelsPerSample > 0.25f ? b.pixelsPerSample : 0.25f;
    int n = static_cast<int>(motionPx / perSample) + 1;
    if (n < 1)          n = 1;
    if (n > maxSamples) n = maxSamples;
    return n;
}

} // namespace bl
