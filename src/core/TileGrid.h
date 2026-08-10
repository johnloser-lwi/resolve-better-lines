#pragma once

// Uniform grid binning primitives to 16x16 pixel tiles.
//
// Without this the kernel is O(pixels * primitives), and a curve flattened at
// sub-pixel tolerance produces thousands of primitives. With it, a pixel only
// ever tests the handful that could actually reach it, which is what makes the
// cost scale with the length of the line rather than with its complexity.

#include "Common.h"
#include "Primitives.h"

#include <vector>

namespace bl {

/** @brief Device-side view of the grid: plain pointers, no ownership. */
struct TileGridView
{
    int tileSize = 16;
    int cols     = 0;
    int rows     = 0;
    int originX  = 0;   ///< pixel x of tile column 0
    int originY  = 0;

    const int* start = nullptr;   ///< cols*rows + 1 offsets into items (CSR)
    const int* items = nullptr;   ///< primitive indices

    BL_HD int TileOf(int px, int py) const
    {
        const int cx = (px - originX) / tileSize;
        const int cy = (py - originY) / tileSize;
        if (cx < 0 || cy < 0 || cx >= cols || cy >= rows) return -1;
        return cy * cols + cx;
    }
};

/** @brief Owning grid. Built once per frame on the CPU, then uploaded as-is. */
struct TileGrid
{
    int tileSize = 16;
    int cols = 0, rows = 0, originX = 0, originY = 0;
    std::vector<int> start;
    std::vector<int> items;

    TileGridView View() const
    {
        TileGridView v;
        v.tileSize = tileSize;
        v.cols = cols; v.rows = rows;
        v.originX = originX; v.originY = originY;
        v.start = start.empty() ? nullptr : start.data();
        v.items = items.empty() ? nullptr : items.data();
        return v;
    }
};

/** @brief Bin segments into tiles covering [x0,x1) x [y0,y1) in pixels.
 *
 * @param reach how far beyond its own geometry a primitive can still paint --
 *              feather, outline and glow all extend a primitive's influence, and
 *              a reach that under-reports produces the bug where a glow is
 *              sliced off along invisible 16-pixel boundaries.
 *
 * Counting pass then fill pass, i.e. a counting sort. The alternative -- a
 * vector per tile -- allocates thousands of times per frame inside the render
 * action, which costs more than the binning itself.
 */
inline void BuildTileGrid(const std::vector<Segment>& segs, float reach,
                          int x0, int y0, int x1, int y1,
                          int tileSize, TileGrid& out)
{
    out.tileSize = tileSize > 0 ? tileSize : 16;
    out.originX  = x0;
    out.originY  = y0;
    out.cols = (x1 - x0 + out.tileSize - 1) / out.tileSize;
    out.rows = (y1 - y0 + out.tileSize - 1) / out.tileSize;
    if (out.cols < 0) out.cols = 0;
    if (out.rows < 0) out.rows = 0;

    const int nTiles = out.cols * out.rows;
    out.start.assign(static_cast<size_t>(nTiles) + 1, 0);
    out.items.clear();
    if (nTiles == 0 || segs.empty()) return;

    // Tile span of one primitive, clamped to the grid. Returned by out-params
    // rather than a struct so both passes are provably using the same rule --
    // a counting pass and a fill pass that disagree corrupt the CSR silently.
    auto spanOf = [&](const Segment& s, int& cx0, int& cy0, int& cx1, int& cy1) {
        Bounds bb = s.BoundsOf();
        bb.Expand(reach);
        cx0 = (static_cast<int>(::floorf(bb.x0)) - out.originX) / out.tileSize;
        cy0 = (static_cast<int>(::floorf(bb.y0)) - out.originY) / out.tileSize;
        cx1 = (static_cast<int>(::floorf(bb.x1)) - out.originX) / out.tileSize;
        cy1 = (static_cast<int>(::floorf(bb.y1)) - out.originY) / out.tileSize;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 >= out.cols) cx1 = out.cols - 1;
        if (cy1 >= out.rows) cy1 = out.rows - 1;
    };

    for (const Segment& s : segs)
    {
        int cx0, cy0, cx1, cy1;
        spanOf(s, cx0, cy0, cx1, cy1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                ++out.start[static_cast<size_t>(cy) * out.cols + cx + 1];
    }

    for (int i = 0; i < nTiles; ++i) out.start[i + 1] += out.start[i];
    out.items.resize(static_cast<size_t>(out.start[nTiles]));

    std::vector<int> cursor(out.start.begin(), out.start.end() - 1);
    for (size_t i = 0; i < segs.size(); ++i)
    {
        int cx0, cy0, cx1, cy1;
        spanOf(segs[i], cx0, cy0, cx1, cy1);
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx)
                out.items[cursor[static_cast<size_t>(cy) * out.cols + cx]++] = static_cast<int>(i);
    }
}

} // namespace bl
