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

/** @brief Serialise strokes to the blob format.
 *
 * v2 adds a paint block per subpath: `<kind>:<paint>:<points>`. v1 files, which
 * have no paint block, still load -- an import someone already made keeps
 * working and simply uses the global style, which is what it did anyway.
 */
inline std::string StrokesToBlob(const std::vector<Stroke>& strokes)
{
    // v3 stores SIX numbers per point: position, outgoing handle, incoming
    // handle. Imported artwork routinely has asymmetric handles, and storing one
    // and mirroring it bends every span whose two ends disagree.
    std::string out = "v3";
    char buf[256];

    for (const Stroke& s : strokes)
    {
        if (s.pts.size() < 2) continue;

        out += ';';
        out += s.closed ? 'C' : 'O';
        out += ':';

        const StrokePaint& k = s.paint;
        const int flags = (k.hasStroke ? 1 : 0) | (k.hasFill ? 2 : 0);
        std::snprintf(buf, sizeof(buf),
                      "%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.6g,%d,%d:",
                      k.strokeColor.r, k.strokeColor.g, k.strokeColor.b, k.strokeColor.a,
                      k.fillColor.r, k.fillColor.g, k.fillColor.b, k.fillColor.a,
                      k.width, flags, k.group);
        out += buf;

        bool first = true;
        for (const ControlPoint& p : s.pts)
        {
            if (!first) out += ' ';
            first = false;
            // Six significant digits: one part in a million of the frame is far
            // finer than a pixel at 8K, and the file stays readable.
            const Vec2 in = p.hasIn ? p.tanIn : Vec2(-p.tan.x, -p.tan.y);
            std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g",
                          p.pos.x, p.pos.y, p.tan.x, p.tan.y, in.x, in.y);
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

    // The version decides whether a paint block precedes the points. Telling the
    // two apart by counting colons would work until a malformed string arrived.
    const int version = (blob.compare(0, 2, "v3") == 0) ? 3
                      : (blob.compare(0, 2, "v2") == 0) ? 2 : 1;

    size_t i = 0;
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

        if (version >= 2)
        {
            double k[11] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0};
            int got = 0;
            for (; got < 11 && i < blob.size() && blob[i] != ';'; ++got)
            {
                const char* start = blob.c_str() + i;
                char* end = nullptr;
                k[got] = std::strtod(start, &end);
                if (end == start) break;
                i += static_cast<size_t>(end - start);
                if (i < blob.size() && (blob[i] == ',' || blob[i] == ':')) ++i;
            }

            if (got >= 10)
            {
                s.paint.strokeColor = Color(static_cast<float>(k[0]), static_cast<float>(k[1]),
                                            static_cast<float>(k[2]), static_cast<float>(k[3]));
                s.paint.fillColor   = Color(static_cast<float>(k[4]), static_cast<float>(k[5]),
                                            static_cast<float>(k[6]), static_cast<float>(k[7]));
                s.paint.width     = static_cast<float>(k[8]);
                const int flags   = static_cast<int>(k[9]);
                s.paint.hasStroke = (flags & 1) != 0;
                s.paint.hasFill   = (flags & 2) != 0;
                s.paint.group     = static_cast<int>(k[10]);
            }
        }

        while (i < blob.size() && blob[i] != ';')
        {
            while (i < blob.size() && blob[i] == ' ') ++i;
            if (i >= blob.size() || blob[i] == ';') break;

            const int want = (version >= 3) ? 6 : 4;

            double v[6] = {0, 0, 0, 0, 0, 0};
            int got = 0;
            for (; got < want; ++got)
            {
                const char* start = blob.c_str() + i;
                char* end = nullptr;
                v[got] = std::strtod(start, &end);
                if (end == start) break;
                i += static_cast<size_t>(end - start);
                if (i < blob.size() && blob[i] == ',') ++i;
                else { ++got; break; }
            }
            if (got < want) break;            // a truncated point ends the subpath

            ControlPoint cp;
            cp.pos = Vec2(static_cast<float>(v[0]), static_cast<float>(v[1]));
            cp.tan = Vec2(static_cast<float>(v[2]), static_cast<float>(v[3]));
            if (version >= 3)
            {
                cp.tanIn = Vec2(static_cast<float>(v[4]), static_cast<float>(v[5]));
                cp.hasIn = true;
            }
            s.pts.push_back(cp);
        }

        if (s.pts.size() >= 2) out.push_back(s);
    }
}

} // namespace bl
