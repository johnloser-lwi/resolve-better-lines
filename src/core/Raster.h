#pragma once

// The per-pixel kernel. THE per-pixel kernel -- this body is compiled twice, once
// for the host and once by nvcc, and there is no second implementation anywhere.
// tools/cudatest exists to keep it that way.

#include "Common.h"
#include "FillGrid.h"
#include "Primitives.h"
#include "Style.h"
#include "TileGrid.h"

namespace bl {

/** @brief Everything the kernel needs, as plain pointers. Uploadable as-is. */
struct SceneView
{
    const Segment* segs     = nullptr;
    int            segCount = 0;
    TileGridView   grid;

    /// Closed contours for fill, with their own band-binned index. Separate from
    /// the stroke geometry because fill is answered by a winding number over the
    /// whole scanline, not by proximity.
    const FillEdge* fill      = nullptr;
    int             fillCount = 0;
    BandGridView    bands;

    /// One style per stroke, indexed by Segment::style / FillEdge::style.
    /// Never empty: index 0 is the effective global style.
    const Style* styles     = nullptr;
    int          styleCount = 0;

    BL_HD const Style& StyleAt(int i) const
    {
        return styles[(i > 0 && i < styleCount) ? i : 0];
    }

    /** @brief Could anything at all be drawn at this pixel?
     *
     * A cheap rejection so whole empty tiles can be skipped sixteen pixels at a
     * time instead of calling ShadePixel on each. A line covers a few percent of
     * a frame, so the overwhelming majority of pixels have nothing near them --
     * and at UHD that is eight million calls whose entire job is to look up an
     * empty tile and return.
     */
    BL_HD bool AnythingAt(int px, int py) const
    {
        if (fillCount > 0 && bands.start && bands.items)
        {
            const int b = bands.BandOf(py);
            if (b >= 0 && bands.start[b] < bands.start[b + 1]) return true;
        }
        if (!grid.start || !grid.items) return false;

        const int tile = grid.TileOf(px, py);
        return tile >= 0 && grid.start[tile] < grid.start[tile + 1];
    }

