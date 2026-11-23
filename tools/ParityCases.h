#pragma once

// The CPU-vs-GPU parity cases, shared by tools/cudatest (Windows) and
// tools/metaltest (macOS) so both GPU paths are held to the same scenes. Adding
// a case here adds it to both.

#include "core/Geometry.h"
#include "core/PathBuild.h"
#include "core/Raster.h"

#include <vector>

namespace parity {

using namespace bl;

struct Case
{
    const char* name;
    Stroke      stroke;
    Style       style;
    TrimState   trim;
    float       dashLen = 0.0f;
    float       gapLen  = 0.0f;
};

inline ControlPoint CP(float x, float y, float tx = 0.0f, float ty = 0.0f)
{
    ControlPoint c;
    c.pos = Vec2(x, y);
    c.tan = Vec2(tx, ty);
    return c;
}

/** Cases chosen to exercise every branch the kernel can take, not to look nice.
 *  A parity suite made of pretty pictures tests the easy paths twice and the
 *  degenerate ones never. */
inline std::vector<Case> MakeCases()
{
    std::vector<Case> v;
    {
        Case c; c.name = "plain";
        c.stroke.pts = { CP(30, 40), CP(220, 200) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.5f;
        v.push_back(c);
    }
    {
        Case c; c.name = "curve + taper body";
        c.stroke.pts = { CP(30, 40, 80, 30), CP(128, 200, 60, 0), CP(220, 60, 0, -70) };
        c.style.halfWidth = 12.0f; c.style.feather = 1.2f;
        c.style.widthStart = 0.0f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        v.push_back(c);
    }
    {
        Case c; c.name = "butt dashes on curve";
        c.stroke.pts = { CP(30, 90, 70, 90), CP(220, 90, 70, -90) };
        c.style.halfWidth = 11.0f; c.style.feather = 1.0f; c.style.cap = kCapButt;
        c.dashLen = 14.0f; c.gapLen = 26.0f;
        v.push_back(c);
    }
    {
        Case c; c.name = "outline + glow + gradient";
        c.stroke.pts = { CP(40, 60, 70, 70), CP(210, 190, 40, -70) };
        c.style.halfWidth = 8.0f; c.style.feather = 1.4f;
        c.style.outline = true; c.style.outlineWidth = 4.0f;
        c.style.outlineColor = Color(0.1f, 0.0f, 0.2f, 1.0f);
        c.style.glow = true; c.style.glowRadius = 20.0f; c.style.glowIntensity = 0.9f;
        c.style.glowColor = Color(0.3f, 0.6f, 1.0f, 1.0f);
        c.style.gradMode = kGradAlong; c.style.gradColor = Color(1.0f, 0.2f, 0.1f, 1.0f);
        v.push_back(c);
    }
    {   // The seamless fold. A separate branch in the shared kernel, so it needs
        // its own parity case -- the default is off and every other gradient case
        // would agree while this one diverged.
        Case c; c.name = "closed gradient, seamless";
        c.stroke.pts = { CP(60, 128, 0, 55), CP(128, 196, 55, 0),
                         CP(196, 128, 0, -55), CP(128, 60, -55, 0) };
        c.stroke.closed = true;
        c.style.halfWidth = 12.0f; c.style.feather = 1.2f;
        c.style.gradMode = kGradAlong; c.style.gradLoop = true;
        c.style.gradColor = Color(0.1f, 0.4f, 1.0f, 1.0f);
        v.push_back(c);
    }
    {
        Case c; c.name = "zero feather, hard edge";
        c.stroke.pts = { CP(40, 200, 90, -80), CP(210, 50, 40, 60) };
        c.style.halfWidth = 10.0f; c.style.feather = 0.0f;   // SmoothStep step branch
        v.push_back(c);
    }
    {
        Case c; c.name = "sub-pixel hairline";
        c.stroke.pts = { CP(30, 70, 80, 50), CP(220, 190, 40, -50) };
        c.style.halfWidth = 0.35f; c.style.feather = 1.0f;
        v.push_back(c);
    }
    {
        Case c; c.name = "closed, trim across seam";
        const float k = 0.5523f * 78.0f;
        c.stroke.pts = { CP(128,  50,   k,  0), CP(206, 128,  0,  k),
                         CP(128, 206,  -k,  0), CP(50,  128,  0, -k) };
        c.stroke.closed = true;
        c.style.halfWidth = 9.0f; c.style.feather = 1.2f; c.style.cap = kCapSquare;
        c.trim.end = 0.6f; c.trim.offset = 0.75f;
        v.push_back(c);
    }
    {
        Case c; c.name = "shadow + arrows + trim";
        c.stroke.pts = { CP(30, 40, 80, 30), CP(128, 200, 60, 0), CP(220, 60, 0, -70) };
        c.style.halfWidth = 6.0f; c.style.feather = 1.3f;
        c.style.arrowStart = true; c.style.arrowEnd = true;
        c.style.arrowLength = 22.0f; c.style.arrowWidth = 16.0f;
        c.style.shadow = true; c.style.shadowDx = 8.0f; c.style.shadowDy = -8.0f;
        c.style.shadowSoft = 9.0f; c.style.shadowOpacity = 0.7f;
        c.trim.start = 0.12f; c.trim.end = 0.8f;
        v.push_back(c);
    }
    {
        // Triangle interior coordinates: the arrowhead derives its own arc and
        // cross-stroke position from its vertices, and the whole stroke's arc
        // domain is renormalised to span the heads. Plenty to disagree about.
        Case c; c.name = "arrow gradients, both ends";
        c.stroke.pts = { CP(30, 60, 80, 40), CP(220, 190, 50, -60) };
        c.style.halfWidth = 12.0f; c.style.feather = 1.2f;
        c.style.arrowStart = true; c.style.arrowEnd = true;
        c.style.arrowLength = 40.0f; c.style.arrowWidth = 34.0f;
        c.style.arrowRound = 5.0f;
        c.style.gradMode = kGradAcross;
        c.style.gradColor = Color(0.1f, 0.3f, 1.0f, 1.0f);
        c.style.gradStart = 0.15f; c.style.gradEnd = 0.85f; c.style.gradMid = 0.35f;
        v.push_back(c);
    }
    {
        // Fill: a whole second acceleration structure, a winding number and a
        // separate distance, none of which the other cases touch.
        Case c; c.name = "filled circle + dashes";
        const float k = 0.5523f * 80.0f;
        c.stroke.pts = { CP(128,  48,   k,  0), CP(208, 128,  0,  k),
                         CP(128, 208,  -k,  0), CP(48,  128,  0, -k) };
        c.stroke.closed = true;
        c.style.halfWidth = 7.0f; c.style.feather = 1.3f;
        c.style.fill = true;
        c.style.fillColor = Color(0.2f, 0.5f, 0.35f, 1.0f);
        c.style.fillOpacity = 0.7f;
        c.dashLen = 20.0f; c.gapLen = 12.0f;
        v.push_back(c);
    }
    {
        // Self-intersecting, so the winding accumulates past 1 and back.
        Case c; c.name = "fill, self-intersecting";
        c.stroke.pts = { CP(40, 50), CP(210, 200), CP(210, 50), CP(40, 200) };
        c.stroke.closed = true;
        c.style.halfWidth = 5.0f; c.style.feather = 1.2f;
        c.style.fill = true;
        c.style.fillColor = Color(0.8f, 0.3f, 0.2f, 1.0f);
        v.push_back(c);
    }
    {
        Case c; c.name = "empty (fully collapsed trim)";
        c.stroke.pts = { CP(30, 40), CP(220, 200) };
        c.style.halfWidth = 9.0f;
        c.trim.start = 0.5f; c.trim.end = 0.5f;   // emits nothing at all
        v.push_back(c);
    }
    return v;
}

constexpr int kSize = 256;

} // namespace parity
