#pragma once

// Acceleration structure for closed-path fill.
//
// Fill needs a winding number, and a winding number needs every edge whose
// y-range straddles the pixel's scanline -- including edges far to the left or
// right. The 16x16 tile grid cannot answer that: it holds what is NEARBY, and an
// edge on the far side of a large shape is not nearby but does decide whether
// the pixel is inside it.
//
// So fill gets its own structure, binned by horizontal BAND rather than by tile.
// A band is a run of scanlines, and it lists every edge overlapping that band's
// y-range. Any edge that can affect a pixel's winding is in that pixel's band by
// construction. For a rounded box or a circle a band holds a handful of edges,
// so the per-pixel cost stays small however large the shape is on screen.

#include "Common.h"
#include "Primitives.h"

#include <vector>

namespace bl {

/** @brief One edge of a closed contour. Fill is the only consumer. */
struct FillEdge
{
    Vec2 a;
    Vec2 b;
    int  style = 0;   ///< which stroke's style fills this contour
};

/** @brief Device-side view: plain pointers, no ownership. */
struct BandGridView
{
    int bandHeight = 16;
    int rows       = 0;
    int originY    = 0;

    const int* start = nullptr;   ///< rows + 1 offsets into items (CSR)
    const int* items = nullptr;

    BL_HD int BandOf(int py) const
    {
        const int r = (py - originY) / bandHeight;
        return (r < 0 || r >= rows) ? -1 : r;
    }
};

/** @brief Owning band grid. Built per frame on the CPU, uploaded as-is. */
struct BandGrid
{
    int bandHeight = 16;
    int rows = 0, originY = 0;
    std::vector<int> start;
    std::vector<int> items;

    BandGridView View() const
    {
        BandGridView v;
        v.bandHeight = bandHeight;
        v.rows       = rows;
        v.originY    = originY;
        v.start = start.empty() ? nullptr : start.data();
        v.items = items.empty() ? nullptr : items.data();
        return v;
    }
};

/** @brief Bin fill edges into horizontal bands covering [y0, y1) in pixels.
 *
 * @param margin vertical slack. Winding alone needs none -- an edge straddling
 *               the pixel's scanline necessarily overlaps its band -- but the
 *               same edge list also supplies the distance used to antialias the
 *               fill boundary, and a pixel a fraction under an edge would
 *               otherwise find its nearest edge sitting one band away. Without
 *               the slack the fill edge goes hard along every band boundary.
 *
 * Counting pass then fill pass, as with the tile grid: a vector per band would
 * allocate hundreds of times per frame inside the render action.
 */
inline void BuildBandGrid(const std::vector<FillEdge>& edges, float margin,
                          int y0, int y1, int bandHeight, BandGrid& out)
{
    out.bandHeight = bandHeight > 0 ? bandHeight : 16;
    out.originY    = y0;
    out.rows = (y1 - y0 + out.bandHeight - 1) / out.bandHeight;
    if (out.rows < 0) out.rows = 0;

    out.start.assign(static_cast<size_t>(out.rows) + 1, 0);
    out.items.clear();
    if (out.rows == 0 || edges.empty()) return;

    // Band span of one edge. Shared by both passes so they cannot disagree --
    // a counting pass and a fill pass using different rules corrupt the CSR
    // silently rather than loudly.
    auto spanOf = [&](const FillEdge& e, int& r0, int& r1) {
        const float lo = Min(e.a.y, e.b.y) - margin;
        const float hi = Max(e.a.y, e.b.y) + margin;
        r0 = (static_cast<int>(::floorf(lo)) - out.originY) / out.bandHeight;
        r1 = (static_cast<int>(::floorf(hi)) - out.originY) / out.bandHeight;
        if (r0 < 0) r0 = 0;
        if (r1 >= out.rows) r1 = out.rows - 1;
    };

    for (const FillEdge& e : edges)
    {
        int r0, r1;
        spanOf(e, r0, r1);
        for (int r = r0; r <= r1; ++r) ++out.start[static_cast<size_t>(r) + 1];
    }

    for (int r = 0; r < out.rows; ++r) out.start[r + 1] += out.start[r];
    out.items.resize(static_cast<size_t>(out.start[out.rows]));

    std::vector<int> cursor(out.start.begin(), out.start.end() - 1);
    for (size_t i = 0; i < edges.size(); ++i)
    {
        int r0, r1;
        spanOf(edges[i], r0, r1);
        for (int r = r0; r <= r1; ++r) out.items[cursor[static_cast<size_t>(r)]++] = static_cast<int>(i);
    }
}

/** @brief Append a flattened closed contour to the fill edge list.
 *
 * The contour is closed explicitly rather than trusting the flattener to land
 * exactly back on its first point: it drops duplicate vertices as it goes, so a
 * loop that closes cleanly ends one point SHORT of repeating itself. Leaving
 * that final edge out opens the contour, and an open contour's winding number is
 * meaningless -- the fill leaks across the whole scanline.
 */
inline void AppendFillContour(const std::vector<Vec2>& pts, int style,
                              std::vector<FillEdge>& out)
{
    if (pts.size() < 3) return;

    for (size_t i = 0; i + 1 < pts.size(); ++i) out.push_back({pts[i], pts[i + 1], style});

    if (Dot2(pts.back() - pts.front()) > 1e-12f)
        out.push_back({pts.back(), pts.front(), style});
}

} // namespace bl
