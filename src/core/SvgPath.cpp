#include "SvgPath.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// =============================================================================
// A 2x3 affine, for SVG transform attributes and the ancestor stack
// =============================================================================

struct Mat
{
    // | a c e |
    // | b d f |
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Vec2 Apply(Vec2 p) const { return Vec2(a * p.x + c * p.y + e, b * p.x + d * p.y + f); }

    /// Handles are directions: the translation must not apply to them.
    Vec2 ApplyVector(Vec2 v) const { return Vec2(a * v.x + c * v.y, b * v.x + d * v.y); }

    Mat Then(const Mat& m) const   // this, then m
    {
        Mat r;
        r.a = m.a * a + m.c * b;
        r.b = m.b * a + m.d * b;
        r.c = m.a * c + m.c * d;
        r.d = m.b * c + m.d * d;
        r.e = m.a * e + m.c * f + m.e;
        r.f = m.b * e + m.d * f + m.f;
        return r;
    }
};

// =============================================================================
// Scanning helpers
// =============================================================================

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }

void SkipWs(const std::string& s, size_t& i)
{
    while (i < s.size() && IsWs(s[i])) ++i;
}

/** Read one number. SVG allows them run together -- "1-2" is two numbers, and
 *  ".5.5" is two more -- which is why this cannot just call strtod on the rest
 *  of the string and trust it. */
bool ReadNum(const std::string& s, size_t& i, float& out)
{
    SkipWs(s, i);
    if (i >= s.size()) return false;

    const char* start = s.c_str() + i;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if (end == start) return false;

    i += static_cast<size_t>(end - start);
    out = static_cast<float>(v);
    return true;
}

bool ReadFlag(const std::string& s, size_t& i, bool& out)
{
    SkipWs(s, i);
    if (i >= s.size()) return false;
    // Arc flags are single characters and may be packed without separators:
    // "a1 1 0 011 1" is legal and means flags 0 and 1.
    if (s[i] == '0' || s[i] == '1') { out = (s[i] == '1'); ++i; return true; }

    float v = 0.0f;
    if (!ReadNum(s, i, v)) return false;
    out = (v != 0.0f);
    return true;
}

// =============================================================================
// Elliptical arc -> cubic beziers
// =============================================================================

/** Endpoint parameterisation to centre parameterisation, then split into
 *  segments of at most 90 degrees. A single cubic cannot represent a quarter
 *  ellipse to better than about 0.03%, and cannot represent more than a quarter
 *  usefully at all. */
