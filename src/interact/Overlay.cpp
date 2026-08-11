#include "interact/Overlay.h"
#include "interact/DrawUtils.h"

#include "EditBlock.h"
#include "HostProbe.h"
#include "ParamNames.h"
#include "core/Easing.h"
#include "core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The viewer overlay is the only surface where this can live. Resolve's OFX host
// supports neither parametric (curve) parameters nor custom parameter interacts,
// so there is no way to put a real editor in the Inspector -- the panel can only
// ever show a list of numbers.
//
// Every entry point here is wrapped in try/catch and returns rather than throws.
// An exception escaping into the host mid-draw is far worse than a frame without
// an overlay.

namespace bl {

namespace {

struct PointRef
{
    int stroke = -1;
    int index  = -1;
    bool valid() const { return stroke >= 0 && index >= 0; }
    bool operator==(const PointRef& o) const
    {
        return stroke == o.stroke && index == o.index;
    }
};

enum DragKind
{
    kDragNone,
    kDragPoint,
    kDragCurve1,   ///< the easing curve's first bezier handle
    kDragCurve2,
    kDragTanOut,
    kDragTanIn,
    /// Ctrl-drag started ON the point rather than on a handle tip. Behaves like
    /// kDragTanOut while dragging, but is distinguished so that a Ctrl press
    /// which never moves can collapse the handle back to a corner. Clicking a
    /// handle TIP is not the same gesture and must not collapse anything.
    kDragTanPull
};

class BetterLineInteract : public OFX::OverlayInteract
{
public:
    BetterLineInteract(OfxInteractHandle p_Handle, OFX::ImageEffect* p_Effect);

    bool draw(const OFX::DrawArgs& args) override;
    bool penDown(const OFX::PenArgs& args) override;
    bool penMotion(const OFX::PenArgs& args) override;
    bool penUp(const OFX::PenArgs& args) override;
    bool keyDown(const OFX::KeyArgs& args) override;
    bool keyUp(const OFX::KeyArgs& args) override;
    void loseFocus(const OFX::FocusArgs& args) override;

private:
    bool buildContext(double time, const OfxPointD& pixelScale,
                      OfxDrawContextHandle ctx, OverlayContext& out) const;

    int  strokeCount(double t) const;
    int  pointCount(int s, double t) const;
    bool strokeEnabled(int s, double t) const;
    bool strokeClosed(int s, double t) const;

    OfxPointD posAt(const OverlayContext& c, int s, int i) const;
    OfxPointD tanTip(const OverlayContext& c, int s, int i, bool outward) const;

    void buildStroke(const OverlayContext& c, int s, Stroke& out) const;

    void writePoint(OFX::Double2DParam* p, double t, double x, double y) const;
    void insertPoint(int s, int at, double nx, double ny, double t);
    void deletePoint(int s, int at);
    void abandonDrag();

    /// Fetched on first use; see the note in the constructor.
    OFX::Double2DParam* pos(int s, int i) const
    {
        if (!_pos[s][i])
        {
            try { _pos[s][i] = _effect->fetchDouble2DParam(PointParam(kPointPos, s, i)); }
            catch (...) { return nullptr; }
        }
        return _pos[s][i];
    }
    OFX::Double2DParam* tan(int s, int i) const
    {
        if (!_tan[s][i])
        {
            try { _tan[s][i] = _effect->fetchDouble2DParam(PointParam(kPointTan, s, i)); }
            catch (...) { return nullptr; }
        }
        return _tan[s][i];
    }

    /// Fetched once. fetchClip is a host round-trip, and buildContext runs on
    /// every draw AND every pen-motion event -- so doing it there put two of
    /// them in the path of every mouse move.
    OFX::Clip* _srcClip = nullptr;
    OFX::Clip* _dstClip = nullptr;

    /// Reused across draws. These were being reallocated every redraw, and the
    /// overlay redraws continuously while the playhead is over the clip.
    mutable Polyline               _poly;
    mutable std::vector<OfxPointD> _screen;

    OFX::IntParam*      _strokeCount = nullptr;
    OFX::BooleanParam*  _sEnable[kMaxStrokes] = {};
    OFX::IntParam*      _sCount[kMaxStrokes]  = {};
    OFX::BooleanParam*  _sClosed[kMaxStrokes] = {};
    mutable OFX::Double2DParam* _pos[kMaxStrokes][kMaxPoints] = {};
    mutable OFX::Double2DParam* _tan[kMaxStrokes][kMaxPoints] = {};

    // --- easing curve editor -------------------------------------------------
    OFX::Double2DParam* _xfPos    = nullptr;
    OFX::DoubleParam*   _xfRot    = nullptr;
    OFX::Double2DParam* _xfScale  = nullptr;
    OFX::Double2DParam* _xfAnchor = nullptr;

    OFX::BooleanParam* _showCurve  = nullptr;
    OFX::ChoiceParam*  _curveStage = nullptr;
    OFX::DoubleParam*  _easeIn[2]  = {};
    OFX::DoubleParam*  _easeOut[2] = {};
    OFX::DoubleParam*  _antic[2]   = {};
    OFX::DoubleParam*  _over[2]    = {};

    bool      curveVisible(double t) const;
    int       curveStage(double t) const;
    Easing    curveAt(double t, int stage) const;
    OfxRectD  curveRect(const OverlayContext& c) const;
    OfxRectD  curvePlotRect(const OverlayContext& c) const;
    OfxRectD  curveTabRect(const OverlayContext& c, int which) const;
    OfxPointD curveToCanonical(const OverlayContext& c, float tx, float vy) const;
    void      drawCurve(const OverlayContext& c);
    void      writeCurveHandle(double t, int stage, bool second, float hx, float hy);

    /// Plotted y range. LATCHED, not refitted every draw.
    ///
    /// Refitting continuously turns a drag into a feedback loop: pulling a
    /// handle up grows the range, growing the range rescales the plot, and the
    /// rescale drags the handle back down under the cursor. The graph appears to
    /// fight the edit. Between drags it is free to move in both directions -- a
    /// range that could only grow would stay zoomed out after one wild drag.
    mutable float _curveLo = -0.25f;
    mutable float _curveHi =  1.25f;

