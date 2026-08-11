#pragma once

// Control points -> flattened polyline -> cumulative arc length.
//
// The arc-length array built here is the quiet workhorse of the whole plugin.
// Trim, dashes, taper and along-length gradient are all just lookups into it, so
// they cost one shared O(n) pass rather than four separate traversals of the
// curve.

#include "Common.h"

#include <vector>

namespace bl {

/** @brief One editable point on a path.
 *
 * @c tan is the *outgoing* bezier handle, as an offset from @c pos. The incoming
 * handle is its mirror. One symmetric handle rather than two independent ones is
 * a deliberate halving of the parameter count: in a host where every point costs
 * keyframable slots, an asymmetric handle is a topology decision that almost
 * nobody animates, so it does not earn a second Double2D per point.
 */
struct ControlPoint
{
    Vec2 pos;
    Vec2 tan;   ///< outgoing handle offset; zero makes a corner
};

/** @brief A path: an ordered run of control points, optionally closed. */
struct Stroke
{
    std::vector<ControlPoint> pts;
    bool closed = false;
};

/** @brief A flattened path with its cumulative arc length.
 *
 * @c cum has the same length as @c pts; cum[0] is 0 and cum[i] is the distance
 * along the polyline from the start to pts[i].
 */
struct Polyline
{
    std::vector<Vec2>  pts;
    std::vector<float> cum;

    float Total() const { return cum.empty() ? 0.0f : cum.back(); }
    bool  Valid() const { return pts.size() >= 2 && Total() > 1e-6f; }
};

/** @brief A 2D similarity transform applied to every point of every stroke.
 *
 * Editing points one at a time is fine for drawing a shape and useless for
 * placing one: nudging a six-point arrow across the frame means six drags that
 * must all agree. This is the handle on the whole thing.
 *
 * Deliberately applied in PIXEL space, not in the normalised space the points
 * are stored in. Normalised units are not square on a non-square frame, so a
 * rotation performed there shears the shape instead of turning it -- a rotated
 * square would come out as a rhombus, and worse, differently on every timeline
 * resolution.
 */
struct Xform2D
{
    Vec2  pivot{0.0f, 0.0f};
    Vec2  offset{0.0f, 0.0f};
    float cosR = 1.0f, sinR = 0.0f;
    float scaleX = 1.0f, scaleY = 1.0f;

    BL_HD bool IsIdentity() const
    {
        return offset.x == 0.0f && offset.y == 0.0f &&
               sinR == 0.0f && cosR == 1.0f &&
               scaleX == 1.0f && scaleY == 1.0f;
    }

    /// Scale, then rotate, about the pivot; then translate.
    BL_HD Vec2 Apply(Vec2 p) const
    {
        const float dx = (p.x - pivot.x) * scaleX;
        const float dy = (p.y - pivot.y) * scaleY;
        return Vec2(pivot.x + dx * cosR - dy * sinR + offset.x,
                    pivot.y + dx * sinR + dy * cosR + offset.y);
    }

    /// For direction-like quantities -- bezier handles are offsets from their
    /// point, so they must rotate and scale but must NOT be translated.
    BL_HD Vec2 ApplyVector(Vec2 v) const
    {
        const float dx = v.x * scaleX;
        const float dy = v.y * scaleY;
        return Vec2(dx * cosR - dy * sinR, dx * sinR + dy * cosR);
    }

    /// The inverse, needed by the overlay: handles are DISPLAYED transformed, so
    /// a drag arrives in transformed space and has to be carried back before it
    /// is written to the point it came from. Without this every drag would be
    /// applied twice -- once by the user and once by the transform.
    ///
    /// A zero scale is not invertible; the guard makes it a no-op on that axis
    /// rather than a division by zero, so a shape scaled to nothing can still be
    /// scaled back up instead of becoming permanently unusable.
    BL_HD Vec2 ApplyInverse(Vec2 p) const
    {
        const float dx = p.x - offset.x - pivot.x;
        const float dy = p.y - offset.y - pivot.y;
        const float rx =  dx * cosR + dy * sinR;
        const float ry = -dx * sinR + dy * cosR;
        const float ax = (scaleX * scaleX > 1e-12f) ? scaleX : 1.0f;
        const float ay = (scaleY * scaleY > 1e-12f) ? scaleY : 1.0f;
        return Vec2(pivot.x + rx / ax, pivot.y + ry / ay);
    }