void ArcToBeziers(Vec2 p0, Vec2 p1, float rx, float ry, float xRotDeg,
                  bool largeArc, bool sweep, std::vector<ControlPoint>& out)
{
    if (rx == 0.0f || ry == 0.0f)
    {
        // Zero radius is defined to mean a straight line, not an error.
        ControlPoint cp; cp.pos = p1; cp.tan = Vec2(0, 0);
        out.push_back(cp);
        return;
    }

    rx = ::fabsf(rx);
    ry = ::fabsf(ry);

    const float phi = xRotDeg * kPi / 180.0f;
    const float cosP = ::cosf(phi), sinP = ::sinf(phi);

    const float dx2 = (p0.x - p1.x) * 0.5f;
    const float dy2 = (p0.y - p1.y) * 0.5f;
    const float x1p =  cosP * dx2 + sinP * dy2;
    const float y1p = -sinP * dx2 + cosP * dy2;

    // Radii too small to join the endpoints are scaled up until they can, which
    // the spec requires -- otherwise the square root below goes imaginary.
    float lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0f)
    {
        const float k = ::sqrtf(lam);
        rx *= k; ry *= k;
    }

    const float rx2 = rx * rx, ry2 = ry * ry;
    float num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const float den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    if (num < 0.0f) num = 0.0f;

    float coef = (den > 1e-12f) ? ::sqrtf(num / den) : 0.0f;
    if (largeArc == sweep) coef = -coef;

    const float cxp =  coef * rx * y1p / ry;
    const float cyp = -coef * ry * x1p / rx;

    const Vec2 centre(cosP * cxp - sinP * cyp + (p0.x + p1.x) * 0.5f,
                      sinP * cxp + cosP * cyp + (p0.y + p1.y) * 0.5f);

    auto angle = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float len = ::sqrtf((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float a = (len > 1e-12f) ? ::acosf(Clamp(dot / len, -1.0f, 1.0f)) : 0.0f;
        if (ux * vy - uy * vx < 0.0f) a = -a;
        return a;
    };

    const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;

    const float theta1 = angle(1.0f, 0.0f, ux, uy);
    float sweepAngle   = angle(ux, uy, vx, vy);

    if (!sweep && sweepAngle > 0.0f) sweepAngle -= 2.0f * kPi;
    else if (sweep && sweepAngle < 0.0f) sweepAngle += 2.0f * kPi;

    const int n = static_cast<int>(::ceilf(::fabsf(sweepAngle) / (kPi * 0.5f)));
    const int segs = (n < 1) ? 1 : n;
    const float delta = sweepAngle / static_cast<float>(segs);

    // The magic constant that makes a cubic match a circular arc of angle d.
    const float t = (4.0f / 3.0f) * ::tanf(delta * 0.25f);

    float th = theta1;
    for (int k = 0; k < segs; ++k)
    {
        const float th2 = th + delta;

        const float c1 = ::cosf(th),  s1 = ::sinf(th);
        const float c2 = ::cosf(th2), s2 = ::sinf(th2);

        // Points and derivatives on the unit circle, mapped through the ellipse.
        auto onEllipse = [&](float cs, float sn) {
            return Vec2(centre.x + rx * cs * cosP - ry * sn * sinP,
                        centre.y + rx * cs * sinP + ry * sn * cosP);
        };
        auto deriv = [&](float cs, float sn) {
            return Vec2(-rx * sn * cosP - ry * cs * sinP,
                        -rx * sn * sinP + ry * cs * cosP);
        };

        const Vec2 pStart = onEllipse(c1, s1);
        const Vec2 pEnd   = onEllipse(c2, s2);
        const Vec2 dStart = deriv(c1, s1);
        const Vec2 dEnd   = deriv(c2, s2);

        // The outgoing handle belongs to the point we already emitted; the
        // incoming handle belongs to the one we are about to.
        if (!out.empty()) out.back().tan = dStart * t;

        ControlPoint cp;
        cp.pos = pEnd;
        cp.tan = Vec2(0, 0);
        out.push_back(cp);

        // A cubic's second control point is the endpoint minus its handle, and
        // this format stores one SYMMETRIC handle per point -- so an arc's
        // incoming handle is recorded as the outgoing one of the same length,
        // which is exact here because an arc segment is symmetric about its
        // midpoint.
        out.back().tan = dEnd * t;

        th = th2;
        (void)pStart;
    }
}

// =============================================================================
// Path data
// =============================================================================

/** Convert an explicit cubic control point pair into this format's single
 *  symmetric handle per point.
 *
 *  This is where SVG and this plugin genuinely disagree. SVG gives each span two
 *  independent control points; this plugin stores one handle per POINT and
 *  mirrors it. A span whose two controls are not mirror images cannot be
 *  represented exactly, so the outgoing handle is taken from the span's first
 *  control and the incoming from its second -- which is exact whenever the curve
 *  is smooth, and the closest available answer when it is not. */
void EmitCubic(std::vector<ControlPoint>& pts, Vec2 c1, Vec2 c2, Vec2 end)
{
    if (!pts.empty()) pts.back().tan = c1 - pts.back().pos;

    ControlPoint cp;
    cp.pos = end;
    cp.tan = end - c2;   // mirrored: the stored handle is the OUTGOING one
    pts.push_back(cp);
}

} // namespace

