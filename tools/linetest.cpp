// Offline renderer for src/core. No OFX, no Resolve, no GPU.
//
// This is where antialiasing quality, cap shapes, dash boundaries and taper
// continuity get judged, because judging them by scrubbing a timeline is slow
// and you cannot diff two timelines.
//
//   linetest out.png            renders the full contact sheet
//   linetest out.png --cell 320 bigger cells
//
// NOTE: this uses the CPU path. It is not proof the CUDA kernel agrees -- that
// is what tools/cudatest is for.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "core/Geometry.h"
#include "core/PathBuild.h"
#include "core/Raster.h"
#include "core/TileGrid.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace bl;

namespace {

struct Case
{
    const char* name;
    Stroke      stroke;
    Style       style;
    TrimState   trim;
    float       dashLen = 0.0f;
    float       gapLen  = 0.0f;
    float       phase   = 0.0f;

    // Animation. When enabled the case is evaluated at `frame`, through the same
    // core seed/apply/write path the plugin uses.
    bool  anim       = false;
    Stage stIn, stOut;
    float clipLength = 60.0f;
    float frame      = 0.0f;
    float pctToPx    = 2.56f;   ///< a 256px cell, so 1% of height is 2.56px

    // Wiggle, in pixels.
    float    wiggleAmp   = 0.0f;
    float    wiggleWave  = 30.0f;
    float    wigglePhase = 0.0f;
    unsigned wiggleSeed  = 1;
};

/// Name buffers for generated case labels, kept alive for the run.
///
/// A deque, not a vector: growing a vector<string> MOVES its elements, and a
/// short string moved to a new address takes its small-string buffer with it --
/// so every c_str() handed out earlier would dangle. A deque never relocates
/// what it already holds.
std::deque<std::string> g_names;
const char* KeepName(const std::string& s)
{
    g_names.push_back(s);
    return g_names.back().c_str();
}

ControlPoint CP(float x, float y, float tx = 0.0f, float ty = 0.0f)
{
    ControlPoint c;
    c.pos = Vec2(x, y);
    c.tan = Vec2(tx, ty);
    return c;
}

/** Render one case into a cell-sized premultiplied RGBA buffer. */
void RenderCase(const Case& c, int size, std::vector<float>& rgba)
{
    rgba.assign(static_cast<size_t>(size) * size * 4, 0.0f);

    // Evaluate the animation through the SAME core path the plugin uses, so a
    // strip rendered here is evidence about what Resolve will do rather than
    // about a lookalike written for the harness.
    Style     style = c.style;
    TrimState trim  = c.trim;
    float     phase = c.phase;
    if (c.anim)
    {
        AnimTargets av;
        SeedTargets(style, trim,
                    style.halfWidth * 2.0f / c.pctToPx,   // px -> % of height
                    phase / c.pctToPx, av);
        ApplyStages(c.stIn, c.stOut, c.frame, c.clipLength, av.v);
        WriteTargets(av, c.pctToPx, style, trim, phase);
    }

    Polyline poly;
    FlattenStroke(c.stroke, 0.2f, poly);

    if (c.wiggleAmp > 1e-4f)
    {
        Polyline wobbled;
        WigglePolyline(poly, c.wiggleAmp, c.wiggleWave, c.wigglePhase, c.wiggleSeed, wobbled);
        poly = wobbled;
    }

    std::vector<FillEdge> fill;
    if (style.fill && c.stroke.closed) AppendFillContour(poly.pts, 0, fill);

    std::vector<Segment> segs;
    BuildStrokeSegments(poly, style, trim, c.stroke.closed,
                        c.dashLen, c.gapLen, phase, segs);

    TileGrid grid;
    BuildTileGrid(segs, style.MaxReach() + 1.0f, 0, 0, size, size, 16, grid);

    BandGrid bands;
    BuildBandGrid(fill, style.feather + 1.0f, 0, size, 16, bands);

    SceneView scene;
    scene.segs      = segs.empty() ? nullptr : segs.data();
    scene.segCount  = static_cast<int>(segs.size());
    scene.grid      = grid.View();
    scene.fill      = fill.empty() ? nullptr : fill.data();
    scene.fillCount = static_cast<int>(fill.size());
    scene.bands     = bands.View();
    scene.styles     = &style;
    scene.styleCount = 1;

    ShadeRect(scene, rgba.data(), size * 4, 0, 0, size, size);

    std::printf("  %-22s flat=%4zu segs=%4zu binned=%6zu fill=%4zu\n",
                c.name, poly.pts.size(), segs.size(), grid.items.size(), fill.size());
}

std::vector<Case> MakeCases()
{
    std::vector<Case> cases;

    // 1. plain thick line, round caps
    {
        Case c; c.name = "round caps";
        c.stroke.pts = { CP(40, 60), CP(216, 196) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.85f, 0.2f, 1.0f);
        cases.push_back(c);
    }
    // 2. butt caps -- should be visibly SHORTER than the round-cap version
    {
        Case c; c.name = "butt caps";
        c.stroke.pts = { CP(40, 60), CP(216, 196) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.2f; c.style.cap = kCapButt;
        c.style.color = Color(0.3f, 0.9f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 3. square caps -- same length as round, but with corners
    {
        Case c; c.name = "square caps";
        c.stroke.pts = { CP(40, 60), CP(216, 196) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.2f; c.style.cap = kCapSquare;
        c.style.color = Color(1.0f, 0.4f, 0.6f, 1.0f);
        cases.push_back(c);
    }
    // 4. bezier S-curve: checks flattening and join smoothness
    {
        Case c; c.name = "bezier S";
        c.stroke.pts = { CP(40, 40, 70, 0), CP(128, 128, 40, 60), CP(216, 216, 0, 70) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.6f, 1.0f, 0.5f, 1.0f);
        cases.push_back(c);
    }
    // 5. straight taper: Body set to the average of the ends, which is the case
    //    where the quadratic profile must collapse back to a plain linear ramp.
    {
        Case c; c.name = "taper linear 1->0.05";
        c.stroke.pts = { CP(40, 40, 90, 20), CP(216, 200, 20, -90) };
        c.style.halfWidth = 14.0f; c.style.feather = 1.2f;
        c.style.widthStart = 1.0f; c.style.widthEnd = 0.05f;
        c.style.widthMid   = 0.525f;                 // (1.0 + 0.05) / 2
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 6. dashes: every dash must get its own round caps
    {
        Case c; c.name = "dashes";
        c.stroke.pts = { CP(40, 128, 60, -80), CP(216, 128, 60, 80) };
        c.style.halfWidth = 6.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.7f, 0.3f, 1.0f);
        c.dashLen = 18.0f; c.gapLen = 12.0f;
        cases.push_back(c);
    }
    // 7. outline + glow
    {
        Case c; c.name = "outline + glow";
        c.stroke.pts = { CP(50, 90, 60, 60), CP(200, 170, 40, -60) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        c.style.outline = true; c.style.outlineWidth = 3.5f;
        c.style.outlineColor = Color(0.05f, 0.05f, 0.1f, 1.0f);
        c.style.glow = true; c.style.glowRadius = 22.0f; c.style.glowIntensity = 0.85f;
        c.style.glowColor = Color(0.2f, 0.5f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 8. gradient along arc length
    {
        Case c; c.name = "gradient along";
        c.stroke.pts = { CP(40, 200, 80, -60), CP(216, 60, 40, 60) };
        c.style.halfWidth = 11.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.2f, 0.3f, 1.0f);
        c.style.gradMode = kGradAlong;
        c.style.gradColor = Color(0.2f, 0.4f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 9. trim 0.15 -> 0.6: the draw-on reveal, mid-flight
    {
        Case c; c.name = "trim .15-.60";
        c.stroke.pts = { CP(40, 40, 90, 30), CP(128, 210, 60, 0), CP(216, 40, 0, -90) };
        c.style.halfWidth = 8.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.9f, 0.9f, 1.0f, 1.0f);
        c.trim.start = 0.15f; c.trim.end = 0.60f;
        cases.push_back(c);
    }
    // 10. closed square: the "highlight box" case
    {
        Case c; c.name = "closed square";
        c.stroke.pts = { CP(60, 60), CP(196, 60), CP(196, 196), CP(60, 196) };
        c.stroke.closed = true;
        c.style.halfWidth = 6.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.9f, 0.4f, 1.0f);
        cases.push_back(c);
    }
    // 11. closed circle, trim wrapping past the seam
    {
        const float k = 0.5523f * 78.0f;   // circle-from-cubics constant
        Case c; c.name = "closed trim wrap";
        c.stroke.pts = { CP(128,  50,    k,   0), CP(206, 128,   0,   k),
                         CP(128, 206,   -k,   0), CP(50,  128,   0,  -k) };
        c.stroke.closed = true;
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.5f, 1.0f, 0.8f, 1.0f);
        c.trim.start = 0.0f; c.trim.end = 0.5f; c.trim.offset = 0.8f;
        cases.push_back(c);
    }
    // 12. hairline: sub-pixel width must still antialias, not disappear
    {
        Case c; c.name = "hairline 0.4px";
        c.stroke.pts = { CP(40, 70, 80, 40), CP(216, 190, 40, -40) };
        c.style.halfWidth = 0.4f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 13a. Leaf taper on a STRAIGHT span -- zero handles, so the flattener
    //      correctly produces one segment. A round cone can only ramp its radius
    //      linearly, so without profile-driven subdivision this evaluates to
    //      radius 0 at both ends and the stroke disappears entirely. It looked
    //      like taper "only worked on curves".
    {
        Case c; c.name = "leaf on STRAIGHT";
        c.stroke.pts = { CP(40, 80), CP(216, 176) };
        c.style.halfWidth = 16.0f; c.style.feather = 1.2f;
        c.style.widthStart = 0.0f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        c.style.color = Color(1.0f, 0.6f, 0.9f, 1.0f);
        cases.push_back(c);
    }
    // 13b. Same profile on a corner polyline, where every span is straight.
    {
        Case c; c.name = "leaf on corners";
        c.stroke.pts = { CP(40, 60), CP(128, 200), CP(216, 60) };
        c.style.halfWidth = 15.0f; c.style.feather = 1.2f;
        c.style.widthStart = 0.0f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        c.style.color = Color(0.6f, 0.85f, 1.0f, 1.0f);
        cases.push_back(c);
    }
    // 13. leaf: both ends pinched to nothing, body at full width. This is the
    //     shape two stops could not express at all -- thin-to-thin interpolates
    //     to thin everywhere and the line simply vanishes.
    {
        Case c; c.name = "leaf 0 / 1 / 0";
        c.stroke.pts = { CP(40, 90, 70, 60), CP(216, 150, 70, -60) };
        c.style.halfWidth = 16.0f; c.style.feather = 1.2f;
        c.style.widthStart = 0.0f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        c.style.color = Color(0.75f, 1.0f, 0.6f, 1.0f);
        cases.push_back(c);
    }
    // 14. brush: fat body, one blunt end and one point.
    {
        Case c; c.name = "brush 0.35 / 1 / 0";
        c.stroke.pts = { CP(40, 60, 60, 90), CP(216, 190, 60, -50) };
        c.style.halfWidth = 15.0f; c.style.feather = 1.2f;
        c.style.widthStart = 0.35f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        c.style.color = Color(1.0f, 0.75f, 0.85f, 1.0f);
        cases.push_back(c);
    }
    // 15. pinched waist: body THINNER than the ends, which drives the Bezier
    //     control point negative and is where an unclamped profile would invert
    //     the round cone rather than pinch it.
    {
        Case c; c.name = "waist 1 / 0.05 / 1";
        c.stroke.pts = { CP(40, 128, 60, -70), CP(216, 128, 60, 70) };
        c.style.halfWidth = 15.0f; c.style.feather = 1.2f;
        c.style.widthStart = 1.0f; c.style.widthMid = 0.05f; c.style.widthEnd = 1.0f;
        c.style.color = Color(1.0f, 0.6f, 0.4f, 1.0f);
        cases.push_back(c);
    }

    // 16. arrowhead at the end: a connector
    {
        Case c; c.name = "arrow end";
        c.stroke.pts = { CP(40, 60, 80, 40), CP(210, 190, 40, -40) };
        c.style.halfWidth = 5.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.9f, 0.5f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 26.0f; c.style.arrowWidth = 20.0f;
        cases.push_back(c);
    }
    // 17. arrows both ends, and the arrow must ride the TRIMMED end rather than
    //     the path's own end -- otherwise it sits where the line has not reached.
    {
        Case c; c.name = "arrows + trim .2-.75";
        c.stroke.pts = { CP(40, 40, 90, 30), CP(128, 210, 60, 0), CP(216, 40, 0, -90) };
        c.style.halfWidth = 5.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.6f, 0.95f, 1.0f, 1.0f);
        c.style.arrowStart = true; c.style.arrowEnd = true;
        c.style.arrowLength = 24.0f; c.style.arrowWidth = 18.0f;
        c.trim.start = 0.2f; c.trim.end = 0.75f;
        cases.push_back(c);
    }
    // 18. drop shadow
    {
        Case c; c.name = "drop shadow";
        c.stroke.pts = { CP(40, 80, 70, 70), CP(210, 170, 40, -70) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        c.style.shadow = true; c.style.shadowDx = 9.0f; c.style.shadowDy = -9.0f;
        c.style.shadowSoft = 10.0f; c.style.shadowOpacity = 0.75f;
        cases.push_back(c);
    }
    // 19. hard shadow, zero softness: must be a crisp offset copy, not a shape
    //     that has quietly shrunk by half the falloff.
    {
        Case c; c.name = "hard shadow + dashes";
        c.stroke.pts = { CP(40, 128, 60, -70), CP(216, 128, 60, 70) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.5f, 0.4f, 1.0f);
        c.style.shadow = true; c.style.shadowDx = 7.0f; c.style.shadowDy = -7.0f;
        c.style.shadowSoft = 0.0f; c.style.shadowOpacity = 0.85f;
        c.dashLen = 20.0f; c.gapLen = 12.0f;
        cases.push_back(c);
    }

    return cases;
}

/** Cap-and-dash diagnostics, deliberately at exaggerated thickness.
 *
 * Cap shape is a few pixels of a normal line and invisible at a glance, which is
 * exactly why cap bugs survive. Everything here is fat enough that a wrong cap
 * is unmistakable. */
std::vector<Case> MakeCapCases()
{
    std::vector<Case> cases;

    auto line = [](const char* name, int cap, float dash, float gap, float hw) {
        Case c; c.name = name;
        c.stroke.pts = { CP(40, 128), CP(216, 128) };
        c.style.halfWidth = hw; c.style.feather = 1.0f; c.style.cap = cap;
        c.style.color = Color(1.0f, 0.95f, 0.9f, 1.0f);
        c.dashLen = dash; c.gapLen = gap;
        return c;
    };
    auto arc = [](const char* name, int cap, float dash, float gap, float hw) {
        Case c; c.name = name;
        c.stroke.pts = { CP(40, 90, 60, 90), CP(216, 90, 60, -90) };
        c.style.halfWidth = hw; c.style.feather = 1.0f; c.style.cap = cap;
        c.style.color = Color(0.7f, 0.95f, 1.0f, 1.0f);
        c.dashLen = dash; c.gapLen = gap;
        return c;
    };

    // Straight, fat, long dashes: the baseline. Butt must be a clean rectangle.
    // The gap is wider than twice the radius on purpose -- round and square caps
    // each extend a dash by one radius at both ends, so a narrower gap makes
    // them legitimately merge into a solid bar and the cap shape becomes
    // impossible to judge.
    cases.push_back(line("straight round",  kCapRound,  34, 52, 16));
    cases.push_back(line("straight BUTT",   kCapButt,   34, 52, 16));
    cases.push_back(line("straight square", kCapSquare, 34, 52, 16));
    cases.push_back(line("straight solid BUTT", kCapButt, 0, 0, 16));

    // Curved: segments a few pixels long against a radius many times that, so
    // every interior capsule wants to bulge through the run's flat end. This is
    // the case that clipping only the terminal segment gets wrong.
    cases.push_back(arc("curve round", kCapRound, 30, 46, 14));
    cases.push_back(arc("curve BUTT",  kCapButt,  30, 46, 14));
    cases.push_back(arc("curve BUTT fine", kCapButt, 11, 20, 14));
    cases.push_back(arc("curve square", kCapSquare, 30, 46, 14));

    // Dash shorter than the line is thick. A butt cap here is legitimately a
    // tall thin bar, not a dot -- worth being able to see that on purpose.
    cases.push_back(line("dash < thickness round", kCapRound, 6, 20, 16));
    cases.push_back(line("dash < thickness BUTT",  kCapButt,  6, 20, 16));
    // Taper plus dashes plus butt: radii differ across each dash.
    {
        Case c = arc("taper+dash BUTT", kCapButt, 24, 16, 16);
        c.style.widthStart = 1.0f; c.style.widthEnd = 0.15f;
        cases.push_back(c);
    }
    // Trim landing mid-dash, butt capped.
    {
        Case c = arc("trim+dash BUTT", kCapButt, 24, 30, 14);
        c.trim.start = 0.17f; c.trim.end = 0.71f;
        cases.push_back(c);
    }
    // Arrowheads, big enough to judge the actual silhouette. The flank must be
    // straight and the back flat -- a rounded back means the flat-cap plane was
    // not applied and the head is really a teardrop.
    {
        Case c; c.name = "arrow big straight";
        c.stroke.pts = { CP(40, 128), CP(216, 128) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 0.9f, 0.6f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 56.0f; c.style.arrowWidth = 44.0f;
        cases.push_back(c);
    }
    {
        Case c; c.name = "arrow big curve";
        c.stroke.pts = { CP(40, 80, 70, 80), CP(216, 80, 70, -80) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.0f;
        c.style.color = Color(0.7f, 1.0f, 0.8f, 1.0f);
        c.style.arrowStart = true; c.style.arrowEnd = true;
        c.style.arrowLength = 48.0f; c.style.arrowWidth = 38.0f;
        cases.push_back(c);
    }
    {
        // THE case that drove the position and direction rework: a path whose
        // direction at the very last point has swung well away from the way the
        // line appears to be travelling. Aligned to the final tangent, and hung
        // off the back of the end, the head sits skewed off the curve entirely.
        Case c; c.name = "arrow on hooked end";
        c.stroke.pts = { CP(30, 60, 90, 10), CP(150, 150, 60, 55) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 0.35f, 0.3f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 52.0f; c.style.arrowWidth = 46.0f;
        cases.push_back(c);
    }
    {
        // Same path, tip pinned to the end: what it used to do, kept so the two
        // can be compared side by side.
        Case c; c.name = "hooked end, position 0";
        c.stroke.pts = { CP(30, 60, 90, 10), CP(150, 150, 60, 55) };
        c.style.halfWidth = 9.0f; c.style.feather = 1.0f;
        c.style.color = Color(0.6f, 0.6f, 0.7f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 52.0f; c.style.arrowWidth = 46.0f;
        c.style.arrowPosition = 0.0f;
        cases.push_back(c);
    }
    {
        // A path whose direction changes sharply over the head's own length.
        // At position 1 the head must follow the trajectory AT the end; aimed by
        // a chord reaching a full arrow-length back it points off along where
        // the line used to be going instead.
        Case c; c.name = "steep tail, position 1";
        c.stroke.pts = { CP(30, 190, 100, 0), CP(200, 60, 30, -80) };
        c.style.halfWidth = 8.0f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 0.55f, 0.2f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 54.0f; c.style.arrowWidth = 46.0f;
        cases.push_back(c);
    }
    {
        // The same path centred, so the three placements can be compared.
        Case c; c.name = "steep tail, position .5";
        c.stroke.pts = { CP(30, 190, 100, 0), CP(200, 60, 30, -80) };
        c.style.halfWidth = 8.0f; c.style.feather = 1.0f;
        c.style.color = Color(0.55f, 0.75f, 1.0f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 54.0f; c.style.arrowWidth = 46.0f;
        c.style.arrowPosition = 0.5f;
        cases.push_back(c);
    }
    {
        // Roundness. The silhouette must stay the same SIZE as the sharp
        // version -- if it grows, the vertex inset is not compensating.
        Case c; c.name = "arrow rounded";
        c.stroke.pts = { CP(40, 128), CP(216, 128) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.0f;
        c.style.color = Color(0.8f, 0.85f, 1.0f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 56.0f; c.style.arrowWidth = 44.0f;
        c.style.arrowRound = 9.0f;
        cases.push_back(c);
    }
    {
        // Roundness cranked past what the triangle can absorb. Must degrade to a
        // blunt lozenge rather than folding inside out.
        Case c; c.name = "arrow over-rounded";
        c.stroke.pts = { CP(40, 128), CP(216, 128) };
        c.style.halfWidth = 7.0f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 0.8f, 0.95f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 56.0f; c.style.arrowWidth = 44.0f;
        c.style.arrowRound = 60.0f;
        cases.push_back(c);
    }
    {
        // Arrow NARROWER than the line it terminates. The shaft is pulled back
        // by the arrow length now, so the head should still be visible as a
        // head rather than swallowed whole.
        Case c; c.name = "arrow narrower than line";
        c.stroke.pts = { CP(40, 128), CP(216, 128) };
        c.style.halfWidth = 20.0f; c.style.feather = 1.0f;
        c.style.color = Color(1.0f, 0.7f, 0.7f, 1.0f);
        c.style.arrowEnd = true; c.style.arrowLength = 44.0f; c.style.arrowWidth = 26.0f;
        cases.push_back(c);
    }
    // A circle trimmed to about 300 degrees, butt capped. Most of this run sits
    // BEHIND the infinite half-plane at its own end, so it is the case that
    // proves cap clipping has to stay local to the ends. If the arc comes back
    // as a stump, the locality walk is broken.
    {
        const float k = 0.5523f * 78.0f;
        Case c; c.name = "300deg arc BUTT";
        c.stroke.pts = { CP(128,  50,    k,   0), CP(206, 128,   0,   k),
                         CP(128, 206,   -k,   0), CP(50,  128,   0,  -k) };
        c.stroke.closed = true;
        c.style.halfWidth = 15.0f; c.style.feather = 1.0f; c.style.cap = kCapButt;
        c.style.color = Color(1.0f, 0.8f, 0.5f, 1.0f);
        c.trim.end = 0.83f;
        cases.push_back(c);
    }

    return cases;
}

/** Animation strip: one in/out animation sampled across a clip.
 *
 * The whole point of the in/out system is that it needs no keyframes, so the
 * thing to verify is that a single set of parameters produces a sensible motion
 * across the clip -- which a strip shows at a glance and scrubbing a timeline
 * does not.
 */
std::vector<Case> MakeAnimCases()
{
    std::vector<Case> cases;

    const float clipLen = 60.0f;
    const int   frames  = 16;

    Case proto;
    proto.stroke.pts = { CP(35, 60, 80, 30), CP(128, 200, 60, 0), CP(221, 60, 0, -80) };
    proto.style.halfWidth = 9.0f;
    proto.style.feather   = 1.2f;
    proto.style.color     = Color(1.0f, 0.85f, 0.35f, 1.0f);
    proto.style.arrowEnd  = true;
    proto.style.arrowLength = 26.0f;
    proto.style.arrowWidth  = 22.0f;
    proto.anim       = true;
    proto.clipLength = clipLen;

    // In: draw on from the clip start, easing to a halt.
    proto.stIn.enabled    = true;
    proto.stIn.target     = kTargetTrimEnd;
    proto.stIn.from       = 0.0f;
    proto.stIn.to         = 1.0f;
    proto.stIn.anchor     = kAnchorClipStart;
    proto.stIn.startFrame = 0.0f;
    proto.stIn.duration   = 24.0f;
    proto.stIn.easing     = MakeEasing(10.0f, 70.0f, 0.0f, 0.0f);

    // Out: the tail catches up and swallows the line, anchored to the clip END
    // so it lands correctly whatever the clip length turns out to be.
    proto.stOut.enabled    = true;
    proto.stOut.target     = kTargetTrimStart;
    proto.stOut.from       = 0.0f;
    proto.stOut.to         = 1.0f;
    proto.stOut.anchor     = kAnchorClipEnd;
    proto.stOut.startFrame = -20.0f;
    proto.stOut.duration   = 20.0f;
    proto.stOut.easing     = MakeEasing(60.0f, 10.0f, 0.0f, 0.0f);

    for (int i = 0; i < frames; ++i)
    {
        Case c = proto;
        c.frame = (clipLen - 1.0f) * static_cast<float>(i) / static_cast<float>(frames - 1);

        char buf[64];
        std::snprintf(buf, sizeof(buf), "f%.0f/%.0f", c.frame, clipLen - 1.0f);
        c.name = KeepName(buf);

        cases.push_back(c);
    }

    return cases;
}

/** Offset sweep: a short window flying along a path and off the end.
 *
 * Rendered as a strip because the whole question is whether the window LEAVES.
 * A wrapped closed path circles forever and every frame looks plausible on its
 * own; only the sequence shows that it never ends.
 */
std::vector<Case> MakeSweepCases()
{
    std::vector<Case> cases;
    const int steps = 6;

    const float k = 0.5523f * 80.0f;
    Stroke ring;
    ring.pts = { CP(128,  48,   k,  0), CP(208, 128,  0,  k),
                 CP(128, 208,  -k,  0), CP(48,  128,  0, -k) };
    ring.closed = true;

    Stroke open;
    open.pts = { CP(30, 70, 90, 30), CP(226, 190, 50, -40) };

    // Row 1: closed, wrap ON -- the window circles and never leaves.
    // Row 2: closed, wrap OFF -- it flies through and off, like an open path.
    // Row 3: open, for reference.
    for (int row = 0; row < 3; ++row)
    {
        for (int i = 0; i < steps; ++i)
        {
            Case c;
            c.stroke = (row == 2) ? open : ring;
            c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
            c.style.color = (row == 0) ? Color(1.0f, 0.5f, 0.4f, 1.0f)
                          : (row == 1) ? Color(0.5f, 0.9f, 1.0f, 1.0f)
                                       : Color(1.0f, 0.9f, 0.5f, 1.0f);
            c.style.arrowEnd = true;
            c.style.arrowLength = 28.0f; c.style.arrowWidth = 24.0f;

            c.trim.start  = 0.0f;
            c.trim.end    = 0.22f;
            c.trim.offset = -0.22f + 1.22f * static_cast<float>(i)
                                          / static_cast<float>(steps - 1);
            c.trim.wrap   = (row == 0);

            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s o%+.2f",
                          row == 0 ? "wrap" : (row == 1 ? "nowrap" : "open"),
                          c.trim.offset);
            c.name = KeepName(buf);

            cases.push_back(c);
        }
    }

    return cases;
}

/** Wiggle diagnostics. */
std::vector<Case> MakeWiggleCases()
{
    std::vector<Case> cases;

    auto make = [](const char* name, float amp, float wave, float phase, unsigned seed) {
        Case c; c.name = name;
        c.stroke.pts = { CP(30, 80, 90, 40), CP(226, 176, 60, -40) };
        c.style.halfWidth = 6.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.85f, 0.5f, 1.0f);
        c.wiggleAmp = amp; c.wiggleWave = wave; c.wigglePhase = phase; c.wiggleSeed = seed;
        return c;
    };

    cases.push_back(make("none", 0.0f, 40.0f, 0.0f, 1));
    cases.push_back(make("gentle",  5.0f, 60.0f, 0.0f, 1));
    cases.push_back(make("scribbly", 9.0f, 22.0f, 0.0f, 1));
    cases.push_back(make("seed 2",  9.0f, 22.0f, 0.0f, 2));

    // Phase advances: the wobble must CRAWL along the line, not re-roll into an
    // unrelated shape each step.
    cases.push_back(make("phase 0.0", 8.0f, 40.0f, 0.0f, 3));
    cases.push_back(make("phase 0.3", 8.0f, 40.0f, 0.3f, 3));
    cases.push_back(make("phase 0.6", 8.0f, 40.0f, 0.6f, 3));
    cases.push_back(make("phase 0.9", 8.0f, 40.0f, 0.9f, 3));

    // A dead straight span has two flattened vertices. Wiggle must resample it,
    // or the wobble can only appear where vertices already were and the line
    // merely gets its endpoints nudged.
    {
        Case c = make("STRAIGHT span", 9.0f, 30.0f, 0.0f, 5);
        c.stroke.pts = { CP(30, 128), CP(226, 128) };
        cases.push_back(c);
    }
    // Interaction with the features that consume arc length.
    {
        Case c = make("wiggle + dashes", 8.0f, 30.0f, 0.0f, 6);
        c.dashLen = 16.0f; c.gapLen = 10.0f;
        cases.push_back(c);
    }
    {
        Case c = make("wiggle + taper", 10.0f, 30.0f, 0.0f, 7);
        c.style.halfWidth = 14.0f;
        c.style.widthStart = 0.0f; c.style.widthMid = 1.0f; c.style.widthEnd = 0.0f;
        cases.push_back(c);
    }
    {
        Case c = make("wiggle + arrow", 8.0f, 34.0f, 0.0f, 8);
        c.style.arrowEnd = true;
        c.style.arrowLength = 30.0f; c.style.arrowWidth = 26.0f;
        cases.push_back(c);
    }

    return cases;
}

/** Fill diagnostics for closed paths. */
std::vector<Case> MakeFillCases()
{
    std::vector<Case> cases;

    auto rect = [](float inset) {
        Stroke s;
        s.pts = { CP(inset, inset), CP(256 - inset, inset),
                  CP(256 - inset, 256 - inset), CP(inset, 256 - inset) };
        s.closed = true;
        return s;
    };
    auto circle = [](float r) {
        const float k = 0.5523f * r;
        Stroke s;
        s.pts = { CP(128, 128 - r,  k,  0), CP(128 + r, 128,  0,  k),
                  CP(128, 128 + r, -k,  0), CP(128 - r, 128,  0, -k) };
        s.closed = true;
        return s;
    };

    {   // The highlight box.
        Case c; c.name = "filled rect";
        c.stroke = rect(60);
        c.style.halfWidth = 5.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.85f, 0.3f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.9f, 0.5f, 0.1f, 1.0f);
        c.style.fillOpacity = 0.55f;
        cases.push_back(c);
    }
    {   // Curved contour: the winding must follow the flattened bezier exactly.
        Case c; c.name = "filled circle";
        c.stroke = circle(78);
        c.style.halfWidth = 5.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.4f, 1.0f, 0.8f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.1f, 0.45f, 0.4f, 1.0f);
        cases.push_back(c);
    }
    {   // Fill with NO stroke at all: exercises the path where a pixel has fill
        // coverage but no stroke geometry anywhere in its tile.
        Case c; c.name = "fill only, no stroke";
        c.stroke = circle(78);
        c.style.halfWidth = 0.0f; c.style.feather = 1.2f;
        c.style.color = Color(0.0f, 0.0f, 0.0f, 0.0f);
        c.style.fill = true; c.style.fillColor = Color(0.9f, 0.35f, 0.5f, 1.0f);
        cases.push_back(c);
    }
    {   // Fill must ignore trim: the shape stays whole while the line draws on.
        Case c; c.name = "fill + trim .0-.45";
        c.stroke = rect(60);
        c.style.halfWidth = 6.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.25f, 0.35f, 0.8f, 1.0f);
        c.trim.end = 0.45f;
        cases.push_back(c);
    }
    {   // ...and dashes.
        Case c; c.name = "fill + dashes";
        c.stroke = circle(78);
        c.style.halfWidth = 6.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 0.9f, 0.4f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.35f, 0.2f, 0.5f, 1.0f);
        c.dashLen = 18.0f; c.gapLen = 12.0f;
        cases.push_back(c);
    }
    {   // Self-intersecting bowtie. The nonzero winding rule has to give a
        // definite answer where the contour crosses itself.
        Case c; c.name = "self-intersecting";
        c.stroke.pts = { CP(50, 60), CP(206, 200), CP(206, 60), CP(50, 200) };
        c.stroke.closed = true;
        c.style.halfWidth = 4.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 0.8f);
        c.style.fill = true; c.style.fillColor = Color(0.8f, 0.3f, 0.2f, 1.0f);
        cases.push_back(c);
    }
    {   // Fill under outline and glow: checks the compositing order.
        Case c; c.name = "fill + outline + glow";
        c.stroke = circle(66);
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        c.style.outline = true; c.style.outlineWidth = 3.0f;
        c.style.outlineColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        c.style.glow = true; c.style.glowRadius = 20.0f; c.style.glowIntensity = 0.8f;
        c.style.glowColor = Color(0.3f, 0.7f, 1.0f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.15f, 0.2f, 0.45f, 1.0f);
        cases.push_back(c);
    }
    {   // Fill with a drop shadow: the shadow belongs to the STROKE, so a
        // translucent fill should show the source through it, not the shadow.
        Case c; c.name = "fill + shadow";
        c.stroke = rect(64);
        c.style.halfWidth = 7.0f; c.style.feather = 1.2f;
        c.style.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        c.style.fill = true; c.style.fillColor = Color(0.9f, 0.6f, 0.2f, 1.0f);
        c.style.fillOpacity = 0.5f;
        c.style.shadow = true; c.style.shadowDx = 9.0f; c.style.shadowDy = -9.0f;
        c.style.shadowSoft = 9.0f; c.style.shadowOpacity = 0.8f;
        cases.push_back(c);
    }

    return cases;
}

/** Gradient diagnostics. Fat strokes, because the whole question is what the
 *  ramp does ACROSS the width, and that is invisible on a thin line. */
std::vector<Case> MakeGradCases()
{
    std::vector<Case> cases;

    auto base = [](const char* name, int mode) {
        Case c; c.name = name;
        c.stroke.pts = { CP(40, 90, 70, 80), CP(216, 160, 60, -70) };
        c.style.halfWidth = 26.0f; c.style.feather = 1.2f;
        c.style.color     = Color(1.0f, 0.25f, 0.2f, 1.0f);
        c.style.gradColor = Color(0.15f, 0.45f, 1.0f, 1.0f);
        c.style.gradMode  = mode;
        return c;
    };

    cases.push_back(base("along, full", kGradAlong));
    cases.push_back(base("across, full", kGradAcross));
    cases.push_back(base("centre out", kGradCentre));
    {   // No gradient, for reference.
        Case c = base("none", kGradNone);
        cases.push_back(c);
    }

    {   // Squeezed into a narrow band: the ramp occupies a fifth of the width,
        // flat colour either side. Impossible without start/end.
        Case c = base("across, band .4-.6", kGradAcross);
        c.style.gradStart = 0.4f; c.style.gradEnd = 0.6f;
        cases.push_back(c);
    }
    {   // Reversed by putting End below Start -- no separate invert control.
        Case c = base("across, reversed", kGradAcross);
        c.style.gradStart = 1.0f; c.style.gradEnd = 0.0f;
        cases.push_back(c);
    }
    {   // Ramp running off the end of the stroke, so only part of it is visible.
        Case c = base("across, .0-2.0", kGradAcross);
        c.style.gradStart = 0.0f; c.style.gradEnd = 2.0f;
        cases.push_back(c);
    }
    {   // Midpoint bias pushed hard one way.
        Case c = base("across, mid .15", kGradAcross);
        c.style.gradMid = 0.15f;
        cases.push_back(c);
    }

    {   // Centre-out squeezed to the outer edge only: this is the "outline"
        // look, now as a deliberate choice rather than the only thing available.
        Case c = base("centre out, edge band", kGradCentre);
        c.style.gradStart = 0.75f; c.style.gradEnd = 1.0f;
        cases.push_back(c);
    }
    {   // Along-length band, for a highlight travelling down the line.
        Case c = base("along, band .3-.5", kGradAlong);
        c.style.gradStart = 0.3f; c.style.gradEnd = 0.5f;
        cases.push_back(c);
    }
    {   // Across-width on a TAPERED stroke: u is normalised by the local
        // half-width, so the ramp must follow the taper rather than staying
        // parallel to the centreline.
        Case c = base("across + taper", kGradAcross);
        c.style.widthStart = 1.0f; c.style.widthMid = 0.7f; c.style.widthEnd = 0.15f;
        cases.push_back(c);
    }
    {   // Along-length on a dashed stroke: arc position is continuous across the
        // gaps, so the ramp must span the whole line, not restart per dash.
        Case c = base("along + dashes", kGradAlong);
        c.dashLen = 26.0f; c.gapLen = 14.0f;
        cases.push_back(c);
    }

    // Arrowheads must carry the gradient too. Each of these came out as one flat
    // colour when the triangle had no interior coordinates: the end colour for a
    // length ramp, the midpoint for a width ramp, the base colour for centre-out.
    {
        Case c = base("ARROW along", kGradAlong);
        c.style.halfWidth = 14.0f;
        c.style.arrowEnd = true;
        c.style.arrowLength = 62.0f; c.style.arrowWidth = 54.0f;
        cases.push_back(c);
    }
    {
        Case c = base("ARROW across", kGradAcross);
        c.style.halfWidth = 14.0f;
        c.style.arrowEnd = true;
        c.style.arrowLength = 62.0f; c.style.arrowWidth = 54.0f;
        cases.push_back(c);
    }
    {
        Case c = base("ARROW centre out", kGradCentre);
        c.style.halfWidth = 14.0f;
        c.style.arrowEnd = true;
        c.style.arrowLength = 62.0f; c.style.arrowWidth = 54.0f;
        cases.push_back(c);
    }
    {   // Both ends, so the renormalised domain has to stretch at BOTH extremes.
        Case c = base("ARROW both, along", kGradAlong);
        c.style.halfWidth = 12.0f;
        c.style.arrowStart = true; c.style.arrowEnd = true;
        c.style.arrowLength = 54.0f; c.style.arrowWidth = 46.0f;
        cases.push_back(c);
    }

    return cases;
}

} // namespace

int main(int argc, char** argv)
{
    const char* outPath = (argc > 1) ? argv[1] : "linetest.png";
    int cell = 256;
    const char* sheetName = "main";
    for (int i = 2; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--cell")  == 0) cell      = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--sheet") == 0) sheetName = argv[i + 1];
    }
    if (cell < 64) cell = 64;

    // Assertions, not pictures. Stagger produces an ORDER, and an order is
    // either a permutation or it is broken -- two strokes assigned the same turn
    // would silently animate together, which is invisible in a still.
    if (std::strcmp(sheetName, "check") == 0)
    {
        int failures = 0;
        for (int count = 1; count <= 8; ++count)
        {
            for (int order = 0; order < kOrderCount; ++order)
            {
                std::vector<int> seen(static_cast<size_t>(count), 0);
                for (int i = 0; i < count; ++i)
                {
                    const int k = StaggerIndex(i, count, order, 12345u);
                    if (k < 0 || k >= count)
                    {
                        std::printf("  order %d count %d: index %d out of range (%d)\n",
                                    order, count, i, k);
                        ++failures;
                        continue;
                    }
                    ++seen[static_cast<size_t>(k)];
                }

                // Centre Out is a RANK with deliberate ties -- the two strokes
                // either side of the middle share a turn, which is the point of
                // it -- so only the others must be strict permutations.
                if (order == kOrderCentreOut) continue;

                for (int k = 0; k < count; ++k)
                {
                    if (seen[static_cast<size_t>(k)] != 1)
                    {
                        std::printf("  order %d count %d: turn %d taken %d times\n",
                                    order, count, k, seen[static_cast<size_t>(k)]);
                        ++failures;
                    }
                }
            }
        }
        // The global transform's inverse. The overlay DRAWS handles transformed
        // and writes drags back through this, so an inverse that is even
        // slightly wrong makes points creep away from the cursor a little more
        // on every mouse-move event -- a drift no single frame would reveal.
        {
            const Vec2 pivots[]  = { {0.0f, 0.0f}, {960.0f, 540.0f}, {-120.0f, 77.0f} };
            const Vec2 offsets[] = { {0.0f, 0.0f}, {33.0f, -210.0f} };
            const float rots[]   = { 0.0f, 30.0f, -117.0f, 180.0f, 359.5f };
            const Vec2 scales[]  = { {1.0f, 1.0f}, {2.0f, 0.5f}, {-1.0f, 1.0f}, {0.01f, 3.0f} };
            const Vec2 probes[]  = { {0.0f, 0.0f}, {1920.0f, 1080.0f}, {-500.0f, 250.0f} };

            double worst = 0.0;
            for (const Vec2& pv : pivots)
            for (const Vec2& of : offsets)
            for (float r : rots)
            for (const Vec2& sc : scales)
            {
                const Xform2D x = MakeXform(pv, of, r, sc.x, sc.y);
                for (const Vec2& p : probes)
                {
                    const Vec2 back = x.ApplyInverse(x.Apply(p));
                    worst = std::max<double>(worst, std::fabs(back.x - p.x));
                    worst = std::max<double>(worst, std::fabs(back.y - p.y));

                    const Vec2 vb = x.ApplyVectorInverse(x.ApplyVector(p));
                    worst = std::max<double>(worst, std::fabs(vb.x - p.x));
                    worst = std::max<double>(worst, std::fabs(vb.y - p.y));
                }
            }

            // Generous in absolute terms because the probes are full-frame
            // coordinates, and tight relative to them: 1e-2 of 1920 is five
            // millionths, well under a pixel of drift per drag event.
            const bool ok = (worst < 1e-2);
            std::printf("xform inverse round-trip: worst=%.6f  %s\n",
                        worst, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        }

        std::printf("check: %d failures\n", failures);
        return failures == 0 ? 0 : 1;
    }

    std::vector<Case> cases;
    if      (std::strcmp(sheetName, "caps") == 0) cases = MakeCapCases();
    else if (std::strcmp(sheetName, "grad") == 0) cases = MakeGradCases();
    else if (std::strcmp(sheetName, "fill") == 0) cases = MakeFillCases();
    else if (std::strcmp(sheetName, "anim") == 0) cases = MakeAnimCases();
    else if (std::strcmp(sheetName, "wiggle") == 0) cases = MakeWiggleCases();
    else if (std::strcmp(sheetName, "sweep") == 0)  cases = MakeSweepCases();
    else                                          cases = MakeCases();

    const int cols  = 4;
    const int rows  = (static_cast<int>(cases.size()) + cols - 1) / cols;
    const int sheetW = cols * cell;
    const int sheetH = rows * cell;

    std::vector<unsigned char> sheet(static_cast<size_t>(sheetW) * sheetH * 3, 0);

    // Lay the checkerboard down across the WHOLE sheet first. Filling it only
    // under the cases leaves the trailing cells of the last row black, which
    // reads as "that case rendered nothing" rather than "there is no case here".
    for (int y = 0; y < sheetH; ++y)
        for (int x = 0; x < sheetW; ++x)
        {
            const float chk  = (((x >> 4) + (y >> 4)) & 1) ? 0.16f : 0.10f;
            const unsigned char v =
                static_cast<unsigned char>(::powf(chk, 1.0f / 2.2f) * 255.0f + 0.5f);
            unsigned char* dst = &sheet[((static_cast<size_t>(y) * sheetW) + x) * 3];
            dst[0] = dst[1] = dst[2] = v;
        }

    std::printf("linetest: %zu cases, %dx%d cells\n", cases.size(), cell, cell);

    std::vector<float> rgba;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        // The cases are authored against a 256px cell, so scale their geometry
        // rather than their pixel sizes when the cell changes -- that way a
        // bigger cell is a zoom, and antialiasing can be inspected closely
        // without the line also getting proportionally fatter.
        const float k = static_cast<float>(cell) / 256.0f;
        Case c = cases[i];
        for (ControlPoint& p : c.stroke.pts) { p.pos = p.pos * k; p.tan = p.tan * k; }
        c.style.halfWidth    *= k;
        c.style.outlineWidth *= k;
        c.style.glowRadius   *= k;
        c.dashLen *= k; c.gapLen *= k;

        RenderCase(c, cell, rgba);

        const int ox = static_cast<int>(i % cols) * cell;
        const int oy = static_cast<int>(i / cols) * cell;

        for (int y = 0; y < cell; ++y)
        {
            for (int x = 0; x < cell; ++x)
            {
                const float* px = &rgba[(static_cast<size_t>(y) * cell + x) * 4];

                // Composite over a checkerboard so alpha is visible as alpha
                // rather than silently reading as black.
                const float chk = (((x >> 4) + (y >> 4)) & 1) ? 0.16f : 0.10f;
                unsigned char* dst =
                    &sheet[((static_cast<size_t>(oy + y) * sheetW) + (ox + x)) * 3];

                for (int ch = 0; ch < 3; ++ch)
                {
                    const float lin = px[ch] + chk * (1.0f - px[3]);
                    const float srgb = ::powf(Saturate(lin), 1.0f / 2.2f);
                    dst[ch] = static_cast<unsigned char>(Saturate(srgb) * 255.0f + 0.5f);
                }
            }
        }
    }

    if (!stbi_write_png(outPath, sheetW, sheetH, 3, sheet.data(), sheetW * 3))
    {
        std::fprintf(stderr, "failed to write %s\n", outPath);
        return 1;
    }

    std::printf("wrote %s (%dx%d)\n", outPath, sheetW, sheetH);
    return 0;
}
