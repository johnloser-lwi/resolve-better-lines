#pragma once

// The drawable primitives and their signed distance functions.
//
// The whole plugin reduces to: build a list of these on the CPU, then ask each
// pixel for its distance to the nearest one. Thickness, feather, outline, glow,
// gradient and fill are then all thresholds on that single distance value --
// which is the reason for choosing an SDF over a scanline rasterizer. A glow
// costs an extra smoothstep, not a separable blur pass and a scratch buffer.

#include "Common.h"

namespace bl {

/** @brief One flattened path segment: a capsule whose two ends have different radii.
 *
 * A varying radius is what makes taper fall out for free -- a constant-width line
 * is just the case ra == rb, and costs nothing extra. Every join is implicitly
 * round, because the union of two overlapping capsules is exactly a round join.
 *
 * @c s0 / @c s1 are the normalised arc-length positions (0..1 along the whole
 * stroke) of the two ends, carried so the shading pass can answer "how far along
 * the line is this pixel" for a gradient without re-deriving it.
 */
struct Segment
{
    /** Flat-cap flags. A round cap needs no flag because it is what a capsule
     *  already is; butt and square caps are the ones that need the bulge cut
     *  off, by a half-plane at the end of the RUN.
     *
     *  The plane belongs to the run, not to this segment, and it is applied to
     *  every segment near enough to that end to protrude through it.
     *
     *  Setting it only on the run's two terminal segments looks obviously
     *  sufficient and is not. It holds only while a segment is longer than the
     *  line is thick; a flattened curve produces segments a few pixels long
     *  against a radius many times that, so each interior capsule is a fat blob
     *  bulging straight through the flat end. Measured as butt-capped dashes
     *  that were clean on straight lines and lumpy and rounded on curves --
     *  which is exactly the shape of "only the terminal segment was clipped". */
    /// kTriangle switches the primitive from a round cone to a genuine rounded
    /// triangle over vertices a/b/c, with @c ra as the corner radius.
    ///
    /// An arrowhead was originally a cone -- a full-width base tapering to a
    /// zero-width tip -- on the reasoning that this is already triangular. It is
    /// not. The flanks of a cone are the tangent lines from the tip to the base
    /// circle, and those touch the circle BEHIND the base plane, so cutting the
    /// back off flat leaves a short arc of that circle on each side instead of a
    /// straight edge. The result has a visible kink where arc meets cut, which
    /// is exactly the "weird edge" it was reported as.
    enum : unsigned { kCapA = 1u, kCapB = 2u, kTriangle = 4u };

    Vec2  a;
    Vec2  b;
    Vec2  c;                  ///< third vertex; unused unless kTriangle
    float ra = 1.0f;          ///< segment: radius at a. triangle: corner radius
    float rb = 1.0f;
    float s0 = 0.0f;
    float s1 = 1.0f;

    /// Outward half-plane normals, and a point ON each plane (the run's end).
    /// Clipped where Dot(n, p - planePoint) > 0.
    ///
    /// Stored as a point rather than the algebraically tidier scalar offset
    /// Dot(n, planePoint). The offset form subtracts two numbers of the order of
    /// the frame size to produce a distance of the order of a pixel, which is
    /// catastrophic cancellation: float epsilon on a coordinate of 200 is
    /// already 2e-5, and it grows with resolution. Measured as a CPU/GPU parity
    /// failure of 1.7e-5 on butt-capped dashes -- the two compilers rounding the
    /// same doomed subtraction differently. Subtracting the positions FIRST is
    /// exact for nearby coordinates and costs exactly the same arithmetic.
    Vec2  na, nb;
    Vec2  capA, capB;
    unsigned flags = 0;

    /// Which stroke's style this belongs to.
    ///
    /// Needed the moment strokes are staggered: a stagger means each stroke is
    /// at a different point in the same animation, so an animated opacity or
    /// thickness is genuinely a different value per stroke. One shared style
    /// cannot express that, and the strokes would all animate together -- which
    /// is precisely what stagger exists to avoid.
    int style = 0;

    /** Bounds of the drawn shape, before any style expansion. */
    BL_HD Bounds BoundsOf() const
    {
        Bounds bb;
        if (flags & kTriangle)
        {
            bb.Add(a); bb.Add(b); bb.Add(c);
            bb.Expand(ra);
            return bb;
        }
        bb.Add(Vec2(a.x - ra, a.y - ra));
        bb.Add(Vec2(a.x + ra, a.y + ra));
        bb.Add(Vec2(b.x - rb, b.y - rb));
        bb.Add(Vec2(b.x + rb, b.y + rb));
        return bb;
    }
};

/** @brief Result of a distance query: how far, how far along, and which side. */
struct DistanceHit
{
    float d = 1e30f;   ///< signed distance; negative is inside
    float s = 0.0f;    ///< normalised arc-length position at the closest point