bool ParsePathData(const std::string& d, std::vector<Stroke>& out,
                   bool& sawArc, int maxPoints)
{
    size_t i = 0;
    char   cmd = 0;

    Vec2 cur(0, 0), start(0, 0);
    Vec2 lastCubicCtrl(0, 0), lastQuadCtrl(0, 0);
    bool haveCubic = false, haveQuad = false;

    std::vector<ControlPoint> pts;
    int total = 0;

    auto flush = [&](bool closed) {
        if (pts.size() >= 2)
        {
            Stroke s;
            s.pts    = pts;
            s.closed = closed;
            out.push_back(s);
        }
        pts.clear();
    };

    while (i < d.size())
    {
        SkipWs(d, i);
        if (i >= d.size()) break;

        const char ch = d[i];
        if (std::isalpha(static_cast<unsigned char>(ch)))
        {
            cmd = ch;
            ++i;
        }
        else if (cmd == 0)
        {
            return false;   // data before any command
        }
        else if (cmd == 'M') cmd = 'L';    // repeated moveto pairs are linetos
        else if (cmd == 'm') cmd = 'l';

        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const char c   = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

        if (total > maxPoints) { flush(false); return true; }

        auto rp = [&](Vec2 p) { return rel ? Vec2(cur.x + p.x, cur.y + p.y) : p; };

        if (c == 'Z')
        {
            flush(true);
            cur = start;
            haveCubic = haveQuad = false;
            continue;
        }

        float x = 0, y = 0;

        if (c == 'M')
        {
            if (!ReadNum(d, i, x) || !ReadNum(d, i, y)) break;
            flush(false);
            cur = rp(Vec2(x, y));
            start = cur;

            ControlPoint cp; cp.pos = cur; cp.tan = Vec2(0, 0);
            pts.push_back(cp);
            ++total;
            haveCubic = haveQuad = false;
        }
        else if (c == 'L' || c == 'H' || c == 'V')
        {
            Vec2 p = cur;
            if (c == 'L')      { if (!ReadNum(d, i, x) || !ReadNum(d, i, y)) break; p = rp(Vec2(x, y)); }
            else if (c == 'H') { if (!ReadNum(d, i, x)) break; p = Vec2(rel ? cur.x + x : x, cur.y); }
            else               { if (!ReadNum(d, i, y)) break; p = Vec2(cur.x, rel ? cur.y + y : y); }

            ControlPoint cp; cp.pos = p; cp.tan = Vec2(0, 0);
            pts.push_back(cp);
            ++total;
            cur = p;
            haveCubic = haveQuad = false;
        }
        else if (c == 'C' || c == 'S')
        {
            Vec2 c1, c2, end;
            if (c == 'C')
            {
                float x1, y1, x2, y2;
                if (!ReadNum(d, i, x1) || !ReadNum(d, i, y1) ||
                    !ReadNum(d, i, x2) || !ReadNum(d, i, y2) ||
                    !ReadNum(d, i, x)  || !ReadNum(d, i, y)) break;
                c1 = rp(Vec2(x1, y1)); c2 = rp(Vec2(x2, y2)); end = rp(Vec2(x, y));
            }
            else
            {
                float x2, y2;
                if (!ReadNum(d, i, x2) || !ReadNum(d, i, y2) ||
                    !ReadNum(d, i, x) || !ReadNum(d, i, y)) break;
                // S reflects the previous curve's second control through the
                // current point; with no previous curve it IS the current point.
                c1 = haveCubic ? Vec2(2.0f * cur.x - lastCubicCtrl.x,
                                      2.0f * cur.y - lastCubicCtrl.y) : cur;
                c2 = rp(Vec2(x2, y2)); end = rp(Vec2(x, y));
            }

            EmitCubic(pts, c1, c2, end);
            ++total;
            lastCubicCtrl = c2;
            cur = end;
            haveCubic = true; haveQuad = false;
        }
        else if (c == 'Q' || c == 'T')
        {
            Vec2 q, end;
            if (c == 'Q')
            {
                float x1, y1;
                if (!ReadNum(d, i, x1) || !ReadNum(d, i, y1) ||
                    !ReadNum(d, i, x) || !ReadNum(d, i, y)) break;
                q = rp(Vec2(x1, y1)); end = rp(Vec2(x, y));
            }
            else
            {
                if (!ReadNum(d, i, x) || !ReadNum(d, i, y)) break;
                q = haveQuad ? Vec2(2.0f * cur.x - lastQuadCtrl.x,
                                    2.0f * cur.y - lastQuadCtrl.y) : cur;
                end = rp(Vec2(x, y));
            }

            // A quadratic is EXACTLY a cubic: the two cubic controls sit two
            // thirds of the way from each endpoint to the quadratic control.
            const Vec2 c1(cur.x + (q.x - cur.x) * (2.0f / 3.0f),
                          cur.y + (q.y - cur.y) * (2.0f / 3.0f));
            const Vec2 c2(end.x + (q.x - end.x) * (2.0f / 3.0f),
                          end.y + (q.y - end.y) * (2.0f / 3.0f));

            EmitCubic(pts, c1, c2, end);
            ++total;
            lastQuadCtrl = q;
            cur = end;
            haveQuad = true; haveCubic = false;
        }
        else if (c == 'A')
        {
            float rx, ry, rot;
            bool large = false, sweep = false;
            if (!ReadNum(d, i, rx) || !ReadNum(d, i, ry) || !ReadNum(d, i, rot) ||
                !ReadFlag(d, i, large) || !ReadFlag(d, i, sweep) ||
                !ReadNum(d, i, x) || !ReadNum(d, i, y)) break;

            const Vec2 end = rp(Vec2(x, y));
            const size_t before = pts.size();
            ArcToBeziers(cur, end, rx, ry, rot, large, sweep, pts);
            total += static_cast<int>(pts.size() - before);

            sawArc = true;
            cur = end;
            haveCubic = haveQuad = false;
        }
        else
        {
            break;   // an unknown command; stop rather than guess
        }
    }

    flush(false);
    return !out.empty();
}

