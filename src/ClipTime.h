#pragma once

#include "HostProbe.h"
#include "ofxsImageEffect.h"

#include <sstream>

namespace bl {

/** @brief How the render time relates to the clip's first frame.
 *
 * This is the least trustworthy thing the host reports, and the two sibling
 * plugins reached OPPOSITE conclusions about it:
 *
 *  - resolve-multi-transform subtracts timeLineGetBounds' t1 from the render
 *    time, because its host hands out absolute timeline frames (~108000).
 *  - resolve-text-animator concluded that Resolve already hands the plugin
 *    clip-relative time and that subtracting anything moves the animation off
 *    the clip's first frame by exactly that amount.
 *
 * Both are right, on different clip types. Measured here: on a FUSION clip the
 * render time runs in the composition's own small range and reads as
 * clip-relative, which is why animation worked there. On a still, an adjustment
 * clip or ordinary footage it is a timeline frame in the tens of thousands --
 * and then an entrance of 24 frames evaluates to progress 4500, clamps to 1, and
 * the line is simply fully drawn on every frame. The effect appears to do
 * nothing at all, which is exactly how it was reported.
 *
 * So neither answer can be hard-coded. It is detected, and overridable.
 */
enum TimeOrigin
{
    kOriginAuto = 0,
    kOriginRelative,   ///< args.time already counts from the clip's first frame
    kOriginAbsolute,   ///< args.time is a timeline frame; subtract the clip start
    kOriginCount
};

struct ClipTiming
{
    double clipFrame = 0.0;   ///< frames since the clip's first visible frame
    double length    = 0.0;   ///< frames; 0 when unknown
    bool   absolute  = false; ///< whether a clip start was subtracted
    bool   suspect   = false; ///< the result does not look like a clip frame
};

/** @brief Is a reported frame range worth believing? */
inline bool ValidClipRange(double t1, double t2)
{
    // NaN fails every comparison, so it has to be rejected explicitly rather
    // than by any of the range tests below.
    if (!(t1 == t1) || !(t2 == t2)) return false;

    const double span = t2 - t1;
    if (span <= 0.0) return false;

    // Resolve reports a sentinel range of exactly 1000 minutes on some clips.
    // Believing it makes a clip-end anchor point somewhere in the next hour.
    if (span > 1.0e6) return false;

    return true;
}

/** @brief Resolve clip timing for this render.
 *
 * @param origin        one of TimeOrigin
 * @param manualOffset  frames added after the origin is resolved
 * @param manualLength  clip length override in frames; <= 0 means auto-detect
 */
inline ClipTiming GetClipTiming(OFX::ImageEffect* effect, double argsTime,
                                int origin, double manualOffset, double manualLength)
{
    double t1 = 0.0, t2 = 0.0;
    bool   got = false;
    try { effect->timeLineGetBounds(t1, t2); got = true; }
    catch (...) { got = false; }

    const bool boundsOk = got && ValidClipRange(t1, t2);

    // --- length --------------------------------------------------------------
    ClipTiming out;
    double duration = 0.0;
    if (manualLength > 0.5)
    {
        out.length = manualLength;
    }
    else if (boundsOk)
    {
        out.length = t2 - t1 + 1.0;
    }
    else
    {
        // Last resort. getEffectDuration reports the AVAILABLE span rather than
        // the visible one, so it is wrong -- but a clip-end anchor against a
        // slightly wrong length still animates, where against no length at all
        // it is skipped entirely.
        try { duration = effect->getEffectDuration(); } catch (...) { duration = 0.0; }
        if (duration > 1.0 && duration < 1.0e6) out.length = duration;
    }

    // --- origin --------------------------------------------------------------
    bool absolute = false;
    if      (origin == kOriginAbsolute) absolute = true;
    else if (origin == kOriginRelative) absolute = false;
    else
    {
        // Auto. The render time sitting INSIDE the reported bounds, with those
        // bounds well away from zero, is the signature of a timeline frame:
        // a clip-relative time would be somewhere near 0 and nowhere near t1.
        absolute = boundsOk && t1 > 1.0 &&
                   argsTime > t1 - 1.0 && argsTime < t2 + 1.0;
    }

    out.absolute  = absolute && boundsOk;
    out.clipFrame = (out.absolute ? argsTime - t1 : argsTime) + manualOffset;

    // A clip frame this large is not a clip frame. Either the origin could not
    // be established or the host is reporting something unexpected; either way
    // every stage will clamp instantly and the effect will look inert.
    out.suspect = (out.clipFrame > 100000.0);

    // Built only once. ProbeOnce already refuses to log twice, but the message
    // was being FORMATTED on every render before it got the chance -- a string
    // stream, several allocations and a dozen conversions per frame, thrown away.
    static bool logged = false;
    if (!logged)
    {
        logged = true;

        std::ostringstream o;
        o << "cliptime: args.time=" << argsTime
          << " bounds=" << (got ? "ok" : "THREW")
          << " t1=" << t1 << " t2=" << t2
          << " valid=" << (boundsOk ? "yes" : "NO")
          << " effectDuration=" << duration
          << " | origin=" << (out.absolute ? "ABSOLUTE (subtracted t1)" : "relative")
          << " offset=" << manualOffset
          << " -> clipFrame=" << out.clipFrame
          << " length=" << out.length;
        if (out.suspect)
            o << "\n    SUSPECT: clipFrame is far too large to be a frame within a clip."
                 " Set Time Origin to Timeline Absolute, or set Time Offset by hand.";
        ProbeOnce("cliptime", o.str());
    }

    return out;
}

} // namespace bl
