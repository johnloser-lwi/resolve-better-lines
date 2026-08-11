#pragma once

// The style of a stroke, already resolved for one frame.
//
// Everything here is in PIXELS and 0..1 colour, not in parameter units. The
// conversion from "% of frame height" happens once, at the OFX boundary, so
// neither the rasterizer nor the offline harness has any opinion about
// resolution -- which is what keeps a design authored at 1080p landing
// identically at UHD.

#include "Common.h"

namespace bl {

struct Color
{
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    BL_HD Color() {}
    BL_HD Color(float cr, float cg, float cb, float ca) : r(cr), g(cg), b(cb), a(ca) {}
};

BL_HD inline Color LerpColor(const Color& x, const Color& y, float t)
{
    return Color(Lerp(x.r, y.r, t), Lerp(x.g, y.g, t),
                 Lerp(x.b, y.b, t), Lerp(x.a, y.a, t));
}

/** @brief Paint a single subpath carries with it, as authored.
 *
 * Imported artwork tells us what it wants: this path is a black fill, that one
 * is a 40-wide stroke. Discarding it and applying one global style is what
 * turns a three-tone logo into a silhouette.
 *
 * Carried per SUBPATH rather than per parameter slot, because an import can be
 * twenty subpaths and parameters cannot.
 */
struct StrokePaint
{
    bool  hasStroke   = false;
    bool  hasFill     = false;
    Color strokeColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
    Color fillColor   = Color(1.0f, 1.0f, 1.0f, 1.0f);

    /// Stroke width in the same normalised units as the points, so it survives
    /// a change of resolution exactly as the geometry does.
    float width = 0.0f;

    /// Which drawn SHAPE this contour belongs to.
    ///
    /// A single SVG <path> often holds several subpaths that are one shape: an
    /// outer contour and an inner one making a ring, or a letter and its
    /// counter. Their windings have to be combined, or the inner contour stops
    /// cutting a hole and the outer one fills solid -- hiding everything the
    /// hole was there to reveal.
    int group = 0;
};

enum CapStyle { kCapRound = 0, kCapButt = 1, kCapSquare = 2 };

enum GradMode
{
    kGradNone   = 0,
    kGradAlong  = 1,   ///< by arc length, start of path to end
    kGradAcross = 2,   ///< one edge to the other, across the stroke
    kGradCentre = 3    ///< centreline outwards to both edges
};

/** @brief Map a raw 0..1 gradient coordinate through the ramp controls.
 *
 * @p start and @p end place and scale the ramp; everything outside them is flat,
 * so the gradient can be squeezed into a narrow band or spread past the ends of
 * the stroke. They are allowed to run backwards -- @p end below @p start simply
 * reverses the ramp, which saves a separate invert control.
 *
 * @p mid biases the halfway point, the way a Photoshop gradient midpoint does:
 * the exponent is chosen so that the ramp passes through exactly 0.5 there.
 */
BL_HD inline float GradientT(float raw, float start, float end, float mid)
{
    const float span = end - start;

    // A zero-width ramp is a hard edge at that position rather than a division
    // by zero. Reachable by dragging the two sliders together, so it has to mean
    // something.
    float t = (::fabsf(span) < 1e-6f)
                  ? ((raw < start) ? 0.0f : 1.0f)
                  : Saturate((raw - start) / span);

    const float m = Clamp(mid, 0.02f, 0.98f);
    if (::fabsf(m - 0.5f) > 1e-4f) t = ::powf(t, ::logf(0.5f) / ::logf(m));

    return t;
}

/** @brief Width multiplier at normalised arc position @p s, from three stops.
 *
 * A quadratic Bezier rather than two straight lerps joined at the middle, and
 * the choice buys two things that matter:
 *
 *  - It is smooth. Two lerps meeting at s = 0.5 leave a corner in the
 *    silhouette, so a leaf shape comes out as a hard-edged rhombus.
 *  - It still reduces EXACTLY to a straight taper. Set @p wMid to the average of
 *    the two ends and the curve collapses to the linear interpolation it
 *    replaced, so nothing that worked before behaves differently now.
 *
 * The control point is derived so that @p wMid is literally the width at the
 * midpoint, rather than the Bezier control value it would otherwise be -- a
 * control point sits off the curve, and a control labelled "Body" that the line
 * never actually reaches would be a poor thing to hand somebody.
 */
BL_HD inline float WidthProfile(float s, float wStart, float wMid, float wEnd)
{
    const float ctrl = 2.0f * wMid - 0.5f * (wStart + wEnd);
    const float u    = 1.0f - s;
    // Clamped: a body much thinner than the ends drives the control point
    // negative, and a negative radius would invert the round cone rather than
    // pinch it.
    return Max(u * u * wStart + 2.0f * u * s * ctrl + s * s * wEnd, 0.0f);
}

/** @brief Style resolved for one stroke at one frame. */
struct Style
{
    // --- core stroke ---------------------------------------------------------
    float halfWidth = 4.0f;    ///< pixels
    float feather   = 1.0f;    ///< pixels of soft edge, each side of the boundary
    Color color     = Color(1.0f, 1.0f, 1.0f, 1.0f);
    float opacity   = 1.0f;
    int   cap       = kCapRound;