// =============================================================================
// Document
// =============================================================================

namespace {

std::string AttrOf(const std::string& tag, const char* name)
{
    const std::string key = std::string(name) + "=";
    size_t p = 0;
    while ((p = tag.find(key, p)) != std::string::npos)
    {
        // Must be preceded by whitespace, or "x=" matches inside "rx=".
        if (p == 0 || IsWs(tag[p - 1]) || tag[p - 1] == '<') break;
        p += key.size();
    }
    if (p == std::string::npos) return std::string();

    p += key.size();
    if (p >= tag.size()) return std::string();

    const char quote = tag[p];
    if (quote != '"' && quote != '\'') return std::string();

    const size_t end = tag.find(quote, p + 1);
    if (end == std::string::npos) return std::string();

    return tag.substr(p + 1, end - p - 1);
}

Mat ParseTransform(const std::string& spec)
{
    Mat m;
    size_t i = 0;

    while (i < spec.size())
    {
        while (i < spec.size() && !std::isalpha(static_cast<unsigned char>(spec[i]))) ++i;
        if (i >= spec.size()) break;

        const size_t nameStart = i;
        while (i < spec.size() && std::isalpha(static_cast<unsigned char>(spec[i]))) ++i;
        const std::string name = spec.substr(nameStart, i - nameStart);

        const size_t open = spec.find('(', i);
        if (open == std::string::npos) break;
        const size_t close = spec.find(')', open);
        if (close == std::string::npos) break;

        const std::string args = spec.substr(open + 1, close - open - 1);
        size_t j = 0;
        float v[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        while (n < 6 && ReadNum(args, j, v[n])) ++n;

        Mat t;
        if (name == "matrix" && n >= 6)
        { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (name == "translate" && n >= 1)
        { t.e = v[0]; t.f = (n >= 2) ? v[1] : 0.0f; }
        else if (name == "scale" && n >= 1)
        { t.a = v[0]; t.d = (n >= 2) ? v[1] : v[0]; }
        else if (name == "rotate" && n >= 1)
        {
            const float r = v[0] * kPi / 180.0f;
            t.a = ::cosf(r); t.b = ::sinf(r); t.c = -::sinf(r); t.d = ::cosf(r);
            if (n >= 3)
            {
                // rotate(a cx cy) is translate(c) rotate(a) translate(-c).
                Mat pre;  pre.e  = -v[1]; pre.f  = -v[2];
                Mat post; post.e =  v[1]; post.f =  v[2];
                t = pre.Then(t).Then(post);
            }
        }
        else if (name == "skewX" && n >= 1) { t.c = ::tanf(v[0] * kPi / 180.0f); }
        else if (name == "skewY" && n >= 1) { t.b = ::tanf(v[0] * kPi / 180.0f); }

        // SVG transform lists apply left to right, outermost first.
        m = t.Then(m);
        i = close + 1;
    }
    return m;
}

/** Basic shapes, expressed as path data so there is one geometry path. */
std::string ShapeToPathData(const std::string& tag, const std::string& name)
{
    auto num = [&](const char* a, float def = 0.0f) {
        const std::string v = AttrOf(tag, a);
        if (v.empty()) return def;
        size_t j = 0; float f = def; ReadNum(v, j, f); return f;
    };
    char buf[512];

    if (name == "rect")
    {
        const float x = num("x"), y = num("y"), w = num("width"), h = num("height");
        if (w <= 0.0f || h <= 0.0f) return std::string();
        std::snprintf(buf, sizeof(buf), "M%g,%g H%g V%g H%g Z",
                      x, y, x + w, y + h, x);
        return buf;
    }
    if (name == "circle" || name == "ellipse")
    {
        const float cx = num("cx"), cy = num("cy");
        const float rx = (name == "circle") ? num("r") : num("rx");
        const float ry = (name == "circle") ? num("r") : num("ry");
        if (rx <= 0.0f || ry <= 0.0f) return std::string();
        // Two half-arcs: one arc command cannot describe a full ellipse, because
        // its start and end points would coincide and the sweep is ambiguous.
        std::snprintf(buf, sizeof(buf), "M%g,%g A%g,%g 0 1 0 %g,%g A%g,%g 0 1 0 %g,%g Z",
                      cx - rx, cy, rx, ry, cx + rx, cy, rx, ry, cx - rx, cy);
        return buf;
    }
    if (name == "line")
    {
        std::snprintf(buf, sizeof(buf), "M%g,%g L%g,%g",
                      num("x1"), num("y1"), num("x2"), num("y2"));
        return buf;
    }
    if (name == "polygon" || name == "polyline")
    {
        const std::string pts = AttrOf(tag, "points");
        if (pts.empty()) return std::string();
        return "M" + pts + (name == "polygon" ? " Z" : "");
    }
    return std::string();
}

} // namespace

SvgImportResult ParseSvg(const std::string& svg, int maxPoints)
{
    SvgImportResult r;

    // --- document extents ----------------------------------------------------
    float vbx = 0, vby = 0, vbw = 0, vbh = 0;
    {
        const size_t root = svg.find("<svg");
        if (root == std::string::npos) { r.error = "no <svg> element found"; return r; }

        const size_t close = svg.find('>', root);
        const std::string tag = svg.substr(root, (close == std::string::npos ? svg.size() : close) - root);

        const std::string vb = AttrOf(tag, "viewBox");
        if (!vb.empty())
        {
            size_t j = 0;
            ReadNum(vb, j, vbx); ReadNum(vb, j, vby);
            ReadNum(vb, j, vbw); ReadNum(vb, j, vbh);
        }
        if (vbw <= 0.0f || vbh <= 0.0f)
        {
            size_t j = 0; float w = 0, h = 0;
            std::string ws = AttrOf(tag, "width"), hs = AttrOf(tag, "height");
            if (!ws.empty()) ReadNum(ws, j, w);
            j = 0;
            if (!hs.empty()) ReadNum(hs, j, h);
            vbx = vby = 0; vbw = w; vbh = h;
        }
    }

    // --- walk the document, maintaining a transform stack ---------------------
    std::vector<Mat> stack;
    stack.push_back(Mat());

    /** Elements whose contents are DEFINITIONS, not drawn content.
     *
     * Skipping these is not a refinement, it is required. A clip path, a mask or
     * a marker is described with exactly the same <path> element as real
     * artwork, so a scanner that takes every path in the file imports the
     * clipping rectangle as a shape -- which is how an Inkscape export came in
     * with a large square across the whole logo. */
    auto isDefinition = [](const std::string& n) {
        return n == "defs" || n == "clipPath" || n == "mask" || n == "symbol" ||
               n == "pattern" || n == "marker";
    };
    int defsDepth = 0;

    std::vector<Stroke> user;   // in SVG user units
    bool sawArc = false;

    size_t i = 0;
    while (i < svg.size())
    {
        const size_t lt = svg.find('<', i);
        if (lt == std::string::npos) break;
        const size_t gt = svg.find('>', lt);
        if (gt == std::string::npos) break;

        const std::string tag = svg.substr(lt, gt - lt + 1);
        i = gt + 1;

        if (tag.size() < 2) continue;
        if (tag.compare(0, 4, "<!--") == 0) continue;

        if (tag[1] == '/')
        {
            size_t k = 2;
            while (k < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[k])) || tag[k] == '-')) ++k;
            const std::string closing = tag.substr(2, k - 2);

            if (closing == "g" && stack.size() > 1) stack.pop_back();
            if (isDefinition(closing) && defsDepth > 0) --defsDepth;
            continue;
        }

