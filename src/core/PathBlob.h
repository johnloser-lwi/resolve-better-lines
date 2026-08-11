#pragma once

// Serialised path geometry for IMPORTED artwork.
//
// A second store alongside the keyframable point slots, and the split is
// deliberate. The slots exist so Resolve can keyframe an individual point, which
// costs two parameters per point and caps the plugin at 4 x 16. An imported logo
// is exactly the thing nobody keyframes point-by-point -- it gets placed, scaled
// and drawn on as a whole, all of which the global transform and the trim
// already do. So it lives in one string parameter instead, with no ceiling.
//
// Format, deliberately one line: a host string parameter is not guaranteed to
// survive newlines intact, and a geometry store that silently loses a character
// is a geometry store that silently loses a shape.
//
//   v1;O:x,y,tx,ty x,y,tx,ty;C:x,y,tx,ty ...
//
// 'O' opens a subpath, 'C' closes one. Each point is its position followed by
// its outgoing bezier handle, all in the normalised space described below.

#include "Common.h"
#include "Geometry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace bl {

/** @brief Coordinate convention for blob paths, which is NOT the slots'.
 *
 * Slot points are a fraction of width in x and of height in y, so a square drawn
 * by hand stays where the user put it on any frame. Imported artwork has the
 * opposite requirement: a circle in the SVG has to come out circular, and
 * stretching x by the frame's aspect would make it an ellipse on anything but a
 * square frame.
 *
 * So blob coordinates are SQUARE units centred on 0.5: y still spans the frame
 * height, and x is scaled by the height too and centred. One unit means the same
 * number of pixels on both axes, which is what keeps imported shapes the shape
 * they were drawn as.
 */
BL_HD inline Vec2 BlobToPixels(Vec2 n, float rodX1, float rodY1, float w, float h)
{
    return Vec2(rodX1 + w * 0.5f + (n.x - 0.5f) * h,
                rodY1 + n.y * h);
}

/// The matching direction mapping, for bezier handles.
BL_HD inline Vec2 BlobVectorToPixels(Vec2 v, float h)
{
    return Vec2(v.x * h, v.y * h);
}

/** @brief Serialise strokes to the blob format. */
inline std::string StrokesToBlob(const std::vector<Stroke>& strokes)
{
    std::string out = "v1";
    char buf[128];

    for (const Stroke& s : strokes)
    {
        if (s.pts.size() < 2) continue;

        out += ';';
        out += s.closed ? 'C' : 'O';
        out += ':';

        bool first = true;
        for (const ControlPoint& p : s.pts)
        {
            if (!first) out += ' ';
            first = false;
            // Six significant digits: one part in a million of the frame is far
            // finer than a pixel at 8K, and the file stays readable.
            std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g",
                          p.pos.x, p.pos.y, p.tan.x, p.tan.y);
            out += buf;
        }
    }
    return out;
}

/** @brief Parse the blob format. Unparseable input yields no strokes.
 *
 * Lenient by design: this string is reachable from the Inspector, so a
 * half-typed edit must produce a shorter path rather than throw mid-render.
 */
inline void BlobToStrokes(const std::string& blob, std::vector<Stroke>& out)
{
    out.clear();
    if (blob.size() < 2) return;

    size_t i = 0;
    // Skip the version tag. Nothing keys off it yet; it exists so a future
    // format change can be told apart from a corrupt string.
    while (i < blob.size() && blob[i] != ';') ++i;

    while (i < blob.size())
    {
        ++i;                                  // past the ';'
        if (i >= blob.size()) break;

        const char kind = blob[i];
        if (kind != 'O' && kind != 'C') { while (i < blob.size() && blob[i] != ';') ++i; continue; }

        i += 1;
        if (i < blob.size() && blob[i] == ':') ++i;

        Stroke s;
        s.closed = (kind == 'C');

        while (i < blob.size() && blob[i] != ';')
        {
            while (i < blob.size() && blob[i] == ' ') ++i;
            if (i >= blob.size() || blob[i] == ';') break;

            double v[4] = {0, 0, 0, 0};
            int got = 0;
            for (; got < 4; ++got)
            {
                const char* start = blob.c_str() + i;
                char* end = nullptr;
                v[got] = std::strtod(start, &end);
                if (end == start) break;
                i += static_cast<size_t>(end - start);
                if (i < blob.size() && blob[i] == ',') ++i;
                else { ++got; break; }
            }
            if (got < 4) break;               // a truncated point ends the subpath

            ControlPoint cp;
            cp.pos = Vec2(static_cast<float>(v[0]), static_cast<float>(v[1]));
            cp.tan = Vec2(static_cast<float>(v[2]), static_cast<float>(v[3]));
            s.pts.push_back(cp);
        }

        if (s.pts.size() >= 2) out.push_back(s);
    }
}

} // namespace bl