    /// SIGNED position across the stroke: -1 at one edge, +1 at the other.
    ///
    /// Deliberately not derivable from @c d. The distance is unsigned across the
    /// width -- it cannot tell the two sides of a line apart -- so a gradient
    /// driven by it is symmetric about the centreline whatever else you do to
    /// it, and can only ever produce a band along both edges. That is why an
    /// across-width gradient built on distance alone comes out looking like an
    /// outline rather than a gradient.
    float u = 0.0f;
};

/** @brief Signed distance to a round cone (a capsule with two radii).
 *
 * Inigo Quilez's analytic form. The three branches are the two spherical caps
 * and the conical flank between them.
 *
 * The degenerate guard at the top is not optional. Two coincident points give
 * l2 == 0, and the reciprocal below then produces an infinity that propagates
 * into every pixel of the tile. Coincident points are routine here: a collapsed
 * trim at the start of a draw-on animation produces them on the first frame of
 * every single animation, so this branch runs constantly rather than never.
 */
BL_HD inline float SdRoundCone(Vec2 p, Vec2 a, Vec2 b, float r1, float r2)
{
    const Vec2  ba = b - a;
    const float l2 = Dot(ba, ba);

    if (l2 < 1e-12f) return Length(p - a) - Max(r1, r2);   // a disc

    const float rr  = r1 - r2;
    const float a2  = l2 - rr * rr;
    const float il2 = 1.0f / l2;

    const Vec2  pa = p - a;
    const float y  = Dot(pa, ba);
    const float z  = y - l2;
    const float x2 = Dot2(pa * l2 - ba * y);
    const float y2 = y * y * l2;
    const float z2 = z * z * l2;

    const float sgnRR = (rr > 0.0f) ? 1.0f : ((rr < 0.0f) ? -1.0f : 0.0f);
    const float k     = sgnRR * rr * rr * x2;

    const float sgnZ = (z > 0.0f) ? 1.0f : ((z < 0.0f) ? -1.0f : 0.0f);
    if (sgnZ * a2 * z2 > k) return ::sqrtf(x2 + z2) * il2 - r2;

    const float sgnY = (y > 0.0f) ? 1.0f : ((y < 0.0f) ? -1.0f : 0.0f);
    if (sgnY * a2 * y2 < k) return ::sqrtf(x2 + y2) * il2 - r1;

    // a2 can go very slightly negative when one radius swallows the other
    // entirely (|rr| >= |ba|), which is a legal shape -- a small dot inside a
    // big one -- but sqrt of a negative is not. Clamp rather than branch.
    return (::sqrtf(Max(x2 * a2 * il2, 0.0f)) + y * rr) * il2 - r1;
}

/** @brief Signed distance to a triangle. Negative inside.
 *
 * Inigo Quilez's form: the distance is the smallest of the three edge distances,
 * and the sign comes from whether the point is consistently on the inner side of
 * every edge. @c s carries the winding so the same code works for a triangle
 * given in either order -- an arrowhead's vertex order flips with the path
 * direction, and a sign convention that depended on it would make half of them
 * inside out.
 */
BL_HD inline float SdTriangle(Vec2 p, Vec2 v0, Vec2 v1, Vec2 v2)
{
    const Vec2 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;
    const Vec2 w0 = p - v0,  w1 = p - v1,  w2 = p - v2;

    const float l0 = Max(Dot(e0, e0), 1e-12f);
    const float l1 = Max(Dot(e1, e1), 1e-12f);
    const float l2 = Max(Dot(e2, e2), 1e-12f);

    const Vec2 q0 = w0 - e0 * Saturate(Dot(w0, e0) / l0);
    const Vec2 q1 = w1 - e1 * Saturate(Dot(w1, e1) / l1);
    const Vec2 q2 = w2 - e2 * Saturate(Dot(w2, e2) / l2);

    const float s = (Cross(e0, e2) > 0.0f) ? 1.0f : -1.0f;

    float dSq  = Min(Min(Dot(q0, q0), Dot(q1, q1)), Dot(q2, q2));
    float sign = Min(Min(s * Cross(w0, e0), s * Cross(w1, e1)), s * Cross(w2, e2));

    return (sign > 0.0f ? -1.0f : 1.0f) * ::sqrtf(Max(dSq, 0.0f));
}

/** @brief Distance to a segment, plus where along it the closest point sits.
 *
 * The arc-length position deliberately comes from the *axis* projection rather
 * than from the true closest point on the cone surface. On the flank the two
 * agree; inside a cap they differ slightly, and the axis answer is the one a
 * gradient wants anyway -- "how far along the line am I", not "where on the
 * silhouette did I land".
 */
BL_HD inline DistanceHit DistanceToSegment(Vec2 p, const Segment& seg)
{
    DistanceHit hit;

    if (seg.flags & Segment::kTriangle)
    {
        // Subtracting the corner radius rounds the corners and inflates the
        // whole shape by that radius; the build compensates by insetting the
        // vertices first, so the silhouette keeps the size that was asked for.
        hit.d = SdTriangle(p, seg.a, seg.b, seg.c) - seg.ra;

        // An arrowhead needs the same two interior coordinates a segment has, or
        // no gradient can vary across it. Handing back a single constant makes
        // the whole head one flat colour -- the end colour for a length ramp, the
        // midpoint for a width ramp -- which reads as the gradient stopping dead
        // at the arrow.
        //
        // Derived from the vertices rather than stored: a = tip, b and c are the
        // base corners, so the axis is their midpoint to the tip and the base
        // half-width is half their separation. Nothing extra to carry, and it
        // cannot fall out of step with the geometry it describes.
        const Vec2  mid = (seg.b + seg.c) * 0.5f;
        const Vec2  ax  = seg.a - mid;
        const float l2  = Dot(ax, ax);

        if (l2 > 1e-12f)
        {
            const float h = Saturate(Dot(p - mid, ax) / l2);   // 0 base, 1 tip
            hit.s = Lerp(seg.s0, seg.s1, h);

            // The head tapers to a point, so the local half-width closes with h.
            //
            // The floor is RELATIVE to the shape, and that matters more than it
            // looks. An absolute floor of 1e-4 leaves the division amplifying
            // its numerator by ten thousand near the tip, where the numerator is
            // itself almost zero -- so u stops describing anything and starts
            // reporting rounding error, swinging between the two extremes of the
            // ramp. Measured as a 1e-2 CPU/GPU divergence, which is the same
            // instability seen from the outside.
            const float halfBase = Length(seg.b - seg.c) * 0.5f;
            const float r        = halfBase * (1.0f - h);
            const Vec2  dir      = ax * (1.0f / ::sqrtf(l2));
            hit.u = Clamp(Cross(dir, p - mid) / Max(r, Max(halfBase * 0.05f, 1e-3f)),
                          -1.0f, 1.0f);
        }
        else
        {
            hit.s = seg.s0;
        }
        return hit;
    }

    hit.d = SdRoundCone(p, seg.a, seg.b, seg.ra, seg.rb);

    const Vec2  ba = seg.b - seg.a;
    const float l2 = Dot(ba, ba);

    // Signed offset across the stroke, normalised by the local half-width so it
    // stays -1..1 even where the line tapers.
    //
    // The floor is relative to the stroke's own width, not absolute: where a
    // taper closes to nothing the division would otherwise amplify a numerator
    // that is itself near zero, and u would report rounding error rather than a
    // position.
    if (l2 > 1e-12f)
    {
        const float inv = 1.0f / ::sqrtf(l2);
        const Vec2  dir = ba * inv;
        const float h   = Saturate(Dot(p - seg.a, ba) / l2);
        const float r   = Lerp(seg.ra, seg.rb, h);
        const float rF  = Max(Max(seg.ra, seg.rb) * 0.05f, 1e-3f);
        hit.u = Clamp(Cross(dir, p - seg.a) / Max(r, rF), -1.0f, 1.0f);
    }

    // Flat caps: intersect with the half-planes that keep the run's own side.
    // Intersection is a max of signed distances, so this both removes the cap
    // bulge and leaves the field usable for feathering the flat edge itself.
    //
    // Deliberately not gated on l2: a degenerate segment must still be clipped.
    // Skipping it there is what turns a zero-length run into a full round disc,
    // which is a round blob on the end of something the user asked to be flat.
    if (seg.flags & Segment::kCapA) hit.d = Max(hit.d, Dot(seg.na, p - seg.capA));
    if (seg.flags & Segment::kCapB) hit.d = Max(hit.d, Dot(seg.nb, p - seg.capB));
    const float h  = (l2 > 1e-12f) ? Saturate(Dot(p - seg.a, ba) / l2) : 0.0f;
    hit.s = Lerp(seg.s0, seg.s1, h);

    return hit;
}

/** @brief Unsigned distance from @p p to the line segment a..b. */
BL_HD inline float DistanceToLine(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2  ab = b - a;
    const float l2 = Dot(ab, ab);
    if (l2 < 1e-12f) return Length(p - a);
    const float h = Saturate(Dot(p - a, ab) / l2);
    return Length(p - (a + ab * h));
}

/** @brief Winding-number contribution of one edge, for closed-path fill.
 *
 * Crossing-count rather than angle-sum: it is exact in integer-ish arithmetic,
 * branch-cheap, and needs no trigonometry. Returns +1, -1 or 0.
 *
 * The half-open rule (@c y0 <= p.y strictly against @c y1 > p.y) is what stops a
 * vertex that lands exactly on the scanline being counted twice by the two edges
 * that share it, which would punch a one-pixel hole through the fill at every
 * horizontal extreme of the shape.
 */
BL_HD inline int WindingContribution(Vec2 p, Vec2 a, Vec2 b)
{
    if (a.y <= p.y)
    {
        if (b.y > p.y && Cross(b - a, p - a) > 0.0f) return  1;
    }
    else
    {
        if (b.y <= p.y && Cross(b - a, p - a) < 0.0f) return -1;
    }
    return 0;
}

} // namespace bl
