#pragma once

// Generators for the common primitives: rectangle, ellipse, regular polygon.
//
// These exist because the plugin already has a global Transform, which makes
// point-by-point placement the wrong unit of work for a shape someone wants as a
// frame, a highlight box or a ring. Dropping in a correct shape and scaling it is
// two actions; drawing a circle out of four bezier points by hand is not
// something anyone should be asked to do twice.
//
// There is no separate triangle: a triangle is a three-sided polygon, and having
// both would be two controls for one idea.
//
// Output is in SLOT coordinates -- x a fraction of frame WIDTH, y a fraction of
// frame HEIGHT, origin bottom-left -- because that is what the keyframable point
// slots store.
//
// Those units are NOT square on a non-square frame, which is the trap this file
// is arranged around. Every generator below works in SQUARE units centred on the
// origin, and MakeShape divides x by the aspect exactly once, at the very end,
// for positions and tangents alike. Doing it any earlier means a scalar handle
// length gets applied to two axes of different scale: the corner arcs come out
// with a horizontal handle an aspect-ratio too long, so a rounded rectangle has
// one flank of every corner stretched. Squashing at the end is also exactly
// right geometrically -- square units map to pixels by a single factor (the
// frame height), so a circular fillet stays circular.
//
// Handles are SYMMETRIC, which is a constraint rather than a preference: a slot
// point stores one tangent, and the incoming handle is its mirror. It happens to
// cost nothing here. A circle's four-point bezier is naturally symmetric, and at
// a rounded corner the mirrored handle points back along the straight edge it
// came from -- collinear with that edge, so the edge stays exactly straight.

#include "Common.h"
#include "Geometry.h"

#include <vector>

