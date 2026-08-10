#pragma once

// Plugin-owned in/out animation, anchored to the clip.
//
// The point of this over plain keyframes is that it needs none: drop the effect
// on a clip of any length and the line draws itself on at the head and off at
// the tail. Keyframes still work -- every parameter here is a normal OFX
// parameter -- but nothing has to be keyed for the common case.
//
// All timing is in FRAMES. There is deliberately no frame-rate helper anywhere
// in this project: Fusion does not publish kOfxImageEffectPropFrameRate on
// clips, and the Support library THROWS when a property is missing. Thrown out
// of the render action that becomes kOfxStatErrMissingHostFeature, failing every
// single frame -- the effect simply appears to do nothing.

#include "Common.h"
#include "Easing.h"
#include "Style.h"
#include "Wiggle.h"   // detail::Hash32, for the deterministic stagger shuffle

namespace bl {

/** @brief The visible span of a path, as fractions of its arc length.
 *
 * Lives here rather than with the geometry because it is animation state that
 * the geometry merely consumes.
 */
struct TrimState
{
    float start  = 0.0f;
    float end    = 1.0f;
    float offset = 0.0f;   ///< slides the window along the path

    /// Whether a closed path's trim window wraps past the seam.
    ///
    /// Wrapping is what a closed path usually wants -- a ring drawing itself on
    /// should carry on round rather than stop at an arbitrary seam. But it makes
    /// one common move impossible: sliding a short window with Offset from -1 to
    /// 1 to fly a segment along the path and off the end. Wrapped, the window
    /// can never leave, so the segment just circles forever and the animation
    /// has no beginning or end.
    ///
    /// With this off a closed path trims exactly like an open one, and the same
    /// Offset sweep works on both.
    bool wrap = true;
};

/** @brief What a stage drives.
 *
 * A choice rather than a fixed "trim end draws on", because the same in/out
 * machinery is what makes a line draw itself, a highlight fade up, a dash
 * pattern march, or a stroke thicken on arrival. Fixing the target would mean
 * building the timing four more times.
 */
enum AnimTarget
{
    kTargetTrimEnd = 0,   ///< the classic draw-on
    kTargetTrimStart,     ///< the line retreats from its own tail
    kTargetTrimOffset,    ///< the whole visible span slides along the path
    kTargetOpacity,
    kTargetThickness,     ///< % of frame height
    kTargetDashOffset,    ///< % of frame height; animate for marching ants
    kTargetFillOpacity,
    kTargetCount
};

/** @brief What a stage's frame numbers are measured from.
 *
 * Expressed as an affine change of variable, local = (t - offset) * scale, which
 * is the whole of it. Multi Transform arrived at the same three.
 */
enum TimeAnchor
{
    kAnchorClipStart = 0,   ///< frame 0 is the clip's first frame
    kAnchorClipEnd,         ///< frame 0 is the clip's LAST frame, so times run negative
    kAnchorStretch,         ///< times are a percentage of the clip's length
    kAnchorCount
};

struct Stage
{
    bool   enabled    = false;
    int    target     = kTargetTrimEnd;
    float  from       = 0.0f;
    float  to         = 1.0f;
    float  startFrame = 0.0f;
    float  duration   = 24.0f;
    int    anchor     = kAnchorClipStart;
    Easing easing;
};

/** @brief Convert clip time into a stage's own timebase.
 *
 * @param clipFrame frames since the clip's first visible frame
 * @param length    clip length in frames; 0 when unknown
 *
 * The `length - 1` in the last two cases is deliberate and was learned the hard
 * way in Multi Transform. A clip spanning `length` frames has its last visible
 * frame at `length - 1`; using the length itself puts the reference one frame
 * past the end, so an outro written "-20 -> 0" finishes fractionally short of
 * where it was asked to.
 */
BL_HD inline float StageLocalTime(const Stage& s, float clipFrame, float length)
{
    const float last = (length > 1.0f) ? (length - 1.0f) : 0.0f;

    switch (s.anchor)
    {
        case kAnchorClipEnd:  return clipFrame - last;
        case kAnchorStretch:  return (last > 0.0f) ? (clipFrame * 100.0f / last) : 0.0f;
        default:              return clipFrame;
    }
}

/** @brief Eased progress of a stage at a given clip frame, in [0,1]. */
BL_HD inline float StageProgress(const Stage& s, float clipFrame, float length)
{
    const float t = StageLocalTime(s, clipFrame, length);

    // A zero or negative duration is an instant cut at the start frame rather
    // than a division by zero or a backwards animation. Both are reachable by
    // dragging a slider, so both have to mean something.
    if (s.duration <= 1e-6f) return (t < s.startFrame) ? 0.0f : 1.0f;

    return ApplyEasing((t - s.startFrame) / s.duration, s.easing);
}

/** @brief The value a stage is producing at this frame. */
BL_HD inline float StageValue(const Stage& s, float clipFrame, float length)
{
    return Lerp(s.from, s.to, StageProgress(s, clipFrame, length));
}

/** @brief Can this stage be evaluated at all?
 *
 * Clip End and Stretch both measure from the clip's LAST frame, so neither means
 * anything without a length. Evaluating them anyway is not a small error, it is
 * a catastrophic one: with length 0 the anchor silently collapses to Clip Start,
 * and an exit written "start -20, duration 20" is then already COMPLETE at frame
 * zero -- so the line is fully retracted on every frame and the effect appears
 * to do nothing whatsoever.
 *
 * Reported from an adjustment clip, where the host gives no usable bounds.
 * Skipping the stage degrades to "the exit never runs", which leaves the line
 * visible and obviously unanimated -- a failure someone can see and describe,
 * rather than a blank frame that looks like the plugin is broken.
 */
BL_HD inline bool StageUsable(const Stage& s, float length)
{
    if (!s.enabled || s.target < 0 || s.target >= kTargetCount) return false;
    if (s.anchor != kAnchorClipStart && !(length > 1.0f)) return false;
    return true;
}

/** @brief Apply both stages to whichever targets they drive.
 *
 * @param value  in/out: indexed by AnimTarget, seeded with the parameter values
 *               the user set, and overwritten by any stage that drives them.
 *
 * A stage applies at EVERY frame, holding `from` before its window and `to`
 * after. Skipping it outside the window looks tidier and is wrong: an entrance
 * that only applied once it had started would leave the line fully drawn on
 * every frame before it, which is the one thing an entrance must not do.
 *
 * Out is applied second, so where both drive the same target the exit wins once
 * it begins. Which stage is running comes from its own window -- never from
 * comparing eased values. An overshooting entrance sits above 1 while a dormant
 * exit rests at exactly 1, so "whichever is further from settled" hands the
 * frame to the stage doing nothing and flattens the overshoot away.
 */
BL_HD inline void ApplyStages(const Stage& in, const Stage& out,
                              float clipFrame, float length,
                              float* value)
{
    // Clamped HERE, on the way out of a stage, rather than in WriteTargets.
    //
    // The clamp exists for eased overshoot: a trim past the end of the path is
    // meaningless and a negative one inverts the cut. But applying it to every
    // value on the way through would clamp values the user set by hand too, and
    // Trim Start and End are deliberately given a range beyond 0..1 so a closed
    // path can be wound past its own seam. Manual and keyframed values must come
    // through untouched -- with Animate off this whole system is inert, and that
    // is the primary way the plugin is used.
    auto write = [&](const Stage& s) {
        float v = StageValue(s, clipFrame, length);
        if (s.target == kTargetTrimEnd || s.target == kTargetTrimStart)
            v = Clamp(v, 0.0f, 1.0f);
        value[s.target] = v;
    };

    if (StageUsable(in,  length)) write(in);
    if (StageUsable(out, length)) write(out);
}

/** @brief The order strokes take their turn in when staggered. */
enum StaggerOrder
{
    kOrderForward = 0,
    kOrderReverse,
    kOrderCentreOut,
    kOrderRandom,
    kOrderCount
};

/** @brief Which turn stroke @p i takes, in 0..count-1.
 *
 * Stagger is the one piece of animation manual keyframing cannot replace. Trim
 * is a single window shared by every stroke, so there is no parameter to key
 * that would draw stroke one and then stroke two -- the effect would need a trim
 * per stroke, which is a parameter slot per stroke that nobody would ever key by
 * hand.
 *
 * Random is a deterministic shuffle from the seed rather than a call to any
 * random source: the same seed has to give the same order on every frame, or the
 * strokes reshuffle themselves during playback.
 */
inline int StaggerIndex(int i, int count, int order, unsigned seed)
{
    if (count <= 1 || i < 0) return 0;
    if (i >= count) return count - 1;

    switch (order)
    {
        case kOrderReverse:
            return count - 1 - i;

        case kOrderCentreOut:
        {
            // Distance from the middle, so the middle stroke goes first and the
            // two ends arrive together.
            const float mid = 0.5f * static_cast<float>(count - 1);
            const float d   = ::fabsf(static_cast<float>(i) - mid);
            return static_cast<int>(d + 0.5f);
        }

        case kOrderRandom:
        {
            // A hash of the index, ranked against every other index's hash. This
            // is a genuine permutation, and it is stable because it depends on
            // nothing but i, count and seed.
            const unsigned mine = detail::Hash32(static_cast<unsigned>(i) * 2654435761u + seed);
            int rank = 0;
            for (int k = 0; k < count; ++k)
            {
                if (k == i) continue;
                const unsigned h = detail::Hash32(static_cast<unsigned>(k) * 2654435761u + seed);
                // Ties broken by index so the ranking is a strict order and no
                // two strokes can claim the same turn.
                if (h < mine || (h == mine && k < i)) ++rank;
            }
            return rank;
        }

        default:
            return i;
    }
}

/** @brief Everything the stages can drive, in each parameter's own units. */
struct AnimTargets
{
    float v[kTargetCount] = {};
};

/** @brief Seed the targets from the values the user set.
 *
 * Seeding everything and then letting stages overwrite what they drive is what
 * lets any target stay under manual or keyframed control simply by not being a
 * stage's target. Branching on "is this animated" instead would mean the
 * unanimated case took a different path, and the two would drift.
 *
 * @param thicknessPct  and @p dashPhasePct are in PARAMETER units (% of frame
 *        height), not pixels, because that is what the user types into From and
 *        To. Converting before the stages run would silently change what those
 *        numbers mean.
 */
inline void SeedTargets(const Style& st, const TrimState& trim,
                        float thicknessPct, float dashPhasePct, AnimTargets& a)
{
    a.v[kTargetTrimEnd]     = trim.end;
    a.v[kTargetTrimStart]   = trim.start;
    a.v[kTargetTrimOffset]  = trim.offset;
    a.v[kTargetOpacity]     = st.opacity;
    a.v[kTargetThickness]   = thicknessPct;
    a.v[kTargetDashOffset]  = dashPhasePct;
    a.v[kTargetFillOpacity] = st.fillOpacity;
}

/** @brief Write the driven values back, converting to pixels where needed.
 *
 * Deliberately an identity when no stage ran: trim passes through unclamped
 * (ApplyStages clamps eased overshoot at source instead), and thickness is
 * simply re-derived from the same percentage it was seeded with. With Animate
 * off the whole system has to be inert, because manual and keyframed control is
 * the primary way this plugin is used -- particularly inside Fusion, which has a
 * far better curve editor than an OFX overlay could offer.
 */
inline void WriteTargets(const AnimTargets& a, float pctToPx,
                         Style& st, TrimState& trim, float& dashPhasePx)
{
    trim.end    = a.v[kTargetTrimEnd];
    trim.start  = a.v[kTargetTrimStart];
    trim.offset = a.v[kTargetTrimOffset];

    st.opacity     = Clamp(a.v[kTargetOpacity], 0.0f, 1.0f);
    st.fillOpacity = Clamp(a.v[kTargetFillOpacity], 0.0f, 1.0f);
    st.halfWidth   = Max(a.v[kTargetThickness] * pctToPx * 0.5f, 0.0f);

    dashPhasePx = a.v[kTargetDashOffset] * pctToPx;
}

} // namespace bl