    BL_HD Vec2 ApplyVectorInverse(Vec2 v) const
    {
        const float rx =  v.x * cosR + v.y * sinR;
        const float ry = -v.x * sinR + v.y * cosR;
        const float ax = (scaleX * scaleX > 1e-12f) ? scaleX : 1.0f;
        const float ay = (scaleY * scaleY > 1e-12f) ? scaleY : 1.0f;
        return Vec2(rx / ax, ry / ay);
    }
};

inline Xform2D MakeXform(Vec2 pivot, Vec2 offset, float degrees, float sx, float sy)
{
    Xform2D x;
    x.pivot  = pivot;
    x.offset = offset;
    const float r = degrees * 0.01745329252f;
    x.cosR = ::cosf(r);
    x.sinR = ::sinf(r);
    x.scaleX = sx;
    x.scaleY = sy;
    return x;
}

/** @brief Point on a cubic bezier at parameter t. */
inline Vec2 CubicAt(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t)
{
    const float u = 1.0f - t;
    return (u * u * u) * p0 + (3.0f * u * u * t) * p1
         + (3.0f * u * t * t) * p2 + (t * t * t) * p3;
}

/** @brief Distance from @p p to the segment @p a -> @p b. */
inline float DistanceToChord(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2  ab = b - a;
    const float l2 = Dot(ab, ab);
    if (l2 < 1e-12f) return Length(p - a);
    const float h = Saturate(Dot(p - a, ab) / l2);
    return Length(p - (a + ab * h));
}

/** @brief How many uniform pieces this cubic needs to stay within @p tol pixels.
 *
 * Measured as how far the two handles stray from the chord, which is what
 * "flatness" actually means geometrically.
 *
 * The obvious alternative -- the second differences of the control polygon -- is
 * wrong here, and wrong in the most common case there is. A corner point has a
 * zero-length handle, which puts p1 on p0 and p2 on p3; the second differences
 * then evaluate to the full length of the chord, so a perfectly straight line
 * asks for the subdivision of a hard curve. Measured: a straight two-point line
 * came out as 30 vertices and a four-corner rectangle as 93, all of them
 * describing geometry that two points describe exactly. Since a zero handle is
 * the DEFAULT for every new point, that penalty applied to almost every path
 * anyone would draw.
 *
 * Distance to the chord as a SEGMENT rather than to the infinite line is
 * deliberate: control points that project outside the chord describe a curve
 * that doubles back along it, which is collinear but not flat, and measuring
 * against the infinite line would flatten the overshoot away.
 *
 * The ceiling is not just paranoia. @p tol arrives scaled by renderScale, and a
 * proxy render can drive it small while a wildly overshooting handle drives the
 * deviation large; without a clamp the two multiply into an allocation that
 * stalls the render thread on a frame nobody will look at.
 */
inline int CubicSteps(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float tol)
{
    const float dev = Max(DistanceToChord(p1, p0, p3), DistanceToChord(p2, p0, p3));

    if (dev <= tol || tol <= 1e-6f) return 1;

    const int n = static_cast<int>(::ceilf(::sqrtf((3.0f * dev) / (4.0f * tol))));
    return n < 1 ? 1 : (n > 256 ? 256 : n);
}

/** @brief Flatten a stroke to a polyline and accumulate its arc length.
 *
 * Duplicate points are dropped as they are produced rather than filtered
 * afterwards: a zero-length polyline edge carries no direction, so it would give
 * the dash walker a step it can never advance past and the gradient a division
 * by zero. Dropping here means nothing downstream has to know they were ever
 * possible.
 */
inline void FlattenStroke(const Stroke& stroke, float tol, Polyline& out)
{
    out.pts.clear();
    out.cum.clear();

    const size_t n = stroke.pts.size();
    if (n < 2) return;

    auto push = [&out](Vec2 p) {
        if (!out.pts.empty())
        {
            const float step = Length(p - out.pts.back());
            if (step < 1e-6f) return;
            out.cum.push_back(out.cum.back() + step);
        }
        else
        {
            out.cum.push_back(0.0f);
        }
        out.pts.push_back(p);
    };

    const size_t spans = stroke.closed ? n : n - 1;
    for (size_t i = 0; i < spans; ++i)
    {
        const ControlPoint& c0 = stroke.pts[i];
        const ControlPoint& c1 = stroke.pts[(i + 1) % n];

        const Vec2 p0 = c0.pos;
        const Vec2 p1 = c0.pos + c0.tan;
        const Vec2 p2 = c1.pos - c1.tan;   // mirrored incoming handle
        const Vec2 p3 = c1.pos;

        push(p0);

        const int steps = CubicSteps(p0, p1, p2, p3, tol);
        for (int s = 1; s <= steps; ++s)
        {
            push(CubicAt(p0, p1, p2, p3, static_cast<float>(s) / static_cast<float>(steps)));
        }
    }
}

/** @brief Position at a given distance along a flattened polyline.
 *
 * Linear scan from @p hint, which the callers walk monotonically, so a full
 * traversal costs O(n) across all its queries rather than O(n) per query.
 */
inline Vec2 PointAtDistance(const Polyline& poly, float dist, size_t& hint)
{
    if (poly.pts.empty()) return Vec2();
    if (poly.pts.size() == 1) return poly.pts[0];

    const float d = Clamp(dist, 0.0f, poly.Total());

    if (hint >= poly.cum.size() - 1) hint = 0;
    while (hint + 1 < poly.cum.size() && poly.cum[hint + 1] < d) ++hint;
    while (hint > 0 && poly.cum[hint] > d) --hint;

    const float span = poly.cum[hint + 1] - poly.cum[hint];
    const float t    = span > 1e-9f ? (d - poly.cum[hint]) / span : 0.0f;
    return poly.pts[hint] + t * (poly.pts[hint + 1] - poly.pts[hint]);
}

/** @brief Unit forward direction at a given distance along the polyline.
 *
 * Taken from the flattened edge that contains @p dist rather than from the
 * analytic curve derivative: the arrowhead has to sit flush against the drawn
 * geometry, and the drawn geometry is the flattened polyline. A tangent from the
 * true curve would be very slightly off wherever flattening has cut a corner,
 * and an arrowhead that does not line up with its own line is obvious.
 */
inline Vec2 TangentAtDistance(const Polyline& poly, float dist, size_t& hint)
{
    if (poly.pts.size() < 2) return Vec2(1.0f, 0.0f);

    const float d = Clamp(dist, 0.0f, poly.Total());

    if (hint >= poly.cum.size() - 1) hint = 0;
    while (hint + 1 < poly.cum.size() && poly.cum[hint + 1] < d) ++hint;
    while (hint > 0 && poly.cum[hint] > d) --hint;

    return Normalize(poly.pts[hint + 1] - poly.pts[hint]);
}

} // namespace bl