        size_t n = 1;
        while (n < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[n])) || tag[n] == '-')) ++n;
        const std::string name = tag.substr(1, n - 1);

        const bool selfClosing = (tag.size() >= 2 && tag[tag.size() - 2] == '/');

        if (isDefinition(name))
        {
            if (!selfClosing) ++defsDepth;
            continue;
        }

        Mat local;
        const std::string xf = AttrOf(tag, "transform");
        if (!xf.empty()) local = ParseTransform(xf);

        if (name == "g")
        {
            // A self-closing <g/> opens nothing, so pushing for it would leave
            // the stack permanently deeper and shift everything after it.
            if (!selfClosing) stack.push_back(local.Then(stack.back()));
            continue;
        }

        // Inside a definition, transforms still nest but nothing is drawn.
        if (defsDepth > 0) continue;

        std::string d;
        if (name == "path") d = AttrOf(tag, "d");
        else                d = ShapeToPathData(tag, name);
        if (d.empty()) continue;

        std::vector<Stroke> got;
        if (!ParsePathData(d, got, sawArc, maxPoints - r.points)) continue;

        const Mat m = local.Then(stack.back());
        for (Stroke& s : got)
        {
            for (ControlPoint& p : s.pts)
            {
                p.pos = m.Apply(p.pos);
                p.tan = m.ApplyVector(p.tan);
            }
            r.points += static_cast<int>(s.pts.size());
            user.push_back(s);
        }

        if (r.points >= maxPoints) { r.truncated = true; break; }
    }

    if (user.empty()) { r.error = "no path geometry found"; return r; }

    // --- extents, if the document did not declare usable ones -----------------
    if (vbw <= 0.0f || vbh <= 0.0f)
    {
        Bounds bb;
        for (const Stroke& s : user)
            for (const ControlPoint& p : s.pts) bb.Add(p.pos);
        if (bb.Empty()) { r.error = "geometry has no extent"; return r; }
        vbx = bb.x0; vby = bb.y0;
        vbw = Max(bb.x1 - bb.x0, 1e-6f);
        vbh = Max(bb.y1 - bb.y0, 1e-6f);
    }

    // --- normalise ------------------------------------------------------------
    // Fitted to the SHORTER relative axis so nothing is cropped, scaled equally
    // on both axes so the artwork keeps its proportions, and centred. The y flip
    // happens here, once: SVG's y runs down and this plugin's runs up.
    const float scale = 1.0f / Max(vbw, vbh);
    const float cx = vbx + vbw * 0.5f;
    const float cy = vby + vbh * 0.5f;

    for (Stroke& s : user)
    {
        for (ControlPoint& p : s.pts)
        {
            p.pos = Vec2(0.5f + (p.pos.x - cx) * scale,
                         0.5f - (p.pos.y - cy) * scale);
            p.tan = Vec2(p.tan.x * scale, -p.tan.y * scale);
        }
        r.strokes.push_back(s);
    }

    r.subpaths = static_cast<int>(r.strokes.size());
    r.sawArc   = sawArc;
    return r;
}

} // namespace bl