    // --- taper ---------------------------------------------------------------
    // Three width stops along the arc length: the two ends and the middle.
    //
    // The middle stop is not a luxury. With only two, setting both ends thin
    // interpolates thin-to-thin and the line disappears completely -- so the one
    // shape a taper control most obviously ought to make, a stroke that swells
    // through its body and points at both tips, was the one shape it could not.
    float widthStart = 1.0f;
    float widthMid   = 1.0f;
    float widthEnd   = 1.0f;

    // --- outline -------------------------------------------------------------
    bool  outline      = false;
    float outlineWidth = 2.0f;   ///< pixels, added outside halfWidth
    Color outlineColor = Color(0.0f, 0.0f, 0.0f, 1.0f);

    // --- glow ----------------------------------------------------------------
    // A distance threshold, not a blur. The SDF already knows how far every pixel
    // is from the line, so an exact falloff costs one smoothstep -- no scratch
    // buffer, no separable pass, and no halo clipped at the tile edge.
    bool  glow          = false;
    float glowRadius    = 12.0f;
    float glowIntensity = 1.0f;
    Color glowColor     = Color(1.0f, 1.0f, 1.0f, 1.0f);

    // --- drop shadow ---------------------------------------------------------
    // Evaluated by asking the SAME distance field where the line is at
    // (p - offset). No blur pass, no scratch buffer, and the falloff is exact
    // rather than a stack of box blurs pretending to be a gaussian.
    bool  shadow        = false;
    float shadowDx      = 0.0f;   ///< pixels
    float shadowDy      = 0.0f;
    float shadowSoft    = 6.0f;   ///< pixels of falloff beyond the silhouette
    float shadowOpacity = 0.6f;
    Color shadowColor   = Color(0.0f, 0.0f, 0.0f, 1.0f);

    // --- arrowheads ----------------------------------------------------------
    // Built as ordinary segments, not as a new primitive type: a round cone with
    // a full-width base and a zero-width tip already IS a triangle with a
    // rounded back, and the existing flat-cap machinery squares that back off.
    // A dedicated triangle SDF would mean a second primitive type, a type tag on
    // every segment, and a branch in the innermost loop -- for a shape the
    // existing primitive draws exactly.
    bool  arrowStart  = false;
    bool  arrowEnd    = false;
    float arrowLength = 0.0f;   ///< pixels, tip to base
    float arrowWidth  = 0.0f;   ///< pixels, full width at the base
    float arrowRound  = 0.0f;   ///< pixels, corner radius

    /// How much of the head sits BEYOND the path end: 0 puts the tip exactly on
    /// it, 0.5 centres the triangle on it, 1 puts the BASE there so the head
    /// carries on past where the line stops.
    ///
    /// 1 is the default, because it is the only one that reads as the line
    /// continuing into a point. With the tip on the end the whole head hangs off
    /// the back of the line, and on a curve -- where the direction at the final
    /// point has already swung away from the direction the line appears to be
    /// travelling -- it looks like an arrow that has slid off the end it belongs
    /// to. Nothing about the maths is wrong there; it just does not read.
    float arrowPosition = 1.0f;

    // --- fill ----------------------------------------------------------------
    // Closed strokes only. Uses the whole closed contour, NOT the trimmed or
    // dashed geometry: a trim is a reveal of the line, and a shape whose fill
    // came and went with it would be a different shape every frame rather than
    // one being drawn. Animate Fill Opacity for that.
    bool  fill        = false;
    float fillOpacity = 1.0f;
    Color fillColor   = Color(0.2f, 0.4f, 0.9f, 1.0f);

    // --- gradient ------------------------------------------------------------
    int   gradMode  = kGradNone;
    Color gradColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
    float gradStart = 0.0f;
    float gradEnd   = 1.0f;
    float gradMid   = 0.5f;

    /// Fold the Along gradient so it runs out and back, reaching the far colour
    /// at the MIDDLE of the path and returning to the near colour at the end.
    ///
    /// For a closed path this is the difference between a gradient and a visible
    /// seam. Arc length runs 0..1 from the start point round to the start point
    /// again, so an unfolded ramp puts its two ENDS next to each other at a
    /// place the shape has no edge -- a hard join across the middle of a
    /// continuous contour. Folded, both ends of the ramp are the same colour, so
    /// the seam closes into itself and the only thing left is the gradient.
    bool  gradLoop  = false;

    /** @brief Largest distance from the centreline at which this style still
     *         paints anything.
     *
     * The tile grid expands every primitive's bounds by this, so a pixel is only
     * ever asked about primitives that could actually reach it. Getting it too
     * small is the bug where a glow is sliced off along invisible 16-pixel
     * boundaries; too large only costs speed, so every contributor is included
     * rather than assumed negligible.
     */
    BL_HD float MaxReach() const
    {
        float r = feather;
        if (outline) r = Max(r, outlineWidth + feather);
        if (glow)    r = Max(r, glowRadius);
        if (shadow)
        {
            // The shadow is read at (p - offset), so a segment influences pixels
            // a whole offset away from itself. Binning it only by its own extent
            // would clip the shadow along tile boundaries -- the same failure as
            // an under-reported glow radius, just displaced.
            const float off = ::sqrtf(shadowDx * shadowDx + shadowDy * shadowDy);
            r = Max(r, off + shadowSoft + feather);
        }
        return r;
    }
};

} // namespace bl
