#pragma once

// Control points -> the flat primitive list the pixel kernel consumes.
//
// The order below is not arbitrary, and the placement of trim and dash *here*
// rather than in the shading pass is the single most important decision in the
// rasterizer:
//
//   Modulating coverage by a dash pattern at shading time gives the wrong answer
//   at every dash boundary. The distance field still describes the uninterrupted
//   line, so each dash gets a hard sliced edge instead of a cap, round caps stop
//   being round, and feather leaks across the gaps. Cutting the *geometry*
//   instead means each dash is a run in its own right and the SDF caps it
//   correctly for free.
//
// The same argument applies to trim, which is just a single dash.

#include "Anim.h"        // TrimState
#include "Geometry.h"
#include "Primitives.h"
#include "Style.h"

#include <vector>

namespace bl {

namespace detail {

/** @brief Pull a polygon vertex inward so that rounding it by @p r keeps the
 *         original edges where they were.
 *
 * Rounding an SDF polygon is a subtraction, which inflates the whole shape by
 * the radius. Insetting each vertex along its angle bisector by r / sin(half the
 * interior angle) puts a circle of radius r tangent to both edges, so the
 * rounded silhouette lands back on the edges that were asked for. Without it,
 * turning the roundness up makes the arrowhead grow.
 */
inline Vec2 InsetVertex(Vec2 v, Vec2 prev, Vec2 next, float r)
{
    if (r <= 1e-6f) return v;

    const Vec2 d1 = Normalize(next - v, Vec2(0.0f, 0.0f));
    const Vec2 d2 = Normalize(prev - v, Vec2(0.0f, 0.0f));

    Vec2 bis = d1 + d2;
    if (Dot2(bis) < 1e-12f) return v;          // straight through: nothing to round
    bis = Normalize(bis);

    const float cosPhi  = Clamp(Dot(d1, d2), -1.0f, 1.0f);
    const float sinHalf = ::sqrtf(Max((1.0f - cosPhi) * 0.5f, 1e-6f));

    // A very sharp corner sends r / sinHalf towards infinity and would fold the
    // vertex out through the opposite edge. Capping at a share of the shorter
    // adjacent edge keeps a spike blunt instead of inside out.
    const float limit = 0.45f * Min(Length(next - v), Length(prev - v));
    return v + bis * Min(r / sinHalf, limit);
}

/** @brief Emit one uninterrupted run of the polyline between two distances.
 *
 * Follows the polyline's own vertices so the emitted capsules trace the curve
 * exactly, with interpolated points only at the two cut ends.
 *
 * @return the number of segments appended.
 */
inline size_t EmitRun(const Polyline& poly, float d0, float d1,
                      float sA, float sB,
                      const Style& st, std::vector<Segment>& out)
{
    const float total = poly.Total();
    if (total <= 1e-6f) return 0;

    d0 = Clamp(d0, 0.0f, total);
    d1 = Clamp(d1, 0.0f, total);
    if (d1 - d0 <= 1e-5f) return 0;

    // The taper parameter is supplied by the caller as its values at the two ends
    // of this run, rather than being derived from arc length here.
    //
    // That indirection is the whole feature. Derived from arc length, the profile
    // is pinned to the PATH: trim reveals a slice of a fixed taper, so a draw-on
    // stroke starts thick wherever the reveal happens to begin and only thins at
    // the path's own far end. Supplied by the caller, the same code maps the
    // profile across the VISIBLE window instead, and the leading tip is tapered
    // wherever it currently is. Passing the path-absolute values reproduces the
    // old behaviour exactly, which is what the Style flag selects between.
    //
    // Still a lerp on arc length WITHIN the run, so a tapered line that is also
    // dashed thins out along the run rather than each dash tapering individually.
    const float dSpan = d1 - d0;
    auto tAt = [d0, dSpan, sA, sB](float d) {
        const float k = (dSpan > 1e-6f) ? Saturate((d - d0) / dSpan) : 0.0f;
        return sA + (sB - sA) * k;
    };

    auto radiusAt = [&st, &tAt](float d) {
        return Max(st.halfWidth * WidthProfile(Saturate(tAt(d)),
                                               st.widthStart, st.widthMid, st.widthEnd),
                   0.0f);
    };
    auto sAt = [total](float d) { return total > 1e-6f ? Saturate(d / total) : 0.0f; };

    const size_t first = out.size();

    size_t hint = 0;
    Vec2   prev  = PointAtDistance(poly, d0, hint);
    float  prevD = d0;

    auto emitRaw = [&](Vec2 p, float d) {
        // Zero-length segments are dropped rather than emitted. The final cut of
        // a run lands wherever the trim or dash boundary falls, which can be a
        // hair past a flattened vertex the loop above just emitted; the leftover
        // carries no direction, so it cannot define a cap plane and it cannot be
        // measured for arc length.
        if (Dot2(p - prev) < 1e-12f) return;

        Segment seg;
        seg.a  = prev;  seg.b  = p;
        seg.ra = radiusAt(prevD); seg.rb = radiusAt(d);
        seg.s0 = sAt(prevD);      seg.s1 = sAt(d);
        out.push_back(seg);
        prev = p; prevD = d;
    };

    // How finely the width profile has to be sampled, in arc length.
    //
    // A round cone interpolates its radius LINEARLY between its two ends, so a
    // segment can only ever represent a straight ramp. That is exactly right
    // when the profile IS straight, and silently wrong when it is not: a path
    // drawn with corner points flattens to one segment per span, so a leaf taper
    // (0 / 1 / 0) evaluates to radius 0 at both ends of that single segment and
    // the stroke disappears completely. It looked like taper only worked on
    // curves, because only curves were being subdivided enough to show it.
    //
    // Subdivision is therefore driven by the PROFILE as well as by curvature,
    // and only when the profile is actually curved -- a straight taper still
    // costs one segment.
    // Measured against the arc length over which the profile completes ONE full
    // sweep, not against the path length. When the taper follows a short trimmed
    // window the profile changes just as much over far less distance, so a step
    // derived from the path would under-sample it badly -- the leaf taper would
    // reappear as the flat-topped shape that a single round cone can represent.
    const float linearMid = 0.5f * (st.widthStart + st.widthEnd);
    const bool  curvedProfile = (::fabsf(st.widthMid - linearMid) > 1e-4f);
    const float sweep  = ::fabsf(sB - sA);
    const float sweepD = (sweep > 1e-6f) ? (dSpan / sweep) : total;
    const float profileStep   = curvedProfile ? Max(sweepD / 48.0f, 1.0f) : 0.0f;

    auto emit = [&](Vec2 p, float d) {
        const Vec2  a0   = prev;
        const float d0v  = prevD;
        const float span = d - d0v;

        int n = 1;
        if (profileStep > 0.0f && span > profileStep)
        {
            n = static_cast<int>(::ceilf(span / profileStep));
            if (n > 64) n = 64;
        }
        if (n <= 1) { emitRaw(p, d); return; }

        // Linear interpolation is EXACT here: between two consecutive flattened
        // vertices the path is a straight line by construction, so subdividing
        // adds width detail without moving the geometry at all.
        for (int k = 1; k <= n; ++k)
        {
            const float f = static_cast<float>(k) / static_cast<float>(n);
            emitRaw(k == n ? p : a0 + (p - a0) * f, d0v + span * f);
        }
    };

    size_t i = 0;
    while (i < poly.cum.size() && poly.cum[i] <= d0) ++i;
    for (; i < poly.cum.size() && poly.cum[i] < d1; ++i) emit(poly.pts[i], poly.cum[i]);

    emit(PointAtDistance(poly, d1, hint), d1);

    const size_t count = out.size() - first;
    if (count == 0) return 0;

    // --- caps ----------------------------------------------------------------
    // A square cap is a butt cap on a run extended by its own radius, so the two
    // share a code path rather than each growing their own.
    if (st.cap != kCapRound)
    {
        const size_t last = out.size() - 1;

        // Directions are read before anything is moved. With a single-segment
        // run head and tail are the same object, so extending the start would
        // otherwise change the direction used to extend the end.
        const Vec2 dirIn  = Normalize(out[first].b - out[first].a);
        const Vec2 dirOut = Normalize(out[last].b  - out[last].a);

        if (st.cap == kCapSquare)
        {
            // Moving the endpoint rather than just pushing the plane out: the
            // square cap's corners sit at radius * sqrt(2) from the original
            // end, which the existing cap disc does not cover. Moved out by one
            // radius, the corners land exactly on the new disc's boundary.
            out[first].a = out[first].a - dirIn  * out[first].ra;
            out[last].b  = out[last].b  + dirOut * out[last].rb;
        }

        const Vec2 pa = out[first].a;
        const Vec2 pb = out[last].b;

        // Only segments close enough to an end can bulge through it: a capsule
        // reaches back exactly one radius from its own start. Walking out from
        // each end and stopping once the accumulated length exceeds the radius
        // is what keeps this local.
        //
        // Being local is the whole point, not an optimisation. These are
        // infinite half-planes, and a run that curves back on itself -- a circle
        // trimmed to 300 degrees, say -- has most of its length sitting "behind"
        // the plane at its own end. Applying the cap to every segment would
        // erase most of the arc.
        float acc = 0.0f;
        for (size_t k = first; k <= last; ++k)
        {
            if (acc >= out[k].ra) break;
            out[k].na   = Vec2(-dirIn.x, -dirIn.y);
            out[k].capA = pa;
            out[k].flags |= Segment::kCapA;
            acc += Length(out[k].b - out[k].a);
        }

        acc = 0.0f;
        for (size_t k = last + 1; k-- > first; )
        {
            if (acc >= out[k].rb) break;
            out[k].nb   = dirOut;
            out[k].capB = pb;
            out[k].flags |= Segment::kCapB;
            acc += Length(out[k].b - out[k].a);
            if (k == first) break;   // size_t underflow guard
        }
    }

    return count;
}

/** @brief Split a run into dashes and emit each as its own run. */
inline void EmitDashed(const Polyline& poly, float d0, float d1,
                       float sA, float sB,
                       const Style& st, float dashLen, float gapLen, float phase,
                       std::vector<Segment>& out)
{
    // Each dash gets the slice of the taper domain that its position in the run
    // earns it, so the profile stays continuous across the gaps.
    const float runSpan = d1 - d0;
    auto tOf = [d0, runSpan, sA, sB](float d) {
        const float k = (runSpan > 1e-6f) ? Saturate((d - d0) / runSpan) : 0.0f;
        return sA + (sB - sA) * k;
    };

    // A non-positive gap is not a dash pattern, it is a solid line with extra
    // steps -- and a non-positive dash would step forever. Both are reachable
    // from the Inspector, so both fall back rather than hang.
    if (dashLen <= 1e-4f || gapLen <= 1e-4f)
    {
        EmitRun(poly, d0, d1, sA, sB, st, out);
        return;
    }

    const float period = dashLen + gapLen;

    // Walk from the pattern boundary at or before d0 rather than from d0 itself,
    // so the dashes stay pinned to the path and do not slide along it whenever
    // the trim start animates.
    float t = d0 - ::fmodf(d0 + phase, period);

    // Bounded independently of the loop condition: a period that underflows to
    // near-zero through some combination of animated inputs would otherwise spin
    // here, inside the render action, with the UI frozen behind it.
    const int maxDashes = 4096;
    for (int n = 0; n < maxDashes && t < d1; ++n, t += period)
    {
        const float e0 = Max(t, d0);
        const float e1 = Min(t + dashLen, d1);
        EmitRun(poly, e0, e1, tOf(e0), tOf(e1), st, out);
    }
}

} // namespace detail

/** @brief Build the primitive list for one stroke at one frame.
 *
 * @param poly    already flattened, with cumulative arc length
 * @param st      style resolved to pixels
 * @param trim    animated trim window
 * @param closed  whether the trim window may wrap past the end of the path
 * @param dashLen dash length in pixels; <= 0 disables dashing
 */
inline void BuildStrokeSegments(const Polyline& poly, const Style& st,
                                const TrimState& trim, bool closed,
                                float dashLen, float gapLen, float dashPhase,
                                std::vector<Segment>& out)
{
    if (!poly.Valid()) return;

    const size_t firstSeg = out.size();
    const float  total    = poly.Total();

    float a = trim.start + trim.offset;
    float b = trim.end   + trim.offset;
    if (b - a <= 1e-6f) return;      // fully collapsed: draw nothing, no fudge

    // A zero thickness means NO stroke, not a faint one.
    //
    // The shading thresholds coverage at the surface, so a zero-radius run still
    // lands at exactly 50% coverage along its own centreline -- a hairline that
    // appears wherever the thickness was deliberately turned off. That matters
    // most for imported artwork, which is often fill-only: every contour of
    // every filled shape came back as a ghost line.
    //
    // Arrowheads are sized by their own parameters and are emitted regardless,
    // so a zero-thickness path with an arrow is still an arrow.
    const bool strokeVisible = (st.halfWidth > 0.0f) &&
                               (st.widthStart > 0.0f || st.widthMid > 0.0f || st.widthEnd > 0.0f);

    // The taper domain: the two values the width profile takes at this run's
    // ends. Path-absolute reproduces the old behaviour exactly; window-relative
    // is what makes a drawing-on stroke taper at the tip it currently has.
    auto run = [&](float u0, float u1, float sA, float sB) {
        if (!strokeVisible) return;
        if (!st.taperFollowsTrim) { sA = u0; sB = u1; }

        if (dashLen > 0.0f)
            detail::EmitDashed(poly, u0 * total, u1 * total, sA, sB, st,
                               dashLen, gapLen, dashPhase, out);
        else
            detail::EmitRun(poly, u0 * total, u1 * total, sA, sB, st, out);
    };

    const bool arrowsOn = (st.arrowStart || st.arrowEnd) &&
                          st.arrowLength > 1e-4f && st.arrowWidth > 1e-4f;

    // The stroke is pulled back from any end that carries an arrowhead.
    //
    // Without this the line's own cap sits exactly where the arrow's tip is, so
    // a disc of halfWidth bulges straight out past the point and the arrow ends
    // in a blob. Shortening by the arrow's length tucks the cap up inside the
    // head, where the union hides it.
    // Only the part of the head that lies BEFORE the path end hides the shaft,
    // so the pull-back follows the position control rather than the full length.
    const float pos       = Saturate(st.arrowPosition);
    const float shaftBack = st.arrowLength * (1.0f - pos);

    const float insetA = (arrowsOn && st.arrowStart) ? shaftBack / total : 0.0f;
    const float insetB = (arrowsOn && st.arrowEnd)   ? shaftBack / total : 0.0f;

    float uStart = 0.0f, uEnd = 1.0f;   // where the ARROWS go: the true ends
    bool  anyVisible = false;           // is there a window left to draw at all

    if (closed && trim.wrap)
    {
        // Wrap into the 0..1 domain and split into at most two runs. A window
        // that spans the seam has to be drawn as two pieces, because the arc
        // length parameterisation is not itself circular.
        //
        // The span is measured BEFORE a is wrapped. Recomputing it afterwards
        // mixes a wrapped start with an unwrapped end: at Offset 1 that turned a
        // 0.22 window into Min(1.22 - 0, 1) = the whole ring, so a sliding
        // window silently became a full circle at exactly one offset.
        const float span = Min(b - a, 1.0f);
        a -= ::floorf(a);
        b  = a + span;

        uStart = a;
        uEnd   = (b > 1.0f) ? b - 1.0f : b;
        anyVisible = true;   // the window was checked non-empty above and cannot leave

        const float da = a + insetA;
        const float db = b - insetB;
        if (db > da)
        {
            // A window across the seam is two runs, but ONE taper: the profile
            // has to carry over the join rather than restarting, or the seam
            // becomes a visible step in the width. So the split point is
            // expressed as its fraction of the window and handed to both halves.
            if (db <= 1.0f) run(da, db, 0.0f, 1.0f);
            else
            {
                const float w   = db - da;
                const float cut = (w > 1e-6f) ? (1.0f - da) / w : 1.0f;
                run(da,  1.0f,      0.0f, cut);
                run(0.0f, db - 1.0f, cut, 1.0f);
            }
        }
    }
    else
    {
        uStart = Clamp(a, 0.0f, 1.0f);
        uEnd   = Clamp(b, 0.0f, 1.0f);

        // Clamping can close the window entirely: an Offset that has carried it
        // off either end of the path leaves nothing visible. That is the whole
        // point of the sweep, so it has to be detectable rather than merely
        // producing no segments -- the arrowheads below key off it.
        anyVisible = (uEnd - uStart) > 1e-6f;

        // A run shorter than the arrows that terminate it leaves nothing to
        // draw, which is correct: the heads meet and there is no shaft.
        const float da = uStart + insetA;
        const float db = uEnd   - insetB;
        if (db > da) run(da, db, 0.0f, 1.0f);
    }

    // --- arrowheads ----------------------------------------------------------
    // Emitted at the TRIMMED ends, not the path's own ends, so an arrow rides
    // the head of a draw-on reveal instead of sitting where the line has not
    // reached yet. Deliberately outside the dash walk: a dashed connector wants
    // one arrow, not one per dash.
    // Arrowheads belong to the LINE, not to the path.
    //
    // Emitting them at the path's ends whatever the trim is doing leaves a head
    // hanging in space after the stroke it terminates has been trimmed away --
    // which reads as a separate object stuck near the line rather than part of
    // it. Gated on the visible window instead, a head slides along with an
    // Offset sweep and leaves the frame with the rest of the stroke.
    if (arrowsOn && anyVisible)
    {
        const float half = st.arrowWidth * 0.5f;
        size_t hint = 0;

        auto arrow = [&](float u, bool forward) {
            const float dist = u * total;
            const Vec2  anchor = PointAtDistance(poly, dist, hint);

            // Direction from the CHORD across the part of the path the head
            // actually covers -- which is what `pos` decides.
            //
            // The span has to follow the position or the head is aimed by
            // geometry it no longer sits on. At pos = 0 the head lies along the
            // last arrowLength of the line, and the chord over exactly that is
            // right: aligning to the instantaneous final tangent instead leaves
            // the head visibly skewed against the curve beneath it. At pos = 1
            // the head has moved entirely PAST the end, covering no path at all,
            // and that same backward chord now averages in a stretch the head
            // has left behind -- so the head points off along where the line WAS
            // heading rather than where it is going.
            //
            // Shrinking the lookback with (1 - pos) makes both ends of that
            // range fall out of one rule. The floor keeps a little baseline so a
            // head sitting past the end is aimed by a short chord rather than a
            // single flattened edge, which would jitter as the flattening
            // tolerance changed.
            const float span = Max(st.arrowLength * (1.0f - pos), st.arrowLength * 0.15f);
            const float back = forward ? dist - span : dist + span;

            size_t ch = 0;
            const Vec2 from = PointAtDistance(poly, Clamp(back, 0.0f, total), ch);

            Vec2 dir = anchor - from;
            if (Dot2(dir) < 1e-10f)
            {
                // Degenerate chord: a head longer than the path it terminates,
                // or a fully collapsed trim. Fall back to the tangent, which is
                // always defined.
                size_t th = 0;
                dir = TangentAtDistance(poly, dist, th);
                if (!forward) dir = Vec2(-dir.x, -dir.y);
            }
            dir = Normalize(dir);

            // Slide the head along that direction so `pos` of it sits past the
            // path end. At 0.5 the triangle straddles the end point.
            const Vec2 tip  = anchor + dir * (st.arrowLength * pos);
            const Vec2 perp = Perp(dir);
            const Vec2 base = tip - dir * st.arrowLength;

            Vec2 v0 = tip;
            Vec2 v1 = base + perp * half;
            Vec2 v2 = base - perp * half;

            // Roundness cannot exceed what the triangle can absorb; past that
            // the insets cross over and the shape turns itself inside out.
            const float r = Min(st.arrowRound,
                                0.5f * Min(st.arrowLength, half));

            const Vec2 i0 = detail::InsetVertex(v0, v2, v1, r);
            const Vec2 i1 = detail::InsetVertex(v1, v0, v2, r);
            const Vec2 i2 = detail::InsetVertex(v2, v1, v0, r);

            Segment seg;
            seg.a = i0; seg.b = i1; seg.c = i2;
            seg.ra = r;

            // Arc positions at the base and the tip, so a length gradient keeps
            // ramping through the head instead of stopping at the path end.
            // Signed by direction: a start arrow points backwards, so arc length
            // DECREASES from its base to its tip.
            const float sign = forward ? 1.0f : -1.0f;
            seg.s0 = u - sign * (st.arrowLength * (1.0f - pos)) / total;  // base
            seg.s1 = u + sign * (st.arrowLength * pos) / total;           // tip

            seg.flags |= Segment::kTriangle;

            out.push_back(seg);
        };

        if (st.arrowEnd)   arrow(uEnd,   true);
        if (st.arrowStart) arrow(uStart, false);
    }

    // Renormalise the arc domain over everything actually drawn.
    //
    // An arrowhead at the default position sits entirely PAST the end of the
    // path, so its arc positions run beyond 1 (or below 0 at the start). Left
    // alone those clamp against the end of the ramp and the head comes out flat
    // in the one colour -- the gradient visibly stopping dead where the arrow
    // begins. Rescaling so the ramp spans the whole visible shape is what makes
    // an arrow read as the end OF the line rather than an object stuck onto it.
    //
    // Seeded with 0 and 1 so the path itself always defines the base domain and
    // a trimmed line does not restretch its gradient.
    if (out.size() > firstSeg)
    {
        float sMin = 0.0f, sMax = 1.0f;
        for (size_t k = firstSeg; k < out.size(); ++k)
        {
            sMin = Min(sMin, Min(out[k].s0, out[k].s1));
            sMax = Max(sMax, Max(out[k].s0, out[k].s1));
        }

        const float span = sMax - sMin;
        if (span > 1e-6f && (sMin < -1e-6f || sMax > 1.0f + 1e-6f))
        {
            const float inv = 1.0f / span;
            for (size_t k = firstSeg; k < out.size(); ++k)
            {
                out[k].s0 = (out[k].s0 - sMin) * inv;
                out[k].s1 = (out[k].s1 - sMin) * inv;
            }
        }
    }
}

} // namespace bl