    /// First x strictly past @p px that lands in the next tile column.
    BL_HD int NextTileX(int px) const
    {
        const int size = grid.tileSize > 0 ? grid.tileSize : 16;
        const int rel  = px - grid.originX;
        // Floor division, so it still steps forward left of the grid origin
        // rather than stalling on a negative remainder.
        const int col  = (rel >= 0) ? (rel / size) : -(((-rel) + size - 1) / size);
        return grid.originX + (col + 1) * size;
    }
};

/** @brief Premultiplied RGBA. What the kernel produces and the compositor consumes. */
struct Premul
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/** @brief Composite @p src over @p dst, both premultiplied. */
BL_HD inline void Over(const Premul& src, float* dst)
{
    const float ia = 1.0f - src.a;
    dst[0] = src.r + dst[0] * ia;
    dst[1] = src.g + dst[1] * ia;
    dst[2] = src.b + dst[2] * ia;
    dst[3] = src.a + dst[3] * ia;
}

BL_HD inline void AccumulateOver(const Color& c, float cov, Premul& acc)
{
    const float a = Saturate(cov) * Saturate(c.a);
    if (a <= 0.0f) return;

    // Premultiplied throughout: opacity scales all four channels, not just
    // alpha. Resolve hands plugins premultiplied RGBA, and touching alpha alone
    // leaves the colour too bright -- a fade then reads wrong over anything but
    // black, which is exactly the case nobody checks.
    const float ia = 1.0f - a;
    acc.r = c.r * a + acc.r * ia;
    acc.g = c.g * a + acc.g * ia;
    acc.b = c.b * a + acc.b * ia;
    acc.a = a       + acc.a * ia;
}

/** @brief Shade one pixel. @p px,@p py are pixel CENTRES, not corners.
 *
 * Returns the line layer at that pixel, premultiplied and ready to composite.
 */
BL_HD inline Premul ShadePixel(const SceneView& scene, float px, float py)
{
    Premul out;

    if (!scene.styles || scene.styleCount <= 0) return out;

    const Vec2 p(px, py);

    // --- fill coverage -------------------------------------------------------
    // MEASURED here but composited further down, so it lands above the shadow
    // and glow and below the stroke.
    //
    // Deliberately independent of the tile grid. The inside of a large shape has
    // no stroke geometry anywhere near it, so anything gated on the tile lookup
    // would find nothing there and leave the shape hollow. Fill is answered by
    // its own band index, which always has an answer.
    float fillCov      = 0.0f;
    int   fillStyleIdx = 0;
    if (scene.fill && scene.fillCount > 0 && scene.bands.start && scene.bands.items)
    {
        const int band = scene.bands.BandOf(static_cast<int>(::floorf(py)));
        if (band >= 0)
        {
            const int flo = scene.bands.start[band];
            const int fhi = scene.bands.start[band + 1];

            int   wn = 0;
            float fd = 1e30f;
            for (int i = flo; i < fhi; ++i)
            {
                const int idx = scene.bands.items[i];
                if (idx < 0 || idx >= scene.fillCount) continue;

                const FillEdge& e = scene.fill[idx];
                wn += WindingContribution(p, e.a, e.b);

                const float ed = DistanceToLine(p, e.a, e.b);
                if (ed < fd) { fd = ed; fillStyleIdx = e.style; }
            }

            // Winding gives inside/outside; distance to the contour gives the
            // soft edge. Combined into a signed distance, the fill boundary
            // antialiases through the same smoothstep the stroke uses, so the
            // two edges match instead of one looking crisper than the other.
            if (fd < 1e29f)
            {
                const Style& fs = scene.StyleAt(fillStyleIdx);
                if (fs.fill)
                    fillCov = 1.0f - SmoothStep(-fs.feather * 0.5f, fs.feather * 0.5f,
                                                (wn != 0) ? -fd : fd);
            }
        }
    }

    const Style& fillStyle = scene.StyleAt(fillStyleIdx);

    const int tile = scene.grid.TileOf(static_cast<int>(::floorf(px)),
                                       static_cast<int>(::floorf(py)));

    const bool haveTile = (tile >= 0 && scene.grid.start && scene.grid.items &&
                           scene.grid.start[tile] < scene.grid.start[tile + 1]);

    // A filled interior with no stroke nearby is the normal case, not an edge
    // case, so bailing out here has to still deliver the fill.
    if (!haveTile)
    {
        if (fillCov > 0.0f)
            AccumulateOver(fillStyle.fillColor,
                           fillCov * fillStyle.fillOpacity * fillStyle.opacity, out);
        return out;
    }

    const int lo = scene.grid.start[tile];
    const int hi = scene.grid.start[tile + 1];

    // --- nearest primitive ---------------------------------------------------
    // The shadow is sampled from the same loop at a displaced position rather
    // than from a second pass over a blurred buffer. One traversal, two answers.
    //
    // Each segment is displaced by ITS OWN style's offset. The alternative --
    // one offset chosen up front -- is not merely less flexible, it is circular:
    // the offset lives in the style, and which style applies is not known until
    // the nearest segment has been found, which is what this loop is for.
    float d  = 1e30f;
    float ds = 1e30f;
    float s  = 0.0f;
    float u  = 0.0f;

    int styleIdx       = 0;
    int shadowStyleIdx = 0;

    // Attribute ownership is tracked SEPARATELY from the union distance.
    //
    // Taking the gradient coordinates from whichever primitive happens to be
    // nearest is unstable exactly where an arrowhead overlaps its own shaft:
    // both contain the pixel, their coordinates disagree sharply, and which one
    // wins is decided by a difference of about 1e-7. That draws a hard seam
    // along the medial axis between them, and it is not even the same seam
    // twice -- the CPU and the GPU round that comparison differently, which is
    // how the parity suite found it.
    //
    // A head that contains the pixel owns it outright. The head is the terminal
    // element; its own coordinates are the ones that should apply throughout it,
    // and "contains" is a sign test rather than a near-tie, so both paths agree.
    float attrD   = 1e30f;
    bool  attrTri = false;

    for (int i = lo; i < hi; ++i)
    {
        const int idx = scene.grid.items[i];
        if (idx < 0 || idx >= scene.segCount) continue;

        const Segment& seg = scene.segs[idx];

        const DistanceHit hit = DistanceToSegment(p, seg);
        if (hit.d < d) d = hit.d;                 // the union is always the min

        const bool tri = (seg.flags & Segment::kTriangle) != 0;
        if (tri && hit.d <= 0.0f)
        {
            if (!attrTri || hit.d < attrD)
            {
                attrD = hit.d; attrTri = true; s = hit.s; u = hit.u; styleIdx = seg.style;
            }
        }
        else if (!attrTri && hit.d < attrD)
        {
            attrD = hit.d; s = hit.s; u = hit.u; styleIdx = seg.style;
        }

        const Style& segSt = scene.StyleAt(seg.style);
        if (segSt.shadow)
        {
            const Vec2  ps(px - segSt.shadowDx, py - segSt.shadowDy);
            const float dsh = DistanceToSegment(ps, seg).d;
            if (dsh < ds) { ds = dsh; shadowStyleIdx = seg.style; }
        }
    }

    const Style& st = scene.StyleAt(styleIdx);
    const float  hf = st.feather * 0.5f;
    if (d > 1e29f && ds > 1e29f)
    {
        if (fillCov > 0.0f)
            AccumulateOver(fillStyle.fillColor,
                           fillCov * fillStyle.fillOpacity * fillStyle.opacity, out);
        return out;
    }

    // --- drop shadow, beneath everything -------------------------------------
    if (ds < 1e29f)
    {
        // Shaded with the style of whichever stroke actually cast it, not of the
        // stroke nearest the pixel -- a shadow reaching out from under a
        // neighbour still belongs to the thing that threw it.
        const Style& ss = scene.StyleAt(shadowStyleIdx);

        // Softness measured OUTWARD from the silhouette, so a zero-softness
        // shadow is a hard offset copy rather than a shape that has quietly
        // shrunk by half the falloff.
        const float cov = 1.0f - SmoothStep(0.0f, Max(ss.shadowSoft, 1e-4f), ds);
        AccumulateOver(ss.shadowColor, cov * ss.shadowOpacity * ss.opacity, out);
    }

    // --- glow ----------------------------------------------------------------
    // A distance falloff, not a blur. Squared so it reads as light rather than
    // as a flat halo with a visible rim.
    if (st.glow && st.glowRadius > 0.0f && d < 1e29f)
    {
        const float t = 1.0f - Saturate(d / st.glowRadius);
        AccumulateOver(st.glowColor, t * t * st.glowIntensity * st.opacity, out);
    }

    // --- fill, above the glow and below the stroke ---------------------------
    if (fillCov > 0.0f)
        AccumulateOver(fillStyle.fillColor,
                       fillCov * fillStyle.fillOpacity * fillStyle.opacity, out);

    if (d > 1e29f) return out;

    // --- outline -------------------------------------------------------------
    if (st.outline && st.outlineWidth > 0.0f)
    {
        const float cov = 1.0f - SmoothStep(st.outlineWidth - hf,
                                            st.outlineWidth + hf, d);
        AccumulateOver(st.outlineColor, cov * st.opacity, out);
    }

    // --- core stroke ---------------------------------------------------------
    Color core = st.color;
    if (st.gradMode != kGradNone)
    {
        // u is signed across the stroke, so Across runs genuinely edge to edge
        // while Centre folds it to get the symmetric look. Driving both from the
        // same signed value is what makes the difference between the two a
        // choice rather than a limitation.
        float raw = 0.0f;
        if      (st.gradMode == kGradAlong)  raw = Saturate(s);
        else if (st.gradMode == kGradAcross) raw = (u + 1.0f) * 0.5f;
        else                                 raw = (u < 0.0f) ? -u : u;

        core = LerpColor(st.color, st.gradColor,
                         GradientT(raw, st.gradStart, st.gradEnd, st.gradMid));
    }

    AccumulateOver(core, (1.0f - SmoothStep(-hf, hf, d)) * st.opacity, out);
    return out;
}

/** @brief Shade a rectangle of pixels and composite over an existing buffer.
 *
 * @param dst        float RGBA, premultiplied
 * @param rowStride  FLOATS per row, not bytes, and legitimately negative -- OFX
 *                   images are bottom-up, and the flip is done by walking
 *                   backwards rather than by moving any pixels.
 */
inline void ShadeRect(const SceneView& scene, float* dst, int rowStride,
                      int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; ++y)
    {
        float* row = dst + static_cast<ptrdiff_t>(y - y0) * rowStride;
        for (int x = x0; x < x1; ++x)
        {
            // +0.5 puts the sample at the pixel centre. Sampling the corner
            // instead drifts the whole image half a pixel, which is invisible
            // at 1:1 and obvious the moment anything is scaled up.
            const Premul src = ShadePixel(scene,
                                          static_cast<float>(x) + 0.5f,
                                          static_cast<float>(y) + 0.5f);
            if (src.a > 0.0f) Over(src, row + static_cast<ptrdiff_t>(x - x0) * 4);
        }
    }
}

} // namespace bl