namespace bl {

enum ShapeKind
{
    kShapeRectangle = 0,
    kShapeEllipse   = 1,
    kShapePolygon   = 2,
};

/** @brief Handle length for a circular arc of @p sweep radians at radius @p r.
 *
 * The standard cubic approximation to an arc. At a quarter turn this is the
 * familiar kappa, 0.5523 -- but writing it as a function of the sweep is what
 * lets the same code round a triangle's 120-degree corner and a 16-gon's 22.5.
 */
BL_HD inline float ArcHandle(float r, float sweep)
{
    return r * (4.0f / 3.0f) * ::tanf(sweep * 0.25f);
}

/** @brief How many points a shape will occupy, so the caller can budget slots.
 *
 * Rounding a corner replaces one vertex with two, hence the doubling. An ellipse
 * is always four points regardless.
 */
inline int ShapePointCount(int kind, int sides, bool rounded)
{
    if (kind == kShapeEllipse) return 4;

    const int n = (kind == kShapeRectangle) ? 4 : (sides < 3 ? 3 : sides);
    return rounded ? n * 2 : n;
}

/** @brief The largest side count that still fits, given the point ceiling. */
inline int MaxSidesFor(int maxPoints, bool rounded)
{
    const int n = rounded ? maxPoints / 2 : maxPoints;
    return n < 3 ? 3 : n;
}

/** @brief Four-point bezier ellipse about the origin, in square units. */
inline void MakeEllipse(float rx, float ry, std::vector<ControlPoint>& out)
{
    const float hx = ArcHandle(rx, 1.5707963268f);
    const float hy = ArcHandle(ry, 1.5707963268f);

    // Counter-clockwise from the right, matching the polygon generator so a
    // fill's winding does not depend on which shape produced it.
    out.push_back({Vec2( rx, 0.0f), Vec2(0.0f,  hy), Vec2(), false});
    out.push_back({Vec2(0.0f,  ry), Vec2(-hx, 0.0f), Vec2(), false});
    out.push_back({Vec2(-rx, 0.0f), Vec2(0.0f, -hy), Vec2(), false});
    out.push_back({Vec2(0.0f, -ry), Vec2( hx, 0.0f), Vec2(), false});
}

/** @brief Round the corners of a closed polygon given by its vertices.
 *
 * Each vertex becomes two points, one on each adjoining edge, joined by a bezier
 * approximating the fillet arc. @p radius is in the same units as the vertices
 * and is clamped per corner so a fillet can never eat past the middle of either
 * edge -- an unclamped radius on a thin triangle produces edges that cross, and a
 * self-crossing contour fills inside out.
 */
inline void RoundCorners(const std::vector<Vec2>& verts, float radius,
                         std::vector<ControlPoint>& out)
{
    const size_t n = verts.size();
    if (n < 3) return;

    for (size_t i = 0; i < n; ++i)
    {
        const Vec2 v    = verts[i];
        const Vec2 prev = verts[(i + n - 1) % n];
        const Vec2 next = verts[(i + 1) % n];

        const Vec2  ua = Normalize(prev - v);
        const Vec2  ub = Normalize(next - v);
        const float la = Length(prev - v);
        const float lb = Length(next - v);

        // Interior angle at this corner, from the two edge directions.
        const float cosT  = Clamp(Dot(ua, ub), -1.0f, 1.0f);
        const float theta = ::acosf(cosT);
        if (theta < 1e-4f || theta > 3.14149f)
        { out.push_back({v, Vec2(), Vec2(), false}); continue; }

        // Tangent distance for a fillet of this radius, then clamped to half of
        // the shorter edge so neighbouring corners cannot overlap.
        const float tanHalf = ::tanf(theta * 0.5f);
        float d = (tanHalf > 1e-6f) ? radius / tanHalf : 0.0f;
        const float dMax = 0.5f * Min(la, lb);
        if (d > dMax) d = dMax;
        if (d <= 1e-6f) { out.push_back({v, Vec2(), Vec2(), false}); continue; }

        const float r     = d * tanHalf;      // the radius actually achieved
        const float sweep = 3.1415926536f - theta;
        const float hl    = ArcHandle(r, sweep);

        const Vec2 p1 = v + ua * d;
        const Vec2 p2 = v + ub * d;

        // Each fillet point's tangent points AT the corner, so its mirrored
        // incoming handle points back along its own straight edge and leaves that
        // edge dead straight. Both handles are the same length, which is what
        // makes the arc symmetric -- and why this has to happen in square units.
        out.push_back({p1, ua * -hl, Vec2(), false});
        out.push_back({p2, ub *  hl, Vec2(), false});
    }
}

/** @brief Regular n-gon vertices about the origin, counter-clockwise.
 *
 * Rotated so an even-sided polygon rests on an edge and an odd-sided one points
 * up, which is what "hexagon" and "triangle" mean to anyone placing one.
 */
inline void PolygonVerts(float rx, float ry, int sides, std::vector<Vec2>& out)
{
    if (sides < 3) sides = 3;
    const float step = 6.2831853072f / static_cast<float>(sides);
    const float base = 1.5707963268f + ((sides % 2 == 0) ? step * 0.5f : 0.0f);

    for (int i = 0; i < sides; ++i)
    {
        const float a = base + step * static_cast<float>(i);
        out.push_back(Vec2(rx * ::cosf(a), ry * ::sinf(a)));
    }
}

/** @brief Build one shape into slot-space control points.
 *
 * @param kind    ShapeKind
 * @param sides   polygon side count (3 is a triangle); ignored by the others
 * @param corner  corner radius as a FRACTION of the inscribed radius, 0..1.
 *                Relative so it survives a later resize: an absolute radius on a
 *                shape the user then scales down would swallow the shape.
 * @param sizeX   half-width in frame-height units
 * @param sizeY   half-height in frame-height units
 * @param aspect  frame width / height
 */
inline void MakeShape(int kind, int sides, float corner,
                      float cx, float cy, float sizeX, float sizeY, float aspect,
                      std::vector<ControlPoint>& out)
{
    out.clear();

    if (kind == kShapeEllipse)
    {
        MakeEllipse(sizeX, sizeY, out);
    }
    else
    {
        std::vector<Vec2> verts;
        if (kind == kShapeRectangle)
        {
            verts.push_back(Vec2( sizeX, -sizeY));
            verts.push_back(Vec2( sizeX,  sizeY));
            verts.push_back(Vec2(-sizeX,  sizeY));
            verts.push_back(Vec2(-sizeX, -sizeY));
        }
        else
        {
            PolygonVerts(sizeX, sizeY, sides, verts);
        }

        if (corner <= 1e-4f)
        {
            for (const Vec2& v : verts) out.push_back({v, Vec2(), Vec2(), false});
        }
        else
        {
            // The radius is measured against the INSCRIBED radius -- centre to
            // the middle of an edge -- so that 100% means "as round as this shape
            // gets" for every shape alike.
            //
            // The obvious basis, the distance to a VERTEX, is not comparable
            // across shapes: a triangle's corners sit twice as far out as its
            // edges, so the percentage that gently softened a rectangle turned a
            // triangle into a blob. Measured this way a triangle and a square
            // agree, and a square at 100% is exactly a circle.
            float basis = Min(sizeX, sizeY);
            if (kind != kShapeRectangle)
            {
                const int n = (sides < 3) ? 3 : sides;
                basis *= ::cosf(3.1415926536f / static_cast<float>(n));
            }
            RoundCorners(verts, Clamp(corner, 0.0f, 1.0f) * basis, out);
        }
    }

    // The one and only aspect correction, applied to positions and tangents
    // alike. See the file header for why it cannot happen any earlier.
    const float a = (aspect > 1e-6f) ? aspect : 1.0f;
    for (ControlPoint& p : out)
    {
        p.pos = Vec2(cx + p.pos.x / a, cy + p.pos.y);
        p.tan = Vec2(p.tan.x / a, p.tan.y);
    }
}

} // namespace bl