    DragKind  _drag = kDragNone;
    PointRef  _dragRef;
    PointRef  _selected;
    OfxPointD _grab{0.0, 0.0};   ///< canonical offset from pen to the handle

    /// Where the pen went down, and whether it has since travelled far enough to
    /// count as a drag. A Ctrl press that never moves is a click, and means the
    /// opposite of a Ctrl press that does.
    OfxPointD _pressPos{0.0, 0.0};
    bool      _dragMoved = false;

    bool _altDown   = false;
    bool _shiftDown = false;
    bool _ctrlDown  = false;
};

BetterLineInteract::BetterLineInteract(OfxInteractHandle p_Handle,
                                       OFX::ImageEffect* p_Effect)
    : OFX::OverlayInteract(p_Handle)
{
    (void)p_Effect;   // the base class has already stored it as _effect

    try
    {
        _srcClip = _effect->fetchClip(kOfxImageEffectSimpleSourceClipName);
        _dstClip = _effect->fetchClip(kOfxImageEffectOutputClipName);

        _strokeCount = _effect->fetchIntParam(kParamStrokeCount);
        for (int s = 0; s < kMaxStrokes; ++s)
        {
            _sEnable[s] = _effect->fetchBooleanParam(StrokeParam(kStrokeEnable, s));
            _sCount[s]  = _effect->fetchIntParam(StrokeParam(kStrokeCount, s));
            _sClosed[s] = _effect->fetchBooleanParam(StrokeParam(kStrokeClosed, s));
            // Point parameters are fetched lazily -- see pos()/tan(). The
            // interact is rebuilt whenever the viewer wants one, and fetching
            // 256 parameters to draw a two-point line is most of that cost.
        }
        _xfPos    = _effect->fetchDouble2DParam(kParamXfPos);
        _xfRot    = _effect->fetchDoubleParam(kParamXfRot);
        _xfScale  = _effect->fetchDouble2DParam(kParamXfScale);
        _xfAnchor = _effect->fetchDouble2DParam(kParamXfAnchor);

        _showCurve  = _effect->fetchBooleanParam(kParamShowCurve);
        _curveStage = _effect->fetchChoiceParam(kParamCurveStage);
        for (int i = 0; i < 2; ++i)
        {
            const bool out = (i == 1);
            _easeIn[i]  = _effect->fetchDoubleParam(StageParam(kStageEaseIn, out));
            _easeOut[i] = _effect->fetchDoubleParam(StageParam(kStageEaseOut, out));
            _antic[i]   = _effect->fetchDoubleParam(StageParam(kStageAntic, out));
            _over[i]    = _effect->fetchDoubleParam(StageParam(kStageOver, out));
        }
    }
    catch (...) { return; }

    // Slaved to the STRUCTURAL parameters only, not to all 256 point slots.
    //
    // Slaving makes the host redraw the overlay when a value changes. Doing it
    // for every point would be the thorough answer, but it registers a couple of
    // hundred watchers on a host whose behaviour at that count is unmeasured,
    // and slaving is an optimisation rather than a requirement -- a drag already
    // calls requestRedraw itself. Revisit once probe.log says what this host is
    // comfortable with.
    try
    {
        addParamToSlaveTo(_strokeCount);
        for (int s = 0; s < kMaxStrokes; ++s)
        {
            addParamToSlaveTo(_sEnable[s]);
            addParamToSlaveTo(_sCount[s]);
            addParamToSlaveTo(_sClosed[s]);
        }

        // The curve editor's own controls, so typing a value in the Inspector
        // moves the plotted curve immediately rather than on the next refresh.
        // Twelve watchers, not two hundred -- worth it here.
        addParamToSlaveTo(_showCurve);
        addParamToSlaveTo(_curveStage);
        for (int i = 0; i < 2; ++i)
        {
            addParamToSlaveTo(_easeIn[i]);
            addParamToSlaveTo(_easeOut[i]);
            addParamToSlaveTo(_antic[i]);
            addParamToSlaveTo(_over[i]);
        }
    }
    catch (...) {}
}

// --- reads -------------------------------------------------------------------

int BetterLineInteract::strokeCount(double t) const
{
    int n = 1;
    if (_strokeCount) _strokeCount->getValueAtTime(t, n);
    return n < 1 ? 1 : (n > kMaxStrokes ? kMaxStrokes : n);
}

int BetterLineInteract::pointCount(int s, double t) const
{
    int n = 2;
    if (_sCount[s]) _sCount[s]->getValueAtTime(t, n);
    return n < 2 ? 2 : (n > kMaxPoints ? kMaxPoints : n);
}

bool BetterLineInteract::strokeEnabled(int s, double t) const
{
    bool v = false;
    if (_sEnable[s]) _sEnable[s]->getValueAtTime(t, v);
    return v;
}

bool BetterLineInteract::strokeClosed(int s, double t) const
{
    bool v = false;
    if (_sClosed[s]) _sClosed[s]->getValueAtTime(t, v);
    return v;
}

OfxPointD BetterLineInteract::posAt(const OverlayContext& c, int s, int i) const
{
    double x = 0.5, y = 0.5;
    // getValueAtTime, not getValue: the point parameters are genuinely
    // keyframable, so the handle has to sit where the animation puts it at this
    // frame rather than on some static value the user last typed.
    if (OFX::Double2DParam* p = pos(s, i)) p->getValueAtTime(c.time, x, y);

    // Through the global transform, so the handle sits on the line the user can
    // actually see. Drawing the untransformed position would put every handle
    // somewhere the stroke is not the moment the transform is touched.
    const OfxPointD q = c.toCanonical(x, y);
    const Vec2 t = c.xform.Apply(Vec2(static_cast<float>(q.x), static_cast<float>(q.y)));
    return { t.x, t.y };
}

OfxPointD BetterLineInteract::tanTip(const OverlayContext& c, int s, int i,
                                     bool outward) const
{
    double tx = 0.0, ty = 0.0;
    if (OFX::Double2DParam* q = tan(s, i)) q->getValueAtTime(c.time, tx, ty);

    const OfxPointD p = posAt(c, s, i);   // already transformed
    const OfxPointD d0 = c.offsetToCanonical(tx, ty);
    const Vec2 d = c.xform.ApplyVector(Vec2(static_cast<float>(d0.x),
                                            static_cast<float>(d0.y)));
    return outward ? OfxPointD{p.x + d.x, p.y + d.y}
                   : OfxPointD{p.x - d.x, p.y - d.y};
}

void BetterLineInteract::buildStroke(const OverlayContext& c, int s, Stroke& out) const
{
    const int n = pointCount(s, c.time);
    out.pts.clear();
    out.pts.reserve(static_cast<size_t>(n));
    out.closed = strokeClosed(s, c.time);

    for (int i = 0; i < n; ++i)
    {
        double px = 0, py = 0, tx = 0, ty = 0;
        if (OFX::Double2DParam* p = pos(s, i)) p->getValueAtTime(c.time, px, py);
        if (OFX::Double2DParam* q = tan(s, i)) q->getValueAtTime(c.time, tx, ty);

        const OfxPointD p = c.toCanonical(px, py);
        const OfxPointD d = c.offsetToCanonical(tx, ty);

        ControlPoint cp;
        cp.pos = c.xform.Apply(Vec2(static_cast<float>(p.x), static_cast<float>(p.y)));
        cp.tan = c.xform.ApplyVector(Vec2(static_cast<float>(d.x), static_cast<float>(d.y)));
        out.pts.push_back(cp);
    }
}

bool BetterLineInteract::buildContext(double time, const OfxPointD& pixelScale,
                                      OfxDrawContextHandle ctx,
                                      OverlayContext& out) const
{
    try
    {
        out.ctx        = ctx;
        out.time       = time;
        out.pixelScale = pixelScale;

        // A degenerate pixel scale collapses every screen-relative size to zero
        // and the overlay silently vanishes -- no error, just nothing drawn.
        if (!(out.pixelScale.x > 0.0)) out.pixelScale.x = 1.0;
        if (!(out.pixelScale.y > 0.0)) out.pixelScale.y = 1.0;

        OFX::Clip* clip = (_srcClip && _srcClip->isConnected()) ? _srcClip : _dstClip;
        if (!clip) return false;

        out.rod = clip->getRegionOfDefinition(time);
        const double w = out.rod.x2 - out.rod.x1;
        const double h = out.rod.y2 - out.rod.y1;
        if (w <= 0.0 || h <= 0.0) return false;

        // Same transform the renderer builds, in canonical rather than pixel
        // units -- which are the same shape, so rotation behaves identically.
        double ax = 0.5, ay = 0.5, ox = 0.0, oy = 0.0, sx = 1.0, sy = 1.0, rot = 0.0;
        if (_xfAnchor) _xfAnchor->getValueAtTime(time, ax, ay);
        if (_xfPos)    _xfPos->getValueAtTime(time, ox, oy);
        if (_xfScale)  _xfScale->getValueAtTime(time, sx, sy);
        if (_xfRot)    _xfRot->getValueAtTime(time, rot);

        out.xform = MakeXform(Vec2(static_cast<float>(out.rod.x1 + ax * w),
                                   static_cast<float>(out.rod.y1 + ay * h)),
                              Vec2(static_cast<float>(ox * w), static_cast<float>(oy * h)),
                              static_cast<float>(rot),
                              static_cast<float>(sx), static_cast<float>(sy));
    }
    catch (...) { return false; }

    return true;
}

// --- writes ------------------------------------------------------------------

/** @brief Write a point, respecting whether it is animated.
 *
 * This is the rule the whole keyframable-slot design turns on. Calling
 * setValueAtTime unconditionally would set a key on the first drag of every
 * point, silently converting static geometry into animated geometry -- and the
 * user would only find out later, when the line drifted on a frame they never
 * touched. Calling setValue unconditionally is the mirror mistake: on an already
 * animated point it edits the underlying static value, which the animation then
 * overrides, so the drag appears to do nothing at all.
 */
void BetterLineInteract::writePoint(OFX::Double2DParam* p, double t,
                                    double x, double y) const
{
    if (!p) return;
    try
    {
        if (p->getNumKeys() > 0) p->setValueAtTime(t, x, y);
        else                     p->setValue(x, y);
    }
    catch (...) {}
}

void BetterLineInteract::insertPoint(int s, int at, double nx, double ny, double t)
{
    const int n = pointCount(s, t);
    if (n >= kMaxPoints || at < 0 || at > n) return;

    try
    {
        EditBlock eb(_effect, "Insert Point");

        // copyFrom, not a read-then-write of the current value: these slots can
        // carry whole animation curves, and shifting only today's value would
        // quietly discard every keyframe on every point after the insertion.
        for (int i = n; i > at; --i)
        {
            pos(s, i)->copyFrom(*pos(s, i - 1), 0, nullptr);
            tan(s, i)->copyFrom(*tan(s, i - 1), 0, nullptr);
        }

        // The slot being inserted into still holds what it held before the
        // shift, keyframes and all. Clearing is what makes the new point new;
        // without it the fresh point inherits the animation of its neighbour.
        pos(s, at)->deleteAllKeys();
        tan(s, at)->deleteAllKeys();
        pos(s, at)->setValue(nx, ny);
        tan(s, at)->setValue(0.0, 0.0);

        _sCount[s]->setValue(n + 1);
    }
    catch (...) {}
}

void BetterLineInteract::deletePoint(int s, int at)
{
    try
    {
        // getValue, not getValueAtTime: the point count is declared
        // non-animating, so there is exactly one answer and asking at a time
        // would only invite the question of which one.
        int count = 2;
        _sCount[s]->getValue(count);
        // Two points is the minimum that still describes a line. Refusing here
        // rather than clamping later keeps the parameter and the geometry from
        // disagreeing about how many points exist.
        if (count <= 2 || at < 0 || at >= count) return;

        EditBlock eb(_effect, "Delete Point");

        for (int i = at; i < count - 1; ++i)
        {
            pos(s, i)->copyFrom(*pos(s, i + 1), 0, nullptr);
            tan(s, i)->copyFrom(*tan(s, i + 1), 0, nullptr);
        }
        _sCount[s]->setValue(count - 1);
    }
    catch (...) {}
}

void BetterLineInteract::abandonDrag()
{
    if (_drag == kDragNone) return;
    _drag = kDragNone;
    _dragRef = PointRef();

    // Force rather than a matching EndEdit: this path exists precisely for the
    // case where the matching event never arrived.
    ForceCloseEdits(_effect);
}

// --- easing curve editor -----------------------------------------------------

bool BetterLineInteract::curveVisible(double t) const
{
    bool v = false;
    if (_showCurve) _showCurve->getValueAtTime(t, v);
    return v;
}

int BetterLineInteract::curveStage(double t) const
{
    int v = 0;
    if (_curveStage) _curveStage->getValueAtTime(t, v);
    return (v == 1) ? 1 : 0;
}

/** Built from the four human controls, exactly as the renderer builds it. The
 *  editor must plot the curve that will actually be used, not an approximation
 *  of it -- a second mapping here would drift from the one in stageAt(). */
Easing BetterLineInteract::curveAt(double t, int i) const
{
    double a = 0, b = 0, c = 0, d = 0;
    if (_easeIn[i])  _easeIn[i]->getValueAtTime(t, a);
    if (_easeOut[i]) _easeOut[i]->getValueAtTime(t, b);
    if (_antic[i])   _antic[i]->getValueAtTime(t, c);
    if (_over[i])    _over[i]->getValueAtTime(t, d);

    return MakeEasing(static_cast<float>(a), static_cast<float>(b),
                      static_cast<float>(c), static_cast<float>(d));
}

/** The panel, anchored to the image's bottom-left in SCREEN-sized units.
 *
 * Anchored to the image rather than the viewport because OFX 1.4 removed
 * kOfxInteractPropViewportSize and offers no replacement -- there is no way for
 * a plugin to learn where the viewport is. The panel therefore drifts off screen
 * at deep zoom, and "zoom to fit" is the only answer.
 */
OfxRectD BetterLineInteract::curveRect(const OverlayContext& c) const
{
    const double m  = c.sx(28.0), my = c.sy(28.0);
    const double w  = c.sx(200.0), h = c.sy(200.0);
    return { c.rod.x1 + m, c.rod.y1 + my, c.rod.x1 + m + w, c.rod.y1 + my + h };
}

/** The plotting area: the panel minus the tab strip along its top. */
OfxRectD BetterLineInteract::curvePlotRect(const OverlayContext& c) const
{
    OfxRectD r = curveRect(c);
    const double pad = c.sx(8.0), padY = c.sy(8.0);
    r.x1 += pad; r.x2 -= pad;
    r.y1 += padY;
    r.y2 -= c.sy(30.0);   // room for the tabs
    return r;
}

/** @brief One of the two stage tabs, drawn ON the panel.
 *
 * Derived, not stored, so the drawing and the hit test cannot fall out of step.
 * A stored rect would be stale on the first click after a viewer resize.
 */
OfxRectD BetterLineInteract::curveTabRect(const OverlayContext& c, int which) const
{
    const OfxRectD r   = curveRect(c);
    const double   pad = c.sx(8.0);
    const double   w   = (r.x2 - r.x1 - pad * 3.0) * 0.5;
    const double   y2  = r.y2 - c.sy(6.0);
    const double   y1  = y2 - c.sy(18.0);
    const double   x1  = r.x1 + pad + (w + pad) * static_cast<double>(which);
    return { x1, y1, x1 + w, y2 };
}

OfxPointD BetterLineInteract::curveToCanonical(const OverlayContext& c,
                                               float tx, float vy) const
{
    const OfxRectD r = curvePlotRect(c);
    const float span = Max(_curveHi - _curveLo, 1e-4f);
    return { r.x1 + static_cast<double>(tx) * (r.x2 - r.x1),
             r.y1 + static_cast<double>((vy - _curveLo) / span) * (r.y2 - r.y1) };
}

void BetterLineInteract::drawCurve(const OverlayContext& c)
{
    const int    stage = curveStage(c.time);
    const Easing e     = curveAt(c.time, stage);

    // Refit only when NOT dragging -- see the note on _curveLo.
    if (_drag != kDragCurve1 && _drag != kDragCurve2)
    {
        float lo = 0.0f, hi = 1.0f;
        for (int k = 0; k <= 32; ++k)
        {
            const float v = ApplyEasing(static_cast<float>(k) / 32.0f, e);
            lo = Min(lo, v); hi = Max(hi, v);
        }
        // The handles are included so an off-curve handle stays grabbable rather
        // than sitting outside the plot with nothing to take hold of.
        lo = Min(lo, Min(e.y1, e.y2));
        hi = Max(hi, Max(e.y1, e.y2));

        const float pad = Max((hi - lo) * 0.12f, 0.08f);
        _curveLo = lo - pad;
        _curveHi = hi + pad;
    }

    const OfxRectD r = curveRect(c);

    // Panel and the 0..1 rails.
    SetColour(c, 0.05f, 0.06f, 0.09f, 0.78f);
    { const OfxPointD box[2] = {{r.x1, r.y1}, {r.x2, r.y2}}; DrawPrim(c, kOfxDrawPrimitiveRectangle, box, 2); }

    // --- stage tabs ----------------------------------------------------------
    // On the panel, not in the Inspector. A dropdown three collapsed groups away
    // is not somewhere anyone will find it, and the entire point of an on-screen
    // editor is not having to go and look.
    for (int k = 0; k < 2; ++k)
    {
        const OfxRectD tab = curveTabRect(c, k);
        const bool     on  = (k == stage);

        SetColour(c, on ? 0.20f : 0.10f, on ? 0.34f : 0.12f, on ? 0.48f : 0.16f, 0.95f);
        { const OfxPointD box[2] = {{tab.x1, tab.y1}, {tab.x2, tab.y2}};
          DrawPrim(c, kOfxDrawPrimitiveRectangle, box, 2); }

        SetLineWidth(c, 1.0f);
        SetColour(c, on ? 0.45f : 0.30f, on ? 0.85f : 0.40f, 1.0f, on ? 0.95f : 0.45f);
        { const OfxPointD loop[4] = {{tab.x1, tab.y1}, {tab.x2, tab.y1},
                                     {tab.x2, tab.y2}, {tab.x1, tab.y2}};
          DrawPrim(c, kOfxDrawPrimitiveLineLoop, loop, 4); }

        SetColour(c, 1.0f, 1.0f, 1.0f, on ? 1.0f : 0.55f);
        DrawText(c, k == 0 ? "In" : "Out",
                 { (tab.x1 + tab.x2) * 0.5, tab.y1 + c.sy(4.0) },
                 kOfxDrawTextAlignmentCenterH);
    }

    SetLineWidth(c, 1.0f);
    SetColour(c, 1.0f, 1.0f, 1.0f, 0.22f);
    for (int k = 0; k < 2; ++k)
    {
        const OfxPointD a = curveToCanonical(c, 0.0f, static_cast<float>(k));
        const OfxPointD b = curveToCanonical(c, 1.0f, static_cast<float>(k));
        DrawLine(c, a, b);
    }

    // The OTHER stage, ghosted. Both curves are usually being shaped against
    // each other -- an entrance and the exit that has to answer it -- and
    // showing only one means holding the other in your head.
    {
        const Easing g = curveAt(c.time, stage == 0 ? 1 : 0);
        _screen.clear();
        _screen.reserve(49);
        for (int k = 0; k <= 48; ++k)
        {
            const float p = static_cast<float>(k) / 48.0f;
            _screen.push_back(curveToCanonical(c, p, ApplyEasing(p, g)));
        }
        SetLineWidth(c, 1.0f);
        SetColour(c, 0.6f, 0.7f, 0.8f, 0.30f);
        DrawPrim(c, kOfxDrawPrimitiveLineStrip, _screen.data(),
                 static_cast<int>(_screen.size()));
    }

    // The curve, plotted through the SAME evaluator the renderer uses.
    {
        _screen.clear();
        _screen.reserve(65);
        for (int k = 0; k <= 64; ++k)
        {
            const float p = static_cast<float>(k) / 64.0f;
            _screen.push_back(curveToCanonical(c, p, ApplyEasing(p, e)));
        }
        SetLineWidth(c, 2.0f);
        SetColour(c, 0.4f, 0.85f, 1.0f, 0.95f);
        DrawPrim(c, kOfxDrawPrimitiveLineStrip, _screen.data(),
                 static_cast<int>(_screen.size()));
    }

    // Handles, with their tethers to the endpoints they belong to.
    const OfxPointD p0 = curveToCanonical(c, 0.0f, 0.0f);
    const OfxPointD p3 = curveToCanonical(c, 1.0f, 1.0f);
    const OfxPointD h1 = curveToCanonical(c, e.x1, e.y1);
    const OfxPointD h2 = curveToCanonical(c, e.x2, e.y2);

    SetLineWidth(c, 1.0f);
    SetColour(c, 1.0f, 0.8f, 0.3f, 0.45f);
    DrawLine(c, p0, h1);
    DrawLine(c, p3, h2);

    SetColour(c, 1.0f, 0.85f, 0.2f, 1.0f);
    DrawDisc(c, h1, 4.5);
    DrawDisc(c, h2, 4.5);
}

/** Map a dragged handle back onto the four human controls.
 *
 * The parameters are Ease In / Ease Out / Anticipation / Overshoot, not raw
 * bezier handles -- so the editor has to invert MakeEasing rather than write
 * x1/y1 directly. Keeping the parameters human-readable is worth this: the
 * Inspector still shows numbers someone can reason about after a drag.
 */
void BetterLineInteract::writeCurveHandle(double t, int i, bool second,
                                          float hx, float hy)
{
    // x is clamped to 0..1 because outside it the bezier folds back on itself
    // and one time maps to two values, which the solver cannot answer. y is
    // deliberately NOT clamped -- dragging past the rails is exactly what
    // produces anticipation and overshoot.
    hx = Clamp(hx, 0.0f, 1.0f);

    try
    {
        if (!second)
        {
            _easeIn[i]->setValue(hx * 100.0);
            _antic[i]->setValue(-hy / 0.55 * 100.0);
        }
        else
        {
            _easeOut[i]->setValue((1.0 - hx) * 100.0);
            _over[i]->setValue((hy - 1.0) / 0.55 * 100.0);
        }
    }
    catch (...) {}
    (void)t;
}

// --- draw --------------------------------------------------------------------

bool BetterLineInteract::draw(const OFX::DrawArgs& args)
{
    bl::ProbeScope _t("overlayDraw");

    OverlayContext c;
    if (!buildContext(args.time, args.pixelScale, args.context, c)) return false;
    if (!OFX::Private::gDrawSuite || !c.ctx) return false;

    try
    {
        const int strokes = strokeCount(c.time);

        for (int s = 0; s < strokes; ++s)
        {
            if (!strokeEnabled(s, c.time)) continue;

            const bool active = (_selected.stroke == s);

            Stroke stroke;
            buildStroke(c, s, stroke);
            if (stroke.pts.size() < 2) continue;

            // Flattened to half a SCREEN pixel, so the guide stays smooth when
            // zoomed in and cheap when zoomed out. The renderer's tolerance is
            // in image pixels; matching it here would give a visibly faceted
            // guide at high zoom over a line that is actually smooth.
            //
            // Floored, though: sx() shrinks without limit as the viewer zooms
            // in, and an unbounded tolerance asks for subdivision far finer than
            // the screen can show -- paid for on every redraw, and the overlay
            // redraws continuously while the playhead sits on the clip.
            const float tol = Max(static_cast<float>(c.sx(0.5)), 0.05f);
            FlattenStroke(stroke, tol, _poly);

            if (_poly.pts.size() >= 2)
            {
                _screen.clear();
                _screen.reserve(_poly.pts.size());
                for (const Vec2& v : _poly.pts) _screen.push_back({v.x, v.y});

                SetLineWidth(c, active ? 2.0f : 1.0f);
                SetColour(c, 0.35f, 0.75f, 1.0f, active ? 0.95f : 0.55f);
                DrawPrim(c, stroke.closed ? kOfxDrawPrimitiveLineLoop
                                          : kOfxDrawPrimitiveLineStrip,
                         _screen.data(), static_cast<int>(_screen.size()));
            }

            const int n = static_cast<int>(stroke.pts.size());

            // Tangent handles only for the selected stroke. Drawn for every
            // stroke they turn a multi-stroke shape into a thicket, and the ones
            // you cannot act on are the ones in the way.
            // Everything below reads from `stroke`, which buildStroke has just
            // filled in canonical coordinates. Going back to posAt/tanTip here
            // asked the host for the same values again -- and tanTip calls posAt
            // internally, so a handle cost five parameter fetches per redraw
            // that had already been made.
            if (active)
            {
                SetLineWidth(c, 1.0f);
                for (int i = 0; i < n; ++i)
                {
                    const Vec2& q = stroke.pts[i].pos;
                    const Vec2& t = stroke.pts[i].tan;
                    if (t.x == 0.0f && t.y == 0.0f) continue;   // a corner

                    const OfxPointD outP{q.x + t.x, q.y + t.y};
                    const OfxPointD inP {q.x - t.x, q.y - t.y};

                    SetColour(c, 0.9f, 0.8f, 0.3f, 0.5f);
                    DrawLine(c, inP, outP);

                    SetColour(c, 1.0f, 0.85f, 0.2f, 0.95f);
                    DrawDisc(c, outP, 3.5);
                    DrawDisc(c, inP, 3.5);
                }
            }

            for (int i = 0; i < n; ++i)
            {
                const OfxPointD p{stroke.pts[i].pos.x, stroke.pts[i].pos.y};
                const bool sel = (_selected.stroke == s && _selected.index == i);

                const Vec2& tn = stroke.pts[i].tan;
                const bool corner = (tn.x == 0.0f && tn.y == 0.0f);

                if (sel) SetColour(c, 1.0f, 0.95f, 0.3f, 1.0f);
                else if (active) SetColour(c, 1.0f, 1.0f, 1.0f, 0.95f);
                else SetColour(c, 0.8f, 0.85f, 0.9f, 0.6f);

                SetLineWidth(c, 1.5f);

                // Square for a corner, round for a smooth point. A corner's
                // handles sit on top of the point and so cannot be drawn, which
                // otherwise leaves nothing on screen to say that Ctrl-dragging
                // it will do anything at all.
                if (corner) DrawHandle(c, p, sel ? 5.0 : 4.0, sel);
                else        DrawDisc(c, p, sel ? 5.0 : 4.0);

                if (sel)
                {
                    SetLineWidth(c, 1.0f);
                    SetColour(c, 1.0f, 0.95f, 0.3f, 0.55f);
                    DrawHandle(c, p, 8.5, false);
                }
            }
        }

        // Last, so it sits over the path handles -- and hit-tested first below,
        // for the same reason.
        if (curveVisible(c.time)) drawCurve(c);
    }
    catch (...) { return false; }

    return true;
}

// --- pen ---------------------------------------------------------------------

bool BetterLineInteract::penDown(const OFX::PenArgs& args)
{
    // Backstop for a drag whose mouse-up never arrived -- released outside the
    // viewer, or interrupted by a page switch. loseFocus covers most of it, but
    // a host that does not deliver that either would otherwise leave an edit
    // block open for the rest of the session.
    abandonDrag();

    OverlayContext c;
    if (!buildContext(args.time, args.pixelScale, nullptr, c)) return false;

    try
    {
        const OfxPointD pen = args.penPosition;

        // The curve panel is hit-tested FIRST because it is drawn on top, and it
        // is skipped ENTIRELY when hidden rather than merely not drawn -- a
        // hidden panel that still swallowed clicks would eat every attempt to
        // grab the path handles underneath it.
        if (curveVisible(c.time))
        {
            const int    stage = curveStage(c.time);
            const Easing e     = curveAt(c.time, stage);

            // Tabs first: they sit over the top of the plot area, and a click
            // that landed on a tab must not also be read as a curve edit.
            for (int k = 0; k < 2; ++k)
            {
                if (!Contains(curveTabRect(c, k), pen)) continue;
                if (k != stage)
                {
                    try
                    {
                        EditBlock eb(_effect, "Curve Stage");
                        _curveStage->setValue(k);
                    }
                    catch (...) {}
                    requestRedraw();
                }
                return true;
            }

            const OfxPointD h1 = curveToCanonical(c, e.x1, e.y1);
            const OfxPointD h2 = curveToCanonical(c, e.x2, e.y2);

            if (NearPoint(c, pen, h1, 9.0))
            {
                _drag = kDragCurve1; _pressPos = pen; _dragMoved = false;
                BeginEdit(_effect, "Easing Curve");
                return true;
            }
            if (NearPoint(c, pen, h2, 9.0))
            {
                _drag = kDragCurve2; _pressPos = pen; _dragMoved = false;
                BeginEdit(_effect, "Easing Curve");
                return true;
            }

            // Swallow anything else inside the panel, so a miss near a handle
            // does not fall through and start dragging a point behind it.
            if (Contains(curveRect(c), pen)) return true;
        }

        const int strokes = strokeCount(c.time);

        // Tangent handles are hit-tested BEFORE point handles, and only on the
        // selected stroke: they are drawn on top, and a handle you can see but
        // cannot grab because the thing underneath claims the click is worse
        // than one that is not drawn at all.
        if (_selected.valid() && _selected.stroke < strokes &&
            strokeEnabled(_selected.stroke, c.time))
        {
            const int s = _selected.stroke;
            const int n = pointCount(s, c.time);
            for (int i = 0; i < n; ++i)
            {
                double tx = 0, ty = 0;
                if (OFX::Double2DParam* q = tan(s, i)) q->getValueAtTime(c.time, tx, ty);
                if (tx == 0.0 && ty == 0.0) continue;

                if (NearPoint(c, pen, tanTip(c, s, i, true), 7.0))
                {
                    _drag = kDragTanOut; _dragRef = {s, i};
                    _grab = {0.0, 0.0};
                    _pressPos = pen; _dragMoved = false;
                    BeginEdit(_effect, "Drag Handle");
                    return true;
                }
                if (NearPoint(c, pen, tanTip(c, s, i, false), 7.0))
                {
                    _drag = kDragTanIn; _dragRef = {s, i};
                    _grab = {0.0, 0.0};
                    _pressPos = pen; _dragMoved = false;
                    BeginEdit(_effect, "Drag Handle");
                    return true;
                }
            }
        }

        for (int s = 0; s < strokes; ++s)
        {
            if (!strokeEnabled(s, c.time)) continue;
            const int n = pointCount(s, c.time);

            for (int i = 0; i < n; ++i)
            {
                const OfxPointD p = posAt(c, s, i);
                if (!NearPoint(c, pen, p, 8.0)) continue;

                if (_altDown)
                {
                    deletePoint(s, i);
                    if (_selected.stroke == s && _selected.index >= i) _selected.index = -1;
                    requestRedraw();
                    return true;
                }

                _selected   = {s, i};
                _pressPos   = pen;
                _dragMoved  = false;
                _dragRef    = {s, i};

                // Ctrl-drag pulls the handle out of the point itself.
                //
                // Without this a corner point is a dead end: its handle tips sit
                // exactly ON the point, so they can be neither seen nor hit --
                // and hit-testing them there anyway would be worse, because the
                // tangent test runs first and would swallow every attempt to
                // drag the point. Since every point starts at zero, both the
                // defaults and every Shift-inserted point, that left handles
                // reachable only by typing numbers into the Inspector.
                if (_ctrlDown)
                {
                    _drag = kDragTanPull;
                    _grab = {0.0, 0.0};
                    BeginEdit(_effect, "Shape Handle");
                    requestRedraw();
                    return true;
                }

                _drag = kDragPoint;
                // Grab offset, so the point does not snap its centre to the
                // cursor the instant you touch it.
                _grab = {p.x - pen.x, p.y - pen.y};
                BeginEdit(_effect, "Drag Point");
                requestRedraw();
                return true;
            }
        }

        // Shift-click near the curve inserts a point there.
        if (_shiftDown)
        {
            int    bestS = -1, bestSpan = -1;
            double bestD = 1e30;

            for (int s = 0; s < strokes; ++s)
            {
                if (!strokeEnabled(s, c.time)) continue;

                Stroke stroke;
                buildStroke(c, s, stroke);
                const int n = static_cast<int>(stroke.pts.size());
                if (n < 2) continue;

                const int spans = stroke.closed ? n : n - 1;
                for (int sp = 0; sp < spans; ++sp)
                {
                    const ControlPoint& a = stroke.pts[sp];
                    const ControlPoint& b = stroke.pts[(sp + 1) % n];
                    const Vec2 p0 = a.pos, p1 = a.pos + a.tan;
                    const Vec2 p2 = b.pos - b.tan, p3 = b.pos;

                    for (int k = 0; k <= 16; ++k)
                    {
                        const Vec2 q = CubicAt(p0, p1, p2, p3, k / 16.0f);
                        const double d = std::max(std::fabs(q.x - pen.x) / c.pixelScale.x,
                                                  std::fabs(q.y - pen.y) / c.pixelScale.y);
                        if (d < bestD) { bestD = d; bestS = s; bestSpan = sp; }
                    }
                }
            }

            if (bestS >= 0 && bestD < 10.0)
            {
                // The click landed on the transformed curve, so the point it
                // becomes has to be stored in untransformed space.
                const Vec2 back = c.xform.ApplyInverse(
                    Vec2(static_cast<float>(pen.x), static_cast<float>(pen.y)));

                double nx = 0, ny = 0;
                c.toNormalised(back.x, back.y, nx, ny);
                insertPoint(bestS, bestSpan + 1, nx, ny, c.time);
                _selected = {bestS, bestSpan + 1};
                requestRedraw();
                return true;
            }
        }
    }
    catch (...) { abandonDrag(); return false; }

    return false;
}

bool BetterLineInteract::penMotion(const OFX::PenArgs& args)
{
    if (_drag == kDragNone) return false;

    OverlayContext c;
    if (!buildContext(args.time, args.pixelScale, nullptr, c)) return false;

    // The curve handles are not points on a path, so they carry no PointRef.
    if (_drag == kDragCurve1 || _drag == kDragCurve2)
    {
        try
        {
            const OfxRectD r    = curveRect(c);
            const double   wpx  = r.x2 - r.x1;
            const double   hpx  = r.y2 - r.y1;
            if (wpx <= 0.0 || hpx <= 0.0) return false;

            const float tx = static_cast<float>((args.penPosition.x - r.x1) / wpx);
            const float vy = _curveLo + static_cast<float>((args.penPosition.y - r.y1) / hpx)
                                        * (_curveHi - _curveLo);

            writeCurveHandle(c.time, curveStage(c.time), _drag == kDragCurve2, tx, vy);
            requestRedraw();
        }
        catch (...) { abandonDrag(); return false; }
        return true;
    }

    if (!_dragRef.valid()) return false;

    try
    {
        const int s = _dragRef.stroke;
        const int i = _dragRef.index;

        // Measured in SCREEN pixels, so the threshold that separates a click
        // from a drag is the same however far the viewer is zoomed in. Three
        // pixels is about the wobble of a deliberate click.
        {
            const double dx = (args.penPosition.x - _pressPos.x) / c.pixelScale.x;
            const double dy = (args.penPosition.y - _pressPos.y) / c.pixelScale.y;
            if (dx * dx + dy * dy > 9.0) _dragMoved = true;
        }

        // Shift locks the drag to an axis.
        //
        // The axis is decided from the drag SO FAR rather than latched when
        // Shift went down, so Shift can be pressed or released part-way through
        // and simply starts or stops constraining. Latching it at key-down means
        // committing to an axis before the user has indicated one.
        //
        // Compared in SCREEN pixels, because canonical units are not square on
        // an anamorphic frame and comparing them raw would bias every choice
        // towards the same axis.
        auto axisLock = [&c](double& x, double& y, double ax, double ay) {
            const double sdx = std::fabs(x - ax) / c.pixelScale.x;
            const double sdy = std::fabs(y - ay) / c.pixelScale.y;
            if (sdx >= sdy) y = ay; else x = ax;
        };

        if (_drag == kDragPoint)
        {
            double cx = args.penPosition.x + _grab.x;
            double cy = args.penPosition.y + _grab.y;

            // Anchored to where the point was when the press landed, not to the
            // pen: _grab already holds the offset between them, so this is the
            // point's own original position.
            if (_shiftDown) axisLock(cx, cy, _pressPos.x + _grab.x, _pressPos.y + _grab.y);

            // Back through the transform before writing. The handle was drawn
            // transformed, so the drag arrives in transformed space -- storing
            // it straight would apply the transform a second time on the next
            // draw, and the point would run away from the cursor.
            const Vec2 back = c.xform.ApplyInverse(Vec2(static_cast<float>(cx),
                                                        static_cast<float>(cy)));

            double nx = 0, ny = 0;
            c.toNormalised(back.x, back.y, nx, ny);
            writePoint(pos(s, i), c.time, nx, ny);
        }
        else
        {
            const OfxPointD p = posAt(c, s, i);
            double dx = args.penPosition.x - p.x;
            double dy = args.penPosition.y - p.y;

            // A handle locks against the point it belongs to, so Shift makes it
            // exactly horizontal or vertical rather than merely stopping it
            // drifting.
            if (_shiftDown) axisLock(dx, dy, 0.0, 0.0);

            // The in-handle is the mirror of the stored outgoing tangent, so
            // dragging it writes the negated delta. One symmetric handle is the
            // whole reason a point costs two Double2Ds rather than three.
            if (_drag == kDragTanIn) { dx = -dx; dy = -dy; }

            // A handle is a direction, so it goes back through the VECTOR
            // inverse -- the translation must not be subtracted from it.
            const Vec2 back = c.xform.ApplyVectorInverse(
                Vec2(static_cast<float>(dx), static_cast<float>(dy)));

            const double w = c.rod.x2 - c.rod.x1;
            const double h = c.rod.y2 - c.rod.y1;
            writePoint(tan(s, i), c.time,
                       (w > 1e-9) ? back.x / w : 0.0,
                       (h > 1e-9) ? back.y / h : 0.0);
        }

        requestRedraw();
    }
    catch (...)
    {
        // Dropping the capture matters: continuing to feed a drag that just
        // failed to write would repeat the failure on every mouse move.
        abandonDrag();
        return false;
    }

    return true;
}

bool BetterLineInteract::penUp(const OFX::PenArgs& args)
{
    if (_drag == kDragNone) return false;

    // A Ctrl press on a point that never travelled is a click, not a drag, and
    // it means the opposite: collapse the handle and make the point a corner
    // again. Without it the gesture is one-way -- a point could be made smooth
    // but never straightened back, short of typing two zeros into the Inspector.
    //
    // Written before EndEdit so it lands inside the block the press opened,
    // which keeps it a single undo step.
    if (_drag == kDragTanPull && !_dragMoved && _dragRef.valid())
    {
        writePoint(tan(_dragRef.stroke, _dragRef.index), args.time, 0.0, 0.0);
        requestRedraw();
    }

    _drag = kDragNone;
    _dragRef = PointRef();
    _dragMoved = false;
    EndEdit(_effect);
    return true;
}

// --- keys --------------------------------------------------------------------

bool BetterLineInteract::keyDown(const OFX::KeyArgs& args)
{
    if (args.keySymbol == kOfxKey_Alt_L     || args.keySymbol == kOfxKey_Alt_R)     _altDown = true;
    if (args.keySymbol == kOfxKey_Shift_L   || args.keySymbol == kOfxKey_Shift_R)   _shiftDown = true;
    if (args.keySymbol == kOfxKey_Control_L || args.keySymbol == kOfxKey_Control_R) _ctrlDown = true;

    // Never trapped. OFX pen actions carry no modifier state at all, so these
    // have to be tracked from key events -- but returning true would block the
    // key from every other interact AND from the host, so merely observing Shift
    // would swallow the host's own Shift shortcuts along with it.
    return false;
}

bool BetterLineInteract::keyUp(const OFX::KeyArgs& args)
{
    if (args.keySymbol == kOfxKey_Alt_L     || args.keySymbol == kOfxKey_Alt_R)     _altDown = false;
    if (args.keySymbol == kOfxKey_Shift_L   || args.keySymbol == kOfxKey_Shift_R)   _shiftDown = false;
    if (args.keySymbol == kOfxKey_Control_L || args.keySymbol == kOfxKey_Control_R) _ctrlDown = false;
    return false;
}

void BetterLineInteract::loseFocus(const OFX::FocusArgs& /*args*/)
{
    // A drag released outside the viewer, or interrupted by a page switch, never
    // delivers its mouse-up. Without this the edit block it opened stays open for
    // the rest of the session and the host's parameter state is left
    // inconsistent -- which is the bug where on-screen controls stop responding
    // everywhere, not just in this plugin, until Resolve is restarted.
    abandonDrag();

    // A host that delivers a modifier press but takes focus away before the
    // release would otherwise leave it stuck down for good.
    _altDown = _shiftDown = _ctrlDown = false;
}

class BetterLineOverlayDescriptor
    : public OFX::DefaultEffectOverlayDescriptor<BetterLineOverlayDescriptor,
                                                 BetterLineInteract>
{
};

} // namespace

void RegisterOverlay(OFX::ImageEffectDescriptor& desc)
{
    desc.setOverlayInteractDescriptor(new BetterLineOverlayDescriptor());
}

} // namespace bl
