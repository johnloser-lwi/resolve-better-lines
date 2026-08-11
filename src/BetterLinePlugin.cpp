#include "BetterLinePlugin.h"

// ImageProcessor lives here, not in ofxsImageEffect.h. Subclassing it buys the
// host's own render-window slicing across cores, and the GPU dispatch hook.
#include "ofxsProcessing.h"

#include "ClipTime.h"
#include "EditBlock.h"
#include "HostProbe.h"
#include "ParamNames.h"
#include "ParamRegistry.h"
#include "Preset.h"
#include "PresetIO.h"

#include "core/Anim.h"
#include "core/Easing.h"
#include "core/Geometry.h"
#include "core/PathBlob.h"
#include "core/PathBuild.h"
#include "core/SvgPath.h"
#include "core/Raster.h"
#include "core/Shapes.h"
#include "core/TileGrid.h"

#include "interact/Overlay.h"

#ifdef BETTERLINE_HAVE_CUDA
#include "render/CudaRender.h"
#endif

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// A note on coordinates, because it is the thing that usually goes wrong.
//
// OFX images are bottom-up: y = 0 is the BOTTOM row, and canonical overlay
// coordinates run the same way. This plugin therefore keeps everything -- stored
// points, the rasterizer, and the overlay -- in one y-up space and never flips
// anything. resolve-text-animator has a top-down core and has to convert at a
// single carefully-guarded boundary; having no boundary at all is better still,
// and it is available here only because nothing in this plugin reads pixels.
//
// Points are stored NORMALISED, (0,0) bottom-left to (1,1) top-right. Not
// pixels: renderScale tracks proxy scale, not timeline resolution, so it is 1.0
// on a UHD timeline even though the frame has doubled. A design authored at
// 1080p must land identically at UHD, and only a ratio does that.

namespace {

using namespace bl;

/** @brief ChoiceParam has no return-value getter, and a host is free to hand
 *         back an index we never defined. Clamp every read. */
int ChoiceAt(OFX::ChoiceParam* p, double t, int lo, int hi)
{
    int v = 0;
    p->getValueAtTime(t, v);
    return v < lo ? lo : (v > hi ? hi : v);
}

Color ColorAt(OFX::RGBAParam* p, double t)
{
    double r = 1, g = 1, b = 1, a = 1;
    p->getValueAtTime(t, r, g, b, a);
    return Color(static_cast<float>(r), static_cast<float>(g),
                 static_cast<float>(b), static_cast<float>(a));
}

double DoubleAt(OFX::DoubleParam* p, double t)
{
    double v = 0;
    p->getValueAtTime(t, v);
    return v;
}

bool BoolAt(OFX::BooleanParam* p, double t)
{
    bool v = false;
    p->getValueAtTime(t, v);
    return v;
}

// ---------------------------------------------------------------------------

/** @brief CPU renderer. Subclassing ImageProcessor buys the host's own render
 *         window slicing across cores, and the GPU dispatch hook for later. */
class LineProcessor : public OFX::ImageProcessor
{
public:
    explicit LineProcessor(OFX::ImageEffect& p_Instance) : OFX::ImageProcessor(p_Instance) {}

    void setSrcImg(OFX::Image* p_SrcImg) { _srcImg = p_SrcImg; }
    void setScene(const SceneView& p_Scene) { _scene = p_Scene; }

#ifdef BETTERLINE_HAVE_CUDA
    void setUpload(const CudaSceneDesc& p_Desc) { _upload = p_Desc; }

    /** @brief GPU path.
     *
     * Everything the base class hands us here is DEVICE memory: when Resolve
     * renders on the GPU the pixel pointers a plugin receives are CUDA
     * allocations, so nothing below may dereference them on the host. That also
     * means there is no falling back to the CPU loop from in here -- by the time
     * this is called the choice has been made, and the CPU code would read
     * device addresses.
     */
    void processImagesCUDA() override
    {
        const OfxRectI& w = _renderWindow;

        // Pointer arithmetic only -- getPixelAddress does bounds checks and
        // offsets, it never reads the pixel, so it is safe on device memory.
        float* dst = static_cast<float*>(_dstImg->getPixelAddress(w.x1, w.y1));
        const float* src = _srcImg
            ? static_cast<const float*>(_srcImg->getPixelAddress(w.x1, w.y1))
            : nullptr;

        const int dstStride = static_cast<int>(_dstImg->getRowBytes() / sizeof(float));
        const int srcStride = _srcImg
            ? static_cast<int>(_srcImg->getRowBytes() / sizeof(float)) : 0;

        if (!dst || !CudaRenderLine(_pCudaStream, src, srcStride, dst, dstStride,
                                    w.x1, w.y1, w.x2, w.y2, _upload))
        {
            bl::ProbeOnce("cudafail", "CUDA render failed; frames will be wrong");
            OFX::throwSuiteStatusException(kOfxStatFailed);
        }
    }
#endif

    void multiThreadProcessImages(OfxRectI p_Window) override
    {
        const int width = p_Window.x2 - p_Window.x1;
        if (width <= 0) return;

        for (int y = p_Window.y1; y < p_Window.y2; ++y)
        {
            if (_effect.abort()) break;

            float* dstRow = static_cast<float*>(_dstImg->getPixelAddress(p_Window.x1, y));
            if (!dstRow) continue;

            const float* srcRow = _srcImg
                ? static_cast<const float*>(_srcImg->getPixelAddress(p_Window.x1, y))
                : nullptr;

            // Whole-row copy first. This was a per-pixel four-float assignment
            // inside the shading loop, which is the same memory traffic written
            // the slowest possible way -- and it is the dominant cost of the
            // frame, because a line covers a few percent of it and the copy
            // covers all of it.
            const size_t rowBytes = static_cast<size_t>(width) * 4 * sizeof(float);
            if (srcRow) std::memcpy(dstRow, srcRow, rowBytes);
            else        std::memset(dstRow, 0, rowBytes);

            // Then shade only where something could be, skipping empty tiles a
            // whole column at a time rather than asking per pixel.
            for (int x = p_Window.x1; x < p_Window.x2; )
            {
                if (!_scene.AnythingAt(x, y))
                {
                    // bl::Min is float-typed; these are pixel indices.
                    const int next = _scene.NextTileX(x);
                    x = (next > x) ? ((next < p_Window.x2) ? next : p_Window.x2) : x + 1;
                    continue;
                }

                // +0.5 samples the pixel CENTRE. The corner drifts everything
                // half a pixel: invisible at 1:1, obvious once scaled up.
                const Premul line = ShadePixel(_scene,
                                               static_cast<float>(x) + 0.5f,
                                               static_cast<float>(y) + 0.5f);
                if (line.a > 0.0f)
                    Over(line, dstRow + static_cast<ptrdiff_t>(x - p_Window.x1) * 4);
                ++x;
            }
        }
    }

private:
    OFX::Image* _srcImg = nullptr;
    SceneView   _scene;
#ifdef BETTERLINE_HAVE_CUDA
    CudaSceneDesc _upload;
#endif
};

// ---------------------------------------------------------------------------

class BetterLinePlugin : public OFX::ImageEffect
{
public:
    explicit BetterLinePlugin(OfxImageEffectHandle p_Handle);

    void render(const OFX::RenderArguments& p_Args) override;
    bool isIdentity(const OFX::IsIdentityArguments& p_Args,
                    OFX::Clip*& p_IdentityClip, double& p_IdentityTime) override;
    void changedParam(const OFX::InstanceChangedArgs& p_Args,
                      const std::string& p_ParamName) override;
    void getClipPreferences(OFX::ClipPreferencesSetter& p_ClipPreferences) override;

private:
    void  syncVisibility();
    Style styleAt(double time, double frameHeight, double renderScale) const;
    void  buildScene(double time, const OfxRectI& rod, double renderScale,
                     std::vector<Segment>& segs, std::vector<FillEdge>& fill,
                     std::vector<Style>& styles) const;

    OFX::Clip* _dstClip = nullptr;
    OFX::Clip* _srcClip = nullptr;

    OFX::IntParam*      _strokeCount = nullptr;
    OFX::GroupParam*    _strokeGroup[kMaxStrokes]   = {};
    OFX::BooleanParam*  _sEnable[kMaxStrokes]       = {};
    OFX::IntParam*      _sCount[kMaxStrokes]        = {};
    OFX::BooleanParam*  _sClosed[kMaxStrokes]       = {};
    /// Fetched LAZILY. There are up to 256 of these, and a typical instance
    /// touches four -- fetching them all in the constructor put the cost of the
    /// worst case on every instance the host creates, which is the moment the
    /// editor was reported as stalling.
    mutable OFX::Double2DParam* _pos[kMaxStrokes][kMaxPoints] = {};
    mutable OFX::Double2DParam* _tan[kMaxStrokes][kMaxPoints] = {};

    OFX::Double2DParam* posParam(int s, int i) const;
    OFX::Double2DParam* tanParam(int s, int i) const;

    OFX::Double2DParam* _xfPos    = nullptr;
    OFX::DoubleParam*   _xfRot    = nullptr;
    OFX::DoubleParam*   _xfScale  = nullptr;
    OFX::DoubleParam*   _xfScaleY = nullptr;
    OFX::Double2DParam* _xfAnchor = nullptr;
    OFX::BooleanParam*  _xfLink   = nullptr;

    OFX::RGBAParam*    _color      = nullptr;
    OFX::DoubleParam*  _opacity    = nullptr;
    OFX::DoubleParam*  _thickness  = nullptr;
    OFX::DoubleParam*  _feather    = nullptr;
    OFX::ChoiceParam*  _cap        = nullptr;
    OFX::DoubleParam*  _taperStart = nullptr;
    OFX::DoubleParam*  _taperBody  = nullptr;
    OFX::DoubleParam*  _taperEnd   = nullptr;

    OFX::BooleanParam* _outline      = nullptr;
    OFX::DoubleParam*  _outlineWidth = nullptr;
    OFX::RGBAParam*    _outlineColor = nullptr;

    OFX::BooleanParam* _glow          = nullptr;
    OFX::DoubleParam*  _glowRadius    = nullptr;
    OFX::DoubleParam*  _glowIntensity = nullptr;
    OFX::RGBAParam*    _glowColor     = nullptr;

    OFX::BooleanParam*  _fill        = nullptr;
    OFX::RGBAParam*     _fillColor   = nullptr;
    OFX::DoubleParam*   _fillOpacity = nullptr;

    OFX::BooleanParam*  _shadow        = nullptr;
    OFX::Double2DParam* _shadowOffset  = nullptr;
    OFX::DoubleParam*   _shadowSoft    = nullptr;
    OFX::DoubleParam*   _shadowOpacity = nullptr;
    OFX::RGBAParam*     _shadowColor   = nullptr;

    OFX::BooleanParam* _arrowStart  = nullptr;
    OFX::BooleanParam* _arrowEnd    = nullptr;
    OFX::DoubleParam*  _arrowLength = nullptr;
    OFX::DoubleParam*  _arrowWidth  = nullptr;
    OFX::DoubleParam*  _arrowRound  = nullptr;
    OFX::DoubleParam*  _arrowPos    = nullptr;

    OFX::ChoiceParam*  _gradMode  = nullptr;
    OFX::RGBAParam*    _gradColor = nullptr;
    OFX::DoubleParam*  _gradStart = nullptr;
    OFX::DoubleParam*  _gradEnd   = nullptr;
    OFX::DoubleParam*  _gradMid   = nullptr;
    OFX::BooleanParam* _gradLoop  = nullptr;

    OFX::BooleanParam* _dash      = nullptr;
    OFX::DoubleParam*  _dashLen   = nullptr;
    OFX::DoubleParam*  _dashGap   = nullptr;
    OFX::DoubleParam*  _dashPhase = nullptr;

    OFX::DoubleParam*  _trimStart  = nullptr;
    OFX::DoubleParam*  _trimEnd    = nullptr;
    OFX::DoubleParam*  _trimOffset = nullptr;
    OFX::BooleanParam* _trimWrap   = nullptr;

    OFX::BooleanParam* _animEnable = nullptr;
    OFX::DoubleParam*  _clipLength = nullptr;
    OFX::ChoiceParam*  _timeOrigin = nullptr;
    OFX::DoubleParam*  _timeOffset = nullptr;

    OFX::DoubleParam*  _stagger      = nullptr;
    OFX::ChoiceParam*  _staggerOrder = nullptr;
    OFX::IntParam*     _staggerSeed  = nullptr;

    OFX::BooleanParam* _wiggle      = nullptr;
    OFX::DoubleParam*  _wiggleAmp   = nullptr;
    OFX::DoubleParam*  _wiggleWave  = nullptr;
    OFX::DoubleParam*  _wigglePhase = nullptr;
    OFX::IntParam*     _wiggleSeed  = nullptr;

    /// Indexed [0] = In, [1] = Out.
    OFX::BooleanParam* _stEnable[2]   = {};
    OFX::ChoiceParam*  _stTarget[2]   = {};
    OFX::DoubleParam*  _stFrom[2]     = {};
    OFX::DoubleParam*  _stTo[2]       = {};
    OFX::ChoiceParam*  _stAnchor[2]   = {};
    OFX::DoubleParam*  _stStart[2]    = {};
    OFX::DoubleParam*  _stDuration[2] = {};
    OFX::ChoiceParam*  _stPreset[2]   = {};
    OFX::DoubleParam*  _stEaseIn[2]   = {};
    OFX::DoubleParam*  _stEaseOut[2]  = {};
    OFX::DoubleParam*  _stAntic[2]    = {};
    OFX::DoubleParam*  _stOver[2]     = {};

    Stage stageAt(int which, double t) const;

    // --- presets -------------------------------------------------------------
    bool captureParam(const ParamEntry& e, PresetValue& out) const;
    void applyParam(const ParamEntry& e, const PresetValue* v);
    void savePreset();
    void loadPreset();
    void resetAll();
    void syncPresetFolderLabel();

    OFX::StringParam* _presetName   = nullptr;
    OFX::StringParam* _presetFolder = nullptr;

    // --- shape generator -----------------------------------------------------
    void addShape();

    OFX::ChoiceParam*  _shapeKind   = nullptr;
    OFX::IntParam*     _shapeSides  = nullptr;
    OFX::DoubleParam*  _shapeCorner = nullptr;
    OFX::DoubleParam*  _shapeSize   = nullptr;   ///< height
    OFX::DoubleParam*  _shapeWidth  = nullptr;
    OFX::ChoiceParam*  _shapeSlot   = nullptr;

    // --- imported paths ------------------------------------------------------
    void importSvg();

    OFX::BooleanParam* _importShow  = nullptr;
    OFX::BooleanParam* _importCol   = nullptr;
    OFX::BooleanParam* _importWid   = nullptr;
    OFX::DoubleParam*  _importScale = nullptr;
    OFX::StringParam*  _pathBlob    = nullptr;
    OFX::StringParam*  _importInfo  = nullptr;

    /// Parsed blob, kept until the string changes.
    ///
    /// An imported logo is tens of kilobytes of text and reparsing it on every
    /// frame would be a per-frame cost that scales with how detailed the artwork
    /// is -- exactly the wrong way round, since detail is what someone imports
    /// artwork for.
    mutable std::string         _blobCache;
    mutable std::vector<Stroke> _blobStrokes;

    /// Suppresses the per-parameter reactions while a whole preset is being
    /// written. Without it every one of ~240 setValue calls would dispatch back
    /// into changedParam, and syncVisibility alone would run hundreds of times
    /// for a single Load.
    bool _applyingPreset = false;

    /// Last secrecy state pushed to the host, so redundant calls can be skipped.
    ///
    /// syncVisibility touches every one of the several hundred point parameters,
    /// and each setIsSecret is a host round-trip that can make the Inspector
    /// re-lay-out. Almost all of them are setting the value the parameter
    /// already has.
    mutable bool _secretKnown = false;
    mutable bool _secretPos[kMaxStrokes][kMaxPoints] = {};
    mutable bool _secretStroke[kMaxStrokes] = {};

    /// Guards the preset <-> curve-control sync from re-entering itself:
    /// stamping a preset writes the four controls, and writing a control
    /// selects Custom. setValue dispatches straight back into changedParam, so
    /// unguarded the two chase each other.
    bool _syncingEase = false;
};

BetterLinePlugin::BetterLinePlugin(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    // Instance construction is a prime suspect for editor sluggishness that
    // playback does not show: a host may rebuild the effect stack on every
    // timeline edit, and this constructor fetches several hundred parameters.
    bl::ProbeScope _t("ctor");

    bl::ProbeHostOnce();

    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    _strokeCount = fetchIntParam(kParamStrokeCount);
    for (int s = 0; s < kMaxStrokes; ++s)
    {
        _strokeGroup[s] = fetchGroupParam(StrokeParam("group", s));
        _sEnable[s]     = fetchBooleanParam(StrokeParam(kStrokeEnable, s));
        _sCount[s]      = fetchIntParam(StrokeParam(kStrokeCount, s));
        _sClosed[s]     = fetchBooleanParam(StrokeParam(kStrokeClosed, s));
        // Point parameters are deliberately NOT fetched here; see posParam().

        // The descriptors were described with this secrecy already, so the
        // instance is born correct and syncVisibility has nothing to push.
        _secretStroke[s] = DefaultStrokeSecret(s);
        for (int i = 0; i < kMaxPoints; ++i) _secretPos[s][i] = DefaultPointSecret(s, i);
    }
    _secretKnown = true;

    _xfPos    = fetchDouble2DParam(kParamXfPos);
    _xfRot    = fetchDoubleParam(kParamXfRot);
    _xfScale  = fetchDoubleParam(kParamXfScale);
    _xfScaleY = fetchDoubleParam(kParamXfScaleY);
    _xfAnchor = fetchDouble2DParam(kParamXfAnchor);
    _xfLink   = fetchBooleanParam(kParamXfLink);

    _color      = fetchRGBAParam(kParamColor);
    _opacity    = fetchDoubleParam(kParamOpacity);
    _thickness  = fetchDoubleParam(kParamThickness);
    _feather    = fetchDoubleParam(kParamFeather);
    _cap        = fetchChoiceParam(kParamCap);
    _taperStart = fetchDoubleParam(kParamTaperStart);
    _taperBody  = fetchDoubleParam(kParamTaperBody);
    _taperEnd   = fetchDoubleParam(kParamTaperEnd);

    _outline      = fetchBooleanParam(kParamOutline);
    _outlineWidth = fetchDoubleParam(kParamOutlineWidth);
    _outlineColor = fetchRGBAParam(kParamOutlineColor);

    _glow          = fetchBooleanParam(kParamGlow);
    _glowRadius    = fetchDoubleParam(kParamGlowRadius);
    _glowIntensity = fetchDoubleParam(kParamGlowIntensity);
    _glowColor     = fetchRGBAParam(kParamGlowColor);

    _fill        = fetchBooleanParam(kParamFill);
    _fillColor   = fetchRGBAParam(kParamFillColor);
    _fillOpacity = fetchDoubleParam(kParamFillOpacity);

    _shadow        = fetchBooleanParam(kParamShadow);
    _shadowOffset  = fetchDouble2DParam(kParamShadowOffset);
    _shadowSoft    = fetchDoubleParam(kParamShadowSoft);
    _shadowOpacity = fetchDoubleParam(kParamShadowOpacity);
    _shadowColor   = fetchRGBAParam(kParamShadowColor);

    _arrowStart  = fetchBooleanParam(kParamArrowStart);
    _arrowEnd    = fetchBooleanParam(kParamArrowEnd);
    _arrowLength = fetchDoubleParam(kParamArrowLength);
    _arrowWidth  = fetchDoubleParam(kParamArrowWidth);
    _arrowRound  = fetchDoubleParam(kParamArrowRound);
    _arrowPos    = fetchDoubleParam(kParamArrowPos);

    _gradMode  = fetchChoiceParam(kParamGradMode);
    _gradColor = fetchRGBAParam(kParamGradColor);
    _gradStart = fetchDoubleParam(kParamGradStart);
    _gradEnd   = fetchDoubleParam(kParamGradEnd);
    _gradMid   = fetchDoubleParam(kParamGradMid);
    _gradLoop  = fetchBooleanParam(kParamGradLoop);

    _dash      = fetchBooleanParam(kParamDash);
    _dashLen   = fetchDoubleParam(kParamDashLen);
    _dashGap   = fetchDoubleParam(kParamDashGap);
    _dashPhase = fetchDoubleParam(kParamDashPhase);

    _trimStart  = fetchDoubleParam(kParamTrimStart);
    _trimEnd    = fetchDoubleParam(kParamTrimEnd);
    _trimOffset = fetchDoubleParam(kParamTrimOffset);
    _trimWrap   = fetchBooleanParam(kParamTrimWrap);

    _animEnable = fetchBooleanParam(kParamAnimEnable);
    _clipLength = fetchDoubleParam(kParamClipLength);
    _timeOrigin = fetchChoiceParam(kParamTimeOrigin);
    _timeOffset = fetchDoubleParam(kParamTimeOffset);

    _stagger      = fetchDoubleParam(kParamStagger);
    _staggerOrder = fetchChoiceParam(kParamStaggerOrder);
    _staggerSeed  = fetchIntParam(kParamStaggerSeed);

    _wiggle      = fetchBooleanParam(kParamWiggle);
    _wiggleAmp   = fetchDoubleParam(kParamWiggleAmp);
    _wiggleWave  = fetchDoubleParam(kParamWiggleWave);
    _wigglePhase = fetchDoubleParam(kParamWigglePhase);
    _wiggleSeed  = fetchIntParam(kParamWiggleSeed);
    for (int i = 0; i < 2; ++i)
    {
        const bool out = (i == 1);
        _stEnable[i]   = fetchBooleanParam(StageParam(kStageEnable, out));
        _stTarget[i]   = fetchChoiceParam(StageParam(kStageTarget, out));
        _stFrom[i]     = fetchDoubleParam(StageParam(kStageFrom, out));
        _stTo[i]       = fetchDoubleParam(StageParam(kStageTo, out));
        _stAnchor[i]   = fetchChoiceParam(StageParam(kStageAnchor, out));
        _stStart[i]    = fetchDoubleParam(StageParam(kStageStart, out));
        _stDuration[i] = fetchDoubleParam(StageParam(kStageDuration, out));
        _stPreset[i]   = fetchChoiceParam(StageParam(kStagePreset, out));
        _stEaseIn[i]   = fetchDoubleParam(StageParam(kStageEaseIn, out));
        _stEaseOut[i]  = fetchDoubleParam(StageParam(kStageEaseOut, out));
        _stAntic[i]    = fetchDoubleParam(StageParam(kStageAntic, out));
        _stOver[i]     = fetchDoubleParam(StageParam(kStageOver, out));
    }

    _shapeKind   = fetchChoiceParam(kParamShapeKind);
    _shapeSides  = fetchIntParam(kParamShapeSides);
    _shapeCorner = fetchDoubleParam(kParamShapeCorner);
    _shapeSize   = fetchDoubleParam(kParamShapeSize);
    _shapeWidth  = fetchDoubleParam(kParamShapeWidth);
    _shapeSlot   = fetchChoiceParam(kParamShapeSlot);

    _importShow  = fetchBooleanParam(kParamImportShow);
    _importCol   = fetchBooleanParam(kParamImportCol);
    _importWid   = fetchBooleanParam(kParamImportWid);
    _importScale = fetchDoubleParam(kParamImportScale);
    _pathBlob    = fetchStringParam(kParamPathBlob);
    _importInfo  = fetchStringParam(kParamImportInfo);

    _presetName   = fetchStringParam(kParamPresetName);
    _presetFolder = fetchStringParam(kParamPresetFolder);
    syncPresetFolderLabel();

    // Also on load, or a project saved with three strokes would open showing all
    // of them until something happened to touch the count.
    syncVisibility();
}

/** @brief Fetch a point parameter on first use.
 *
 * Wrapped rather than fetched up front because the ceiling is 256 of these and
 * a normal instance uses four. The host call is not free, and paying for the
 * worst case on every instance creation is what made dropping the effect onto a
 * clip feel slow.
 */
OFX::Double2DParam* BetterLinePlugin::posParam(int s, int i) const
{
    if (!_pos[s][i])
    {
        try { _pos[s][i] = const_cast<BetterLinePlugin*>(this)
                               ->fetchDouble2DParam(PointParam(kPointPos, s, i)); }
        catch (...) { return nullptr; }
    }
    return _pos[s][i];
}

OFX::Double2DParam* BetterLinePlugin::tanParam(int s, int i) const
{
    if (!_tan[s][i])
    {
        try { _tan[s][i] = const_cast<BetterLinePlugin*>(this)
                               ->fetchDouble2DParam(PointParam(kPointTan, s, i)); }
        catch (...) { return nullptr; }
    }
    return _tan[s][i];
}

// =============================================================================
// Presets
//
// Driven entirely by the registry, so every parameter is covered automatically
// and a new one needs nothing added here.
// =============================================================================

bool BetterLinePlugin::captureParam(const ParamEntry& e, PresetValue& out) const
{
    BetterLinePlugin* self = const_cast<BetterLinePlugin*>(this);
    out.kind = e.kind;

    try
    {
        switch (e.kind)
        {
            case kPKDouble:
                out.count = 1;
                out.n[0] = self->fetchDoubleParam(e.name)->getValue();
                return true;

            case kPKDouble2D:
            {
                double x = 0, y = 0;
                self->fetchDouble2DParam(e.name)->getValue(x, y);
                out.n[0] = x; out.n[1] = y; out.count = 2;
                return true;
            }
            case kPKRGBA:
            {
                double r = 0, g = 0, b = 0, a = 0;
                self->fetchRGBAParam(e.name)->getValue(r, g, b, a);
                out.n[0] = r; out.n[1] = g; out.n[2] = b; out.n[3] = a; out.count = 4;
                return true;
            }
            case kPKInt:
                out.count = 1;
                out.n[0] = self->fetchIntParam(e.name)->getValue();
                return true;

            case kPKChoice:
            {
                int v = 0;
                self->fetchChoiceParam(e.name)->getValue(v);
                out.n[0] = v; out.count = 1;
                return true;
            }
            case kPKBool:
                out.b = self->fetchBooleanParam(e.name)->getValue();
                return true;

            case kPKString:
                self->fetchStringParam(e.name)->getValue(out.s);
                return true;

            default: return false;
        }
    }
    catch (...) { return false; }
}

/** @brief Apply one parameter, or return it to its default if @p v is null.
 *
 * Missing keys resetting to the DEFAULT rather than being left alone is what
 * makes a preset deterministic: loading the same file from two different
 * starting states gives the same result. Leaving them alone would mean a preset
 * quietly inherited whatever happened to be set before it.
 */
void BetterLinePlugin::applyParam(const ParamEntry& e, const PresetValue* v)
{
    try
    {
        switch (e.kind)
        {
            case kPKDouble:
            {
                OFX::DoubleParam* p = fetchDoubleParam(e.name);
                p->setValue(v ? v->n[0] : p->getDefault());
                break;
            }
            case kPKDouble2D:
            {
                OFX::Double2DParam* p = fetchDouble2DParam(e.name);
                if (v && v->count >= 2) p->setValue(v->n[0], v->n[1]);
                else { double x = 0, y = 0; p->getDefault(x, y); p->setValue(x, y); }
                break;
            }
            case kPKRGBA:
            {
                OFX::RGBAParam* p = fetchRGBAParam(e.name);
                if (v && v->count >= 4) p->setValue(v->n[0], v->n[1], v->n[2], v->n[3]);
                else { double r = 0, g = 0, b = 0, a = 0; p->getDefault(r, g, b, a);
                       p->setValue(r, g, b, a); }
                break;
            }
            case kPKInt:
            {
                OFX::IntParam* p = fetchIntParam(e.name);
                p->setValue(v ? static_cast<int>(v->n[0]) : p->getDefault());
                break;
            }
            case kPKChoice:
            {
                OFX::ChoiceParam* p = fetchChoiceParam(e.name);
                int d = 0; p->getDefault(d);
                p->setValue(v ? static_cast<int>(v->n[0]) : d);
                break;
            }
            case kPKBool:
            {
                OFX::BooleanParam* p = fetchBooleanParam(e.name);
                bool d = false; p->getDefault(d);
                p->setValue(v ? v->b : d);
                break;
            }
            case kPKString:
            {
                OFX::StringParam* p = fetchStringParam(e.name);
                std::string d; p->getDefault(d);
                p->setValue(v ? v->s : d);
                break;
            }
            default: break;
        }
    }
    catch (...) {}   // a parameter that will not take a value is not worth aborting for
}

void BetterLinePlugin::savePreset()
{
    std::string name;
    _presetName->getValue(name);
    name = SanitiseFileName(name);

    // The dialog is deliberately NOT inside an edit block. changedParam is
    // dispatched synchronously from the button press, so the dialog opens
    // inside this call -- and a modal Win32 dialog pumps its own message loop.
    // Holding an OFX edit block open across that hands the host an unbounded
    // amount of unrelated work while it believes a parameter edit is in flight.
    std::string path;
    if (!ChoosePresetToSave(PresetFolder(), name.empty() ? "Better Line" : name, path))
        return;

    PresetFile file;
    file.schemaVersion = kPresetSchemaVersion;
    file.name = name.empty() ? StemOf(path) : name;

    for (const ParamEntry& e : ParamRegistry())
    {
        PresetValue v;
        if (captureParam(e, v)) file.params[e.name] = v;
    }

    std::string error;
    if (!WriteTextFile(path, PresetToJson(file), error))
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Could not save the preset:\n" + error);
        return;
    }

    bl::ProbeLog("preset saved: " + path + "  (" +
                 std::to_string(file.params.size()) + " parameters)");
}

void BetterLinePlugin::loadPreset()
{
    std::string path;
    if (!ChoosePresetToOpen(PresetFolder(), path)) return;   // dialog outside any block

    std::string text, error;
    if (!ReadTextFile(path, text, error))
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Could not read the preset:\n" + error);
        return;
    }

    PresetFile file;
    if (!PresetFromJson(text, file, error))
    {
        // Refused rather than partially applied: half a preset is a state the
        // user never authored and cannot get back from.
        sendMessage(OFX::Message::eMessageError, "",
                    "That file is not a valid Better Line preset:\n" + error);
        return;
    }

    {
        // One edit block for the whole batch. It is what tells the host an edit
        // happened at all, and it collapses ~240 writes into a single undo step
        // and a single cache invalidation instead of 240 of each.
        bl::EditBlock block(this, "Load Better Line Preset");
        _applyingPreset = true;

        for (const ParamEntry& e : ParamRegistry())
        {
            auto it = file.params.find(e.name);
            applyParam(e, it == file.params.end() ? nullptr : &it->second);
        }

        _applyingPreset = false;
    }

    // Once, after the batch, rather than on each of 240 changedParam callbacks.
    syncVisibility();

    if (!file.name.empty()) _presetName->setValue(file.name);
    bl::ProbeLog("preset loaded: " + path);
}

void BetterLinePlugin::resetAll()
{
    {
        bl::EditBlock block(this, "Reset Better Line");
        _applyingPreset = true;
        for (const ParamEntry& e : ParamRegistry()) applyParam(e, nullptr);
        _applyingPreset = false;
    }
    syncVisibility();
}

void BetterLinePlugin::addShape()
{
    const int kind   = ChoiceAt(_shapeKind, 0.0, kShapeRectangle, kShapePolygon);
    const int slotSel = ChoiceAt(_shapeSlot, 0.0, 0, kMaxStrokes);

    double corner = 0.0, sizePct = 40.0, widthPct = 40.0;
    int    sides  = 6;
    _shapeCorner->getValue(corner);
    _shapeSize->getValue(sizePct);
    _shapeWidth->getValue(widthPct);
    _shapeSides->getValue(sides);

    const bool rounded = (kind != kShapeEllipse) && (corner > 0.01);

    // Rounding costs two points per corner, so a polygon that fits sharp may not
    // fit rounded. syncVisibility keeps Sides inside the achievable range as the
    // radius moves, so this should be unreachable -- it stays as a backstop
    // because drawing a polygon with silently fewer sides than asked for is the
    // one outcome worse than a message.
    const int maxSides = MaxSidesFor(kMaxPoints, rounded);
    if (kind == kShapePolygon && sides > maxSides)
    {
        std::ostringstream m;
        m << "A " << sides << "-sided polygon needs "
          << ShapePointCount(kind, sides, rounded) << " points, and a stroke holds "
          << kMaxPoints << ".";
        if (rounded) m << "\n\nRounded corners cost two points each. Without them, "
                       << MaxSidesFor(kMaxPoints, false) << " sides fit.";
        sendMessage(OFX::Message::eMessageError, "", m.str());
        return;
    }

    // "Next free" means the first stroke that is switched off. A shape written
    // over a stroke someone is working on is not a convenience.
    int slot = slotSel - 1;
    if (slotSel == 0)
    {
        slot = -1;
        for (int s = 0; s < kMaxStrokes; ++s)
        {
            bool on = false;
            _sEnable[s]->getValue(on);
            if (!on) { slot = s; break; }
        }
        if (slot < 0)
        {
            sendMessage(OFX::Message::eMessageError, "",
                        "All " + std::to_string(kMaxStrokes) +
                        " strokes are in use. Pick one to overwrite, or switch one off.");
            return;
        }
    }

    // Slot x is a fraction of WIDTH and y a fraction of HEIGHT, so a shape built
    // with equal radii comes out stretched by the frame aspect. The generators
    // divide x by this to undo that; without it a "circle" on a 16:9 timeline is
    // an ellipse nearly twice as wide as it is tall.
    double aspect = 1.0;
    if (_srcClip && _srcClip->isConnected())
    {
        const OfxRectD rod = _srcClip->getRegionOfDefinition(0.0);
        const double w = rod.x2 - rod.x1, h = rod.y2 - rod.y1;
        if (w > 0.0 && h > 0.0) aspect = w / h;
    }

    // Both are a percentage of frame HEIGHT, so a square is Width == Height on
    // any timeline. Separate axes here rather than leaning on Transform Scale,
    // because a non-uniform scale stretches the corner fillets from circular
    // arcs into elliptical ones -- the rounding has to be built at the final
    // proportions to stay circular.
    const float halfW = static_cast<float>(widthPct) * 0.005f;
    const float halfH = static_cast<float>(sizePct)  * 0.005f;

    std::vector<ControlPoint> pts;
    MakeShape(kind, sides, static_cast<float>(corner) * 0.01f,
              0.5f, 0.5f, halfW, halfH, static_cast<float>(aspect), pts);

    if (pts.size() < 3) return;
    if (pts.size() > static_cast<size_t>(kMaxPoints)) pts.resize(static_cast<size_t>(kMaxPoints));

    {
        bl::EditBlock block(this, "Add Shape");

        for (size_t i = 0; i < pts.size(); ++i)
        {
            OFX::Double2DParam* p = posParam(slot, static_cast<int>(i));
            OFX::Double2DParam* q = tanParam(slot, static_cast<int>(i));
            if (!p || !q) continue;

            // Any keys already on this slot describe the shape that used to be
            // here, and would otherwise win at every frame -- the button would
            // look like it had done nothing. Spawning a shape is a structural
            // act, so the slot is cleared rather than keyed.
            try { p->deleteAllKeys(); q->deleteAllKeys(); } catch (...) {}

            p->setValue(pts[i].pos.x, pts[i].pos.y);
            q->setValue(pts[i].tan.x, pts[i].tan.y);
        }

        _sCount[slot]->setValue(static_cast<int>(pts.size()));
        _sClosed[slot]->setValue(true);
        _sEnable[slot]->setValue(true);

        int live = 0;
        _strokeCount->getValue(live);
        if (live < slot + 1) _strokeCount->setValue(slot + 1);
    }

    syncVisibility();

    static const char* kNames[] = {"rectangle", "ellipse", "polygon"};
    bl::ProbeLog("add shape: " + std::string(kNames[kind < 0 ? 0 : (kind > 2 ? 2 : kind)]) +
                 " -> stroke " + std::to_string(slot + 1) +
                 ", " + std::to_string(pts.size()) + " points");
}

void BetterLinePlugin::importSvg()
{
    // Outside any edit block: the dialog pumps its own message loop.
    std::string path;
    if (!ChoosePresetToOpen(PresetFolder(), path)) return;

    std::string text, error;
    if (!ReadTextFile(path, text, error))
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not read the file:\n" + error);
        return;
    }

    const SvgImportResult r = ParseSvg(text);
    if (!r.error.empty() || r.strokes.empty())
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Could not import that SVG:\n" +
                    (r.error.empty() ? std::string("no path geometry found") : r.error) +
                    "\n\nOnly path geometry is read -- text must be converted to "
                    "outlines first, and images cannot be imported.");
        return;
    }

    // What the artwork actually contains decides the starting state, because the
    // two kinds of SVG want opposite defaults and neither is discoverable.
    bool anyFill = false, anyStroke = false;
    for (const Stroke& s : r.strokes)
    {
        anyFill   = anyFill   || s.paint.hasFill;
        anyStroke = anyStroke || s.paint.hasStroke;
    }

    {
        bl::EditBlock block(this, "Import SVG");
        _pathBlob->setValue(StrokesToBlob(r.strokes));
        _importShow->setValue(true);

        // Fill is a master switch over the per-path flag, so filled artwork has
        // to turn it on or it imports invisible.
        if (anyFill) _fill->setValue(true);

        // Artwork with NO stroke anywhere is the common case -- most icon sets
        // are drawn as filled silhouettes -- and it is exactly the case that
        // arrives with nothing to draw on: Trim reveals a STROKE, so a shape
        // that has none animates not at all. Handing those paths the global
        // Thickness gives them an outline to draw, which is the reason someone
        // brought a logo into a line plugin. Left alone when the file does carry
        // strokes, since there the artwork's own widths are the better answer.
        if (!anyStroke) _importWid->setValue(false);
    }

    std::ostringstream o;
    o << StemOf(path) << ": " << r.subpaths << " paths, " << r.points << " points";
    if (!anyStroke) o << ", fill only";
    if (r.truncated) o << " (TRUNCATED -- the file was larger than the import limit)";
    _importInfo->setValue(o.str());

    bl::ProbeLog("svg import: " + o.str() + (r.sawArc ? "  [arcs converted]" : ""));
}

void BetterLinePlugin::syncPresetFolderLabel()
{
    // Shown resolved rather than as "(default)": the point of the read-out is to
    // answer "where will the dialog open", and "default" only moves the question
    // along.
    try { if (_presetFolder) _presetFolder->setValue(PresetFolder()); }
    catch (...) {}
}

/** @brief One place, one pass.
 *
 * Every visibility rule lives here and nowhere else. Split across several
 * independent passes they overwrite each other and strand a control hidden --
 * which is a bug the user cannot even report, because the evidence is invisible.
 */
void BetterLinePlugin::syncVisibility()
{
    bl::ProbeScope _t("syncVisibility");
    try
    {
        // Hidden while linked, because it is not read while linked. Leaving it
        // on screen would show a number that has no effect, which is exactly the
        // confusion the mirroring was meant to avoid and caused instead.
        {
            bool link = true;
            _xfLink->getValue(link);
            _xfScaleY->setIsSecret(link);
        }

        {
            const int kind = ChoiceAt(_shapeKind, 0.0, kShapeRectangle, kShapePolygon);
            _shapeSides->setIsSecret(kind != kShapePolygon);
            _shapeCorner->setIsSecret(kind == kShapeEllipse);

            // Rounding costs two points per corner, so the achievable side count
            // halves the moment the radius leaves zero. Moving the RANGE rather
            // than only validating on the button press means the control cannot
            // be put into a state the button has to refuse -- a slider that stops
            // where the limit is explains the limit better than a dialog does.
            double corner = 0.0;
            _shapeCorner->getValue(corner);
            const int maxSides = MaxSidesFor(kMaxPoints, corner > 0.01);

            _shapeSides->setRange(3, maxSides);
            _shapeSides->setDisplayRange(3, maxSides);

            int sides = 3;
            _shapeSides->getValue(sides);
            if (sides > maxSides) _shapeSides->setValue(maxSides);
        }

        int strokes = 1;
        _strokeCount->getValue(strokes);
        strokes = strokes < 1 ? 1 : (strokes > kMaxStrokes ? kMaxStrokes : strokes);

        const bool first = !_secretKnown;

        for (int s = 0; s < kMaxStrokes; ++s)
        {
            const bool liveStroke = (s < strokes);

            // The group header is hidden too, not just its contents. Hiding only
            // the contents leaves a column of empty section headers, which reads
            // as a broken panel rather than a tidy one.
            if (first || _secretStroke[s] != !liveStroke)
            {
                _strokeGroup[s]->setIsSecret(!liveStroke);
                _sEnable[s]->setIsSecret(!liveStroke);
                _sCount[s]->setIsSecret(!liveStroke);
                _sClosed[s]->setIsSecret(!liveStroke);
                _secretStroke[s] = !liveStroke;
            }

            int pts = 2;
            _sCount[s]->getValue(pts);
            pts = pts < 2 ? 2 : (pts > kMaxPoints ? kMaxPoints : pts);

            for (int i = 0; i < kMaxPoints; ++i)
            {
                const bool secret = !(liveStroke && (i < pts));
                if (!first && _secretPos[s][i] == secret) continue;

                OFX::Double2DParam* p = posParam(s, i);
                OFX::Double2DParam* q = tanParam(s, i);
                if (p) p->setIsSecret(secret);
                if (q) q->setIsSecret(secret);
                _secretPos[s][i] = secret;
            }
        }

        _secretKnown = true;
    }
    catch (...) {}   // a stranded control is bad; a throw into the host is worse
}

void BetterLinePlugin::changedParam(const OFX::InstanceChangedArgs& p_Args,
                                    const std::string& p_ParamName)
{
    bl::ProbeScope _t("changedParam");
    if (_syncingEase) return;

    // While a preset is being written, every setValue dispatches back here.
    // Reacting to each would run syncVisibility hundreds of times for one Load,
    // and would let an easing control stamp Custom over a preset's own preset.
    if (_applyingPreset) return;

    if (p_ParamName == kParamShapeAdd)     { addShape(); return; }
    // The radius decides how many sides still fit, so it drives visibility too.
    if (p_ParamName == kParamShapeKind ||
        p_ParamName == kParamShapeCorner)  { syncVisibility(); return; }

    if (p_ParamName == kParamImportSvg)    { importSvg(); return; }
    if (p_ParamName == kParamClearImport)
    {
        bl::EditBlock block(this, "Clear Import");
        _pathBlob->setValue("");
        _importInfo->setValue("nothing imported");
        return;
    }

    if (p_ParamName == kParamPresetSave)   { savePreset(); return; }
    if (p_ParamName == kParamPresetLoad)   { loadPreset(); return; }
    if (p_ParamName == kParamPresetReset)  { resetAll();   return; }
    if (p_ParamName == kParamPresetSetDir)
    {
        std::string picked;
        if (!ChooseFolder(PresetFolder(), picked)) return;   // modal: no edit block

        std::string error;
        if (!SavePresetFolderSetting(picked, error))
        {
            sendMessage(OFX::Message::eMessageError, "",
                        "Could not save the preset folder setting:\n" + error);
            return;
        }
        syncPresetFolderLabel();
        return;
    }

    // Nothing to write: the link only decides whether Scale Y is READ. All this
    // does is show or hide the field.
    if (p_ParamName == kParamXfLink) { syncVisibility(); return; }

    if (p_ParamName == kParamStrokeCount) { syncVisibility(); return; }

    for (int s = 0; s < kMaxStrokes; ++s)
    {
        if (p_ParamName == StrokeParam(kStrokeCount, s)) { syncVisibility(); return; }
    }

    // Easing preset <-> the four curve controls, in both directions.
    for (int i = 0; i < 2; ++i)
    {
        const bool out = (i == 1);

        if (p_ParamName == StageParam(kStagePreset, out))
        {
            const int preset = ChoiceAt(_stPreset[i], 0.0, 0, kEasePresetCount - 1);
            if (preset == kEaseCustom) return;   // Custom keeps whatever is set

            float a = 0, b = 0, c = 0, d = 0;
            PresetValues(preset, a, b, c, d);

            _syncingEase = true;
            {
                bl::EditBlock eb(this, "Easing Preset");
                _stEaseIn[i]->setValue(a);
                _stEaseOut[i]->setValue(b);
                _stAntic[i]->setValue(c);
                _stOver[i]->setValue(d);
            }
            _syncingEase = false;
            return;
        }

        // Touching any curve control means the preset name no longer describes
        // what is set, so the dropdown stops claiming it does.
        if (p_ParamName == StageParam(kStageEaseIn, out) ||
            p_ParamName == StageParam(kStageEaseOut, out) ||
            p_ParamName == StageParam(kStageAntic, out) ||
            p_ParamName == StageParam(kStageOver, out))
        {
            _syncingEase = true;
            { bl::EditBlock eb(this, "Custom Easing"); _stPreset[i]->setValue(kEaseCustom); }
            _syncingEase = false;
            return;
        }
    }
}

/** @brief Time drives the image without any keyframe saying so.
 *
 * Without this a host is entirely within its rights to render one frame and
 * reuse it for the whole clip -- and Fusion does exactly that, so playback shows
 * a frozen image while the controls still update it live. Resolve's Edit page
 * happens to re-render regardless, which is why this goes unnoticed there.
 */
void BetterLinePlugin::getClipPreferences(OFX::ClipPreferencesSetter& p_ClipPreferences)
{
    p_ClipPreferences.setOutputFrameVarying(true);
}

/** @brief Never a pass-through.
 *
 * Deliberately unconditional, and deliberately not tested at p_Args.time. The
 * host caches an identity verdict per frame; move the line so it now covers a
 * frame that previously drew nothing, and the host stays convinced nothing
 * happens there. The edit then appears to do nothing at all until the cache is
 * purged by hand, which is close to unreportable as a bug.
 */
bool BetterLinePlugin::isIdentity(const OFX::IsIdentityArguments& /*p_Args*/,
                                  OFX::Clip*& /*p_IdentityClip*/, double& /*p_IdentityTime*/)
{
    return false;
}

Style BetterLinePlugin::styleAt(double t, double frameHeight, double renderScale) const
{
    Style st;

    // Widths are authored as a percentage of frame height so a design survives a
    // change of timeline resolution. Feather is the deliberate exception: it is
    // the antialiasing softness, and one screen pixel should stay one screen
    // pixel at any resolution, so it scales with renderScale (proxy) only.
    const double pctToPx = frameHeight / 100.0;

    st.halfWidth = static_cast<float>(DoubleAt(_thickness, t) * pctToPx * 0.5);
    st.feather   = static_cast<float>(DoubleAt(_feather, t) * renderScale);
    st.color     = ColorAt(_color, t);
    st.opacity   = static_cast<float>(Clamp(static_cast<float>(DoubleAt(_opacity, t)), 0.0f, 1.0f));
    st.cap       = ChoiceAt(_cap, t, kCapRound, kCapSquare);

    st.widthStart = static_cast<float>(DoubleAt(_taperStart, t));
    st.widthMid   = static_cast<float>(DoubleAt(_taperBody, t));
    st.widthEnd   = static_cast<float>(DoubleAt(_taperEnd, t));

    st.outline      = BoolAt(_outline, t);
    st.outlineWidth = static_cast<float>(DoubleAt(_outlineWidth, t) * pctToPx);
    st.outlineColor = ColorAt(_outlineColor, t);

    st.glow          = BoolAt(_glow, t);
    st.glowRadius    = static_cast<float>(DoubleAt(_glowRadius, t) * pctToPx);
    st.glowIntensity = static_cast<float>(DoubleAt(_glowIntensity, t));
    st.glowColor     = ColorAt(_glowColor, t);

    st.fill        = BoolAt(_fill, t);
    st.fillColor   = ColorAt(_fillColor, t);
    st.fillOpacity = static_cast<float>(DoubleAt(_fillOpacity, t));

    st.shadow = BoolAt(_shadow, t);
    {
        double ox = 0, oy = 0;
        _shadowOffset->getValueAtTime(t, ox, oy);
        // Both axes scale by HEIGHT, not by their own axis. Scaling x by width
        // would make a 45-degree shadow stop being 45 degrees the moment the
        // frame stopped being square.
        st.shadowDx = static_cast<float>(ox * pctToPx);
        st.shadowDy = static_cast<float>(oy * pctToPx);
    }
    st.shadowSoft    = static_cast<float>(DoubleAt(_shadowSoft, t) * pctToPx);
    st.shadowOpacity = static_cast<float>(DoubleAt(_shadowOpacity, t));
    st.shadowColor   = ColorAt(_shadowColor, t);

    st.arrowStart  = BoolAt(_arrowStart, t);
    st.arrowEnd    = BoolAt(_arrowEnd, t);
    st.arrowLength = static_cast<float>(DoubleAt(_arrowLength, t) * pctToPx);
    st.arrowWidth  = static_cast<float>(DoubleAt(_arrowWidth, t) * pctToPx);
    st.arrowRound  = static_cast<float>(DoubleAt(_arrowRound, t) * pctToPx);
    // A ratio, not a length: it does not scale with the frame.
    st.arrowPosition = static_cast<float>(DoubleAt(_arrowPos, t));

    st.gradMode  = ChoiceAt(_gradMode, t, kGradNone, kGradCentre);
    st.gradColor = ColorAt(_gradColor, t);
    st.gradStart = static_cast<float>(DoubleAt(_gradStart, t));
    st.gradEnd   = static_cast<float>(DoubleAt(_gradEnd, t));
    st.gradMid   = static_cast<float>(DoubleAt(_gradMid, t));
    // Only a request at this point: whether it applies depends on the stroke
    // being closed, which is per-stroke and so decided in buildScene.
    st.gradLoop  = BoolAt(_gradLoop, t);

    return st;
}

Stage BetterLinePlugin::stageAt(int i, double t) const
{
    Stage s;
    s.enabled    = BoolAt(_stEnable[i], t);
    s.target     = ChoiceAt(_stTarget[i], t, 0, kTargetCount - 1);
    s.from       = static_cast<float>(DoubleAt(_stFrom[i], t));
    s.to         = static_cast<float>(DoubleAt(_stTo[i], t));
    s.anchor     = ChoiceAt(_stAnchor[i], t, 0, kAnchorCount - 1);
    s.startFrame = static_cast<float>(DoubleAt(_stStart[i], t));
    s.duration   = static_cast<float>(DoubleAt(_stDuration[i], t));

    // Always built from the four controls, never from the preset index. The
    // preset is a way of WRITING those controls, not a second source of truth --
    // reading it here would mean a curve that had been hand-adjusted still
    // rendered as whatever preset it started from.
    s.easing = MakeEasing(static_cast<float>(DoubleAt(_stEaseIn[i], t)),
                          static_cast<float>(DoubleAt(_stEaseOut[i], t)),
                          static_cast<float>(DoubleAt(_stAntic[i], t)),
                          static_cast<float>(DoubleAt(_stOver[i], t)));
    return s;
}

void BetterLinePlugin::buildScene(double t, const OfxRectI& rod, double renderScale,
                                  std::vector<Segment>& segs, std::vector<FillEdge>& fill,
                                  std::vector<Style>& styles) const
{
    // Separated from render so a slow SETUP (hundreds of parameter reads,
    // flattening, binning) is distinguishable from slow SHADING.
    bl::ProbeScope _t("buildScene");

    const double w = static_cast<double>(rod.x2 - rod.x1);
    const double h = static_cast<double>(rod.y2 - rod.y1);

    // Index 0 always exists and is the un-staggered style, so the shading pass
    // always has something to fall back on even when nothing is drawn.
    styles.clear();
    styles.push_back(Style());
    if (w <= 0.0 || h <= 0.0) return;

    const double pctToPx = h / 100.0;

    const Style style = styleAt(t, h, renderScale);
    styles[0] = style;

    // The base values, before any stage touches them. Read once.
    TrimState baseTrim;
    baseTrim.start  = static_cast<float>(DoubleAt(_trimStart, t));
    baseTrim.end    = static_cast<float>(DoubleAt(_trimEnd, t));
    baseTrim.offset = static_cast<float>(DoubleAt(_trimOffset, t));
    baseTrim.wrap   = BoolAt(_trimWrap, t);

    const float baseThickPct = static_cast<float>(DoubleAt(_thickness, t));
    const float baseDashPct  = static_cast<float>(DoubleAt(_dashPhase, t));

    const bool  animOn  = BoolAt(_animEnable, t);
    const Stage sIn     = stageAt(0, t);
    const Stage sOut    = stageAt(1, t);

    ClipTiming ct;
    if (animOn)
    {
        ct = GetClipTiming(const_cast<BetterLinePlugin*>(this), t,
                           ChoiceAt(_timeOrigin, t, 0, kOriginCount - 1),
                           DoubleAt(_timeOffset, t),
                           DoubleAt(_clipLength, t));

        if (ct.suspect)
        {
            bl::ProbeOnce("suspecttime",
                "anim: the resolved clip frame is far too large to be a frame within a "
                "clip, so every stage will clamp instantly and the line will look inert. "
                "Set Time Origin to 'Timeline Absolute', or set Time Offset by hand.");
        }

        // Say so rather than just doing nothing. A stage that silently declines
        // to run is indistinguishable from a broken effect, and "no usable clip
        // length" has a one-field fix the user can apply.
        if ((sIn.enabled  && !StageUsable(sIn,  static_cast<float>(ct.length))) ||
            (sOut.enabled && !StageUsable(sOut, static_cast<float>(ct.length))))
        {
            bl::ProbeOnce("nolength",
                "anim: a stage anchored to Clip End or Stretch was SKIPPED because the "
                "host reported no usable clip length (adjustment clips do this). "
                "Set the Clip Length parameter by hand to re-enable it.");
        }
    }

    const bool  dashOn  = BoolAt(_dash, t);
    const float dashLen = dashOn ? static_cast<float>(DoubleAt(_dashLen, t) * h / 100.0) : 0.0f;
    const float dashGap = dashOn ? static_cast<float>(DoubleAt(_dashGap, t) * h / 100.0) : 0.0f;

    const float staggerFrames = static_cast<float>(DoubleAt(_stagger, t));
    const int   staggerOrder  = ChoiceAt(_staggerOrder, t, 0, kOrderCount - 1);
    int staggerSeed = 1;
    _staggerSeed->getValue(staggerSeed);

    const bool  wiggleOn    = BoolAt(_wiggle, t);
    const float wiggleAmp   = static_cast<float>(DoubleAt(_wiggleAmp, t) * pctToPx);
    const float wiggleWave  = static_cast<float>(DoubleAt(_wiggleWave, t) * pctToPx);
    const float wigglePhase = static_cast<float>(DoubleAt(_wigglePhase, t));
    int wiggleSeed = 1;
    _wiggleSeed->getValue(wiggleSeed);

    // Built in PIXEL space. Normalised units are not square on a non-square
    // frame, so rotating there shears the shape instead of turning it -- and
    // differently at every timeline resolution.
    Xform2D xf;
    {
        double ax = 0.5, ay = 0.5, ox = 0.0, oy = 0.0;
        _xfAnchor->getValueAtTime(t, ax, ay);
        _xfPos->getValueAtTime(t, ox, oy);

        // Linked simply means Scale Y is not read. Nothing is written anywhere,
        // so the two can never drift out of step.
        const double sx = DoubleAt(_xfScale, t);
        const double sy = BoolAt(_xfLink, t) ? sx : DoubleAt(_xfScaleY, t);

        xf = MakeXform(Vec2(static_cast<float>(rod.x1 + ax * w),
                            static_cast<float>(rod.y1 + ay * h)),
                       Vec2(static_cast<float>(ox * w), static_cast<float>(oy * h)),
                       static_cast<float>(DoubleAt(_xfRot, t)),
                       static_cast<float>(sx), static_cast<float>(sy));
    }

    int strokes = 1;
    _strokeCount->getValueAtTime(t, strokes);
    strokes = strokes < 1 ? 1 : (strokes > kMaxStrokes ? kMaxStrokes : strokes);

    // Flatten to a fixed fraction of a pixel. Scaled by renderScale so a proxy
    // render does proportionally less subdivision instead of the same amount at
    // a quarter of the visible resolution.
    const float tol = static_cast<float>(0.2 * (renderScale > 0.0 ? renderScale : 1.0));

    // Imported geometry, parsed once and cached against the string it came from.
    const std::vector<Stroke>* imported = nullptr;
    if (BoolAt(_importShow, t))
    {
        std::string blob;
        _pathBlob->getValue(blob);
        if (!blob.empty())
        {
            if (blob != _blobCache)
            {
                BlobToStrokes(blob, _blobStrokes);
                _blobCache = blob;
            }
            if (!_blobStrokes.empty()) imported = &_blobStrokes;
        }
        else if (!_blobCache.empty())
        {
            _blobCache.clear();
            _blobStrokes.clear();
        }
    }

    const int importedCount = imported ? static_cast<int>(imported->size()) : 0;
    const int totalStrokes  = strokes + importedCount;

    const bool  useImportCol = BoolAt(_importCol, t);
    const bool  useImportWid = BoolAt(_importWid, t);
    const float importScale  = static_cast<float>(DoubleAt(_importScale, t));

    Polyline poly;
    for (int s = 0; s < totalStrokes; ++s)
    {
        const bool isImported = (s >= strokes);
        if (!isImported && !BoolAt(_sEnable[s], t)) continue;

        Stroke stroke;

        if (isImported)
        {
            // Blob coordinates are SQUARE units: x is scaled by the frame HEIGHT
            // and centred, so an imported circle stays circular. Slot points use
            // width for x, which is right for hand-placed geometry and wrong for
            // artwork that has a shape of its own.
            const Stroke& src = (*imported)[static_cast<size_t>(s - strokes)];
            stroke.closed = src.closed;
            stroke.pts.reserve(src.pts.size());

            for (const ControlPoint& in : src.pts)
            {
                ControlPoint cp;
                cp.pos = BlobToPixels(in.pos, static_cast<float>(rod.x1),
                                      static_cast<float>(rod.y1),
                                      static_cast<float>(w), static_cast<float>(h));
                cp.tan   = BlobVectorToPixels(in.tan, static_cast<float>(h));
                cp.tanIn = BlobVectorToPixels(in.tanIn, static_cast<float>(h));
                cp.hasIn = in.hasIn;

                if (!xf.IsIdentity())
                {
                    cp.pos   = xf.Apply(cp.pos);
                    cp.tan   = xf.ApplyVector(cp.tan);
                    cp.tanIn = xf.ApplyVector(cp.tanIn);
                }
                stroke.pts.push_back(cp);
            }
        }
        else
        {
        int n = 2;
        _sCount[s]->getValueAtTime(t, n);
        n = n < 2 ? 2 : (n > kMaxPoints ? kMaxPoints : n);

        stroke.closed = BoolAt(_sClosed[s], t);
        stroke.pts.reserve(static_cast<size_t>(n));

        for (int i = 0; i < n; ++i)
        {
            double px = 0, py = 0, tx = 0, ty = 0;
            if (OFX::Double2DParam* p = posParam(s, i)) p->getValueAtTime(t, px, py);
            if (OFX::Double2DParam* q = tanParam(s, i)) q->getValueAtTime(t, tx, ty);

            ControlPoint cp;
            cp.pos = Vec2(static_cast<float>(rod.x1 + px * w),
                          static_cast<float>(rod.y1 + py * h));
            // Handles are normalised offsets, so they scale with the frame the
            // same way positions do; using the height for both axes would skew
            // every curve on a non-square frame.
            cp.tan = Vec2(static_cast<float>(tx * w), static_cast<float>(ty * h));

            // A handle is an offset from its point, so it rotates and scales
            // with the shape but must NOT be translated -- applying the position
            // to it as well would double every handle's length.
            if (!xf.IsIdentity())
            {
                cp.pos = xf.Apply(cp.pos);
                cp.tan = xf.ApplyVector(cp.tan);
            }

            stroke.pts.push_back(cp);
        }
        }

        FlattenStroke(stroke, tol, poly);

        // Wiggle displaces the flattened path, so trim, dashes, fill and the
        // arrowhead tangents all follow the wobble rather than the ideal line.
        if (wiggleOn && wiggleAmp > 1e-4f)
        {
            Polyline wobbled;
            // Seeded per stroke, or every stroke in a shape would wobble in
            // lockstep and read as one rigid squiggle rather than several
            // independently drawn lines.
            WigglePolyline(poly, wiggleAmp, wiggleWave, wigglePhase,
                           static_cast<unsigned>(wiggleSeed) + static_cast<unsigned>(s) * 7919u,
                           wobbled);
            poly = wobbled;
        }

        // Each stroke gets its own copy of the style and trim, evaluated at its
        // own point in the animation. This is what stagger IS -- without a style
        // per stroke, an animated opacity or thickness would be identical across
        // all of them and only the trim would appear to stagger.
        Style     st   = style;
        // The seam only exists on a closed contour, so folding an open path's
        // gradient would just halve its range for no reason.
        st.gradLoop    = style.gradLoop && stroke.closed;
        TrimState trim = baseTrim;
        float     dashPhasePx = baseDashPct * static_cast<float>(pctToPx);
        float     thickPct    = baseThickPct;

        // An imported path carries the paint it was drawn with. Applied BEFORE
        // the animation stages, so a stage driving Thickness or Opacity still
        // acts on what the artwork asked for rather than replacing it.
        if (isImported)
        {
            const StrokePaint& k = (*imported)[static_cast<size_t>(s - strokes)].paint;

            if (useImportCol)
            {
                if (k.hasStroke) st.color     = k.strokeColor;
                if (k.hasFill)   st.fillColor = k.fillColor;
            }

            // The global Fill is a MASTER SWITCH over the per-path flag, not a
            // separate opinion. Ignoring it left a fill-only import permanently
            // filled with no way to turn it into an outline.
            st.fill = style.fill && k.hasFill;

            if (useImportWid)
            {
                // Blob width is a fraction of the frame HEIGHT, and Thickness is
                // a percentage of it, so the conversion is a factor of 100.
                thickPct = k.hasStroke ? (k.width * 100.0f * importScale) : 0.0f;
            }
            else
            {
                // Off means "use MY thickness", and that has to include paths
                // the artwork never stroked. Fill-only artwork is extremely
                // common -- most icon sets are drawn that way -- and leaving
                // those at zero meant an import with nothing to draw on, and so
                // nothing for Trim to animate.
                thickPct = baseThickPct;
            }
        }

        AnimTargets av;
        SeedTargets(st, trim, thickPct, baseDashPct, av);

        if (animOn)
        {
            const float delay = staggerFrames *
                static_cast<float>(StaggerIndex(s, totalStrokes, staggerOrder,
                                                static_cast<unsigned>(staggerSeed)));

            ApplyStages(sIn, sOut,
                        static_cast<float>(ct.clipFrame) - delay,
                        static_cast<float>(ct.length), av.v);
        }

        WriteTargets(av, static_cast<float>(pctToPx), st, trim, dashPhasePx);

        // Contours from the same imported element share one style entry, so the
        // fill pass treats them as one shape and their windings combine.
        int styleIdx;
        if (isImported && s > strokes &&
            (*imported)[static_cast<size_t>(s - strokes)].paint.group ==
            (*imported)[static_cast<size_t>(s - strokes - 1)].paint.group)
        {
            styleIdx = static_cast<int>(styles.size()) - 1;
        }
        else
        {
            styleIdx = static_cast<int>(styles.size());
            styles.push_back(st);
        }

        // Fill takes the WHOLE contour, before trim and dashes touch it. A trim
        // is a reveal of the line; a shape whose fill came and went with it would
        // be a different shape every frame rather than one being drawn.
        //
        // `closed` gates a HAND-DRAWN stroke, where it is the user's own toggle
        // and "Fill Closed Paths" says so on the label. It must NOT gate an
        // import: SVG fills implicitly close every subpath, Z or not, so there
        // the fill attribute is the authority and `st.fill` already carries it.
        // A cat drawn by Inkscape lost its nose to this -- one filled subpath
        // among six stroked ones, written without a closing Z because filling it
        // never needed one.
        if (st.fill && (isImported || stroke.closed))
            AppendFillContour(poly.pts, styleIdx, fill);

        const size_t firstSeg = segs.size();
        BuildStrokeSegments(poly, st, trim, stroke.closed,
                            dashLen, dashGap, dashOn ? dashPhasePx : 0.0f, segs);
        for (size_t k = firstSeg; k < segs.size(); ++k) segs[k].style = styleIdx;
    }
}

void BetterLinePlugin::render(const OFX::RenderArguments& p_Args)
{
    bl::ProbeScope _t("render");

    std::unique_ptr<OFX::Image> dst(_dstClip->fetchImage(p_Args.time));
    if (!dst.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

    if (dst->getPixelDepth()      != OFX::eBitDepthFloat ||
        dst->getPixelComponents() != OFX::ePixelComponentRGBA)
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }

    std::unique_ptr<OFX::Image> src(
        (_srcClip && _srcClip->isConnected()) ? _srcClip->fetchImage(p_Args.time) : nullptr);

    // Built once, not once per frame. Concatenating three std::strings on every
    // render to hand them to a function that will discard them is not free at
    // 8 million pixels' worth of budget.
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            bl::ProbeOnce("dispatch",
                          std::string("render dispatch: cuda=") +
                          (p_Args.isEnabledCudaRender ? "yes" : "no") +
                          " opencl=" + (p_Args.isEnabledOpenCLRender ? "yes" : "no") +
                          "   <- if cuda=no, every frame is running on the CPU and that "
                          "dwarfs every other performance consideration");
        }
    }

    const OfxRectI rod = dst->getBounds();

    std::vector<Segment>  segs;
    std::vector<FillEdge> fill;
    std::vector<Style>    styles;
    buildScene(p_Args.time, rod, p_Args.renderScale.x, segs, fill, styles);

    // The MAXIMUM reach across every stroke's style. Binning against one
    // stroke's reach would clip whichever other stroke happened to glow further.
    float reach = 0.0f;
    float feather = 0.0f;
    for (const Style& s : styles)
    {
        reach   = std::max(reach, s.MaxReach());
        feather = std::max(feather, s.feather);
    }

    // A reach that under-reports is the bug where a glow is sliced off along
    // invisible 16-pixel boundaries, so the +1 covers the filter footprint on
    // top of whatever the style itself claims to need.
    TileGrid grid;
    BuildTileGrid(segs, reach + 1.0f,
                  p_Args.renderWindow.x1, p_Args.renderWindow.y1,
                  p_Args.renderWindow.x2, p_Args.renderWindow.y2, 16, grid);

    // The band margin only has to cover the fill's own soft edge; unlike the
    // tile grid it is not carrying glow or shadow, which sit outside the shape
    // and are the stroke's business.
    BandGrid bands;
    BuildBandGrid(fill, feather + 1.0f,
                  p_Args.renderWindow.y1, p_Args.renderWindow.y2, 16, bands);

    SceneView scene;
    scene.segs       = segs.empty() ? nullptr : segs.data();
    scene.segCount   = static_cast<int>(segs.size());
    scene.grid       = grid.View();
    scene.fill       = fill.empty() ? nullptr : fill.data();
    scene.fillCount  = static_cast<int>(fill.size());
    scene.bands      = bands.View();
    scene.styles     = styles.data();
    scene.styleCount = static_cast<int>(styles.size());

    LineProcessor processor(*this);
    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());
    processor.setScene(scene);

#ifdef BETTERLINE_HAVE_CUDA
    CudaSceneDesc upload;
    upload.segs           = scene.segs;
    upload.segCount       = scene.segCount;
    upload.tileStart      = grid.start.empty() ? nullptr : grid.start.data();
    upload.tileStartCount = static_cast<int>(grid.start.size());
    upload.tileItems      = grid.items.empty() ? nullptr : grid.items.data();
    upload.tileItemCount  = static_cast<int>(grid.items.size());
    upload.gridMeta       = grid.View();
    upload.fill           = scene.fill;
    upload.fillCount      = scene.fillCount;
    upload.bandStart      = bands.start.empty() ? nullptr : bands.start.data();
    upload.bandStartCount = static_cast<int>(bands.start.size());
    upload.bandItems      = bands.items.empty() ? nullptr : bands.items.data();
    upload.bandItemCount  = static_cast<int>(bands.items.size());
    upload.bandMeta       = bands.View();
    upload.styles         = styles.data();
    upload.styleCount     = static_cast<int>(styles.size());
    processor.setUpload(upload);
#endif

    processor.setGPURenderArgs(p_Args);
    processor.setRenderWindow(p_Args.renderWindow);
    processor.process();
}

} // namespace

// =============================================================================
// Factory
// =============================================================================

BetterLinePluginFactory::BetterLinePluginFactory()
    : OFX::PluginFactoryHelper<BetterLinePluginFactory>(kPluginIdentifier,
                                                        kPluginVersionMajor,
                                                        kPluginVersionMinor)
{
}

void BetterLinePluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(OFX::eContextFilter);
    p_Desc.addSupportedContext(OFX::eContextGeneral);
    p_Desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(false);

    // Tiling and multi-resolution are off so the source and destination images
    // always share bounds, which is what lets the rasterizer treat a pixel
    // address as a position in the frame with no further mapping.
    p_Desc.setSupportsTiles(false);
    p_Desc.setSupportsMultiResolution(false);

    // In describe(), NOT describeInContext(): the overlay descriptor is a
    // plugin-level property and the host caches the descriptor, so it would not
    // necessarily be picked up from the per-context pass.
    bl::RegisterOverlay(p_Desc);

#ifdef BETTERLINE_HAVE_CUDA
    // Declared only when a device actually exists. Claiming CUDA support on a
    // machine with no GPU would have the host hand us device pointers we cannot
    // service, and processImagesCUDA has no way back to the CPU loop once that
    // has happened -- the pixel addresses are already device memory.
    if (bl::CudaAvailable())
    {
        p_Desc.setSupportsCudaRender(true);
        p_Desc.setSupportsCudaStream(true);
    }
#endif
}

namespace {

using namespace bl;

/** @brief Every value-bearing parameter goes through here.
 *
 * The OFX default, eCacheInvalidateValueChange, means "invalidate only the
 * frames this parameter's keyframe covers". That is right for a purely keyframed
 * effect and wrong here: the trim window reinterprets the geometry at every
 * frame, so a change to any parameter can change any frame. Resolve tolerates
 * the default; Fusion does not, and Fusion is the one behaving correctly.
 */
/// Every parameter this plugin defines passes through the helpers below, so this
/// is the real total rather than an estimate from reading the source. Logged at
/// the end of describeInContext: the count is the single number that decides
/// whether creating an instance is cheap, and guessing at it is how it got out
/// of hand in the first place.
int g_paramCount = 0;

// Which registry kind a descriptor corresponds to. Non-template overloads beat
// the catch-all, so anything without one -- groups, pages, push buttons -- is
// correctly recorded as having no value to save.
template <class T> int KindOf(T*)                            { return kPKNone; }
inline int KindOf(OFX::DoubleParamDescriptor*)               { return kPKDouble; }
inline int KindOf(OFX::Double2DParamDescriptor*)             { return kPKDouble2D; }
inline int KindOf(OFX::IntParamDescriptor*)                  { return kPKInt; }
inline int KindOf(OFX::BooleanParamDescriptor*)              { return kPKBool; }
inline int KindOf(OFX::ChoiceParamDescriptor*)               { return kPKChoice; }
inline int KindOf(OFX::RGBAParamDescriptor*)                 { return kPKRGBA; }
inline int KindOf(OFX::StringParamDescriptor*)               { return kPKString; }

template <class T> T* AddParam(OFX::PageParamDescriptor* page, T* p)
{
    p->setCacheInvalidation(OFX::eCacheInvalidateValueAll);
    page->addChild(*p);
    ++g_paramCount;
    // Registered at the point of definition, so the preset system covers every
    // parameter automatically -- including ones added later, which is the whole
    // reason for doing it here rather than in a hand-written list.
    RegisterParam(p->getName(), KindOf(p));
    return p;
}

/** @brief For parameters that change nothing about the rendered image.
 *
 * Overlay toggles and editing aids. Declaring that these invalidate the cache
 * would throw away every rendered frame to show a widget, which is a real cost
 * for no pixels.
 */
template <class T> T* AddParamUI(OFX::PageParamDescriptor* page, T* p)
{
    page->addChild(*p);
    ++g_paramCount;
    return p;
}

OFX::GroupParamDescriptor* DefineGroup(OFX::ImageEffectDescriptor& desc,
                                       OFX::PageParamDescriptor* page,
                                       const std::string& name, const std::string& label)
{
    OFX::GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    // Collapsed by default: with eight strokes of sixteen points there are
    // several hundred rows, and an Inspector that opens fully expanded is not a
    // panel, it is a wall.
    g->setOpen(false);
    page->addChild(*g);
    ++g_paramCount;   // a group is a parameter too, as far as the host is concerned
    return g;
}

OFX::DoubleParamDescriptor* DefineDouble(OFX::ImageEffectDescriptor& desc,
                                         OFX::PageParamDescriptor* page,
                                         OFX::GroupParamDescriptor* parent,
                                         const std::string& name, const std::string& label,
                                         const std::string& hint,
                                         double def, double lo, double hi,
                                         double dlo, double dhi)
{
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dlo, dhi);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::RGBAParamDescriptor* DefineColor(OFX::ImageEffectDescriptor& desc,
                                      OFX::PageParamDescriptor* page,
                                      OFX::GroupParamDescriptor* parent,
                                      const std::string& name, const std::string& label,
                                      double r, double g, double b, double a)
{
    OFX::RGBAParamDescriptor* p = desc.defineRGBAParam(name);
    p->setLabels(label, label, label);
    p->setDefault(r, g, b, a);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::BooleanParamDescriptor* DefineBool(OFX::ImageEffectDescriptor& desc,
                                        OFX::PageParamDescriptor* page,
                                        OFX::GroupParamDescriptor* parent,
                                        const std::string& name, const std::string& label,
                                        const std::string& hint, bool def)
{
    OFX::BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

} // namespace

void BetterLinePluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc,
                                                OFX::ContextEnum /*p_Context*/)
{
    OFX::ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(false);
    srcClip->setIsMask(false);
    srcClip->setOptional(true);

    OFX::ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    dstClip->setSupportsTiles(false);

    // Resolve reports maxPages = 0 -- it advertises no pages at all, so there is
    // no tab mechanism to be had. One page, groups for structure.
    OFX::PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // Ungrouped parameters must ALL be defined before the first group. Resolve
    // lays the panel out in definition order, and an ungrouped parameter that
    // sits between two groups gets scattered to the other end of the panel.
    {
        OFX::IntParamDescriptor* p = p_Desc.defineIntParam(kParamStrokeCount);
        p->setLabels("Strokes", "Strokes", "Strokes");
        p->setHint("How many separate paths this instance draws.");
        p->setDefault(1);
        p->setRange(1, kMaxStrokes);
        p->setDisplayRange(1, kMaxStrokes);
        // Topology does not animate. Point POSITIONS are keyframable and that is
        // the point of the design, but how many points exist is structure: the
        // overlay's insert and delete shift whole animation curves between
        // slots, and a count that differed frame to frame would make "which
        // slots hold real points" unanswerable.
        p->setAnimates(false);
        AddParam(page, p);
    }

    // --- presets -------------------------------------------------------------
    // First, because a preset is the thing you reach for before anything else.
    // None of these are themselves saved into a preset -- they go through
    // AddParamUI, which does not register. A preset recording where presets live
    // would overwrite that setting on every load.
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "presetGroup", "Presets");

        {
            OFX::StringParamDescriptor* p = p_Desc.defineStringParam(kParamPresetName);
            p->setLabels("Preset Name", "Preset Name", "Preset Name");
            p->setHint("Offered as the file name when saving.");
            p->setStringType(OFX::eStringTypeSingleLine);
            p->setDefault("");
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamPresetSave);
            p->setLabels("Save Preset...", "Save Preset...", "Save Preset...");
            p->setHint("Writes every setting to a .json file.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamPresetLoad);
            p->setLabels("Load Preset...", "Load Preset...", "Load Preset...");
            p->setHint("Applies every setting in the file. Anything the file does not "
                       "mention is returned to its default, so loading the same preset "
                       "always gives the same result whatever was set before.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            // Read-out, not an input. Resolve draws a value field beside a string
            // parameter whether or not one makes sense, so it is greyed out --
            // an editable-looking path that is ignored is worse than no path.
            OFX::StringParamDescriptor* p = p_Desc.defineStringParam(kParamPresetFolder);
            p->setLabels("Folder", "Folder", "Folder");
            p->setStringType(OFX::eStringTypeSingleLine);
            p->setDefault("");
            p->setEnabled(false);
            p->setAnimates(false);
            p->setIsPersistant(false);   // recomputed on load; it is not project data
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamPresetSetDir);
            p->setLabels("Set Folder...", "Set Folder...", "Set Folder...");
            p->setHint("Where the preset dialogs open. Stored per machine, outside the "
                       "project, because it belongs to you rather than to this timeline.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamPresetReset);
            p->setLabels("Reset All", "Reset All", "Reset All");
            p->setHint("Returns every setting to its default.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
    }

    // --- shape ---------------------------------------------------------------
    for (int s = 0; s < kMaxStrokes; ++s)
    {
        const std::string label = "Stroke " + std::to_string(s + 1);
        OFX::GroupParamDescriptor* g =
            DefineGroup(p_Desc, page, StrokeParam("group", s), label);
        g->setIsSecret(DefaultStrokeSecret(s));

        // All three are structure, not animation -- see the note on Strokes above.
        {
            OFX::BooleanParamDescriptor* p =
                DefineBool(p_Desc, page, g, StrokeParam(kStrokeEnable, s), "Enabled",
                           "Draw this stroke.", s == 0);
            p->setAnimates(false);
            p->setIsSecret(DefaultStrokeSecret(s));
        }

        {
            OFX::IntParamDescriptor* p = p_Desc.defineIntParam(StrokeParam(kStrokeCount, s));
            p->setLabels("Points", "Points", "Points");
            p->setDefault(2);
            p->setRange(2, kMaxPoints);
            p->setDisplayRange(2, kMaxPoints);
            p->setAnimates(false);
            p->setIsSecret(DefaultStrokeSecret(s));
            p->setParent(*g);
            AddParam(page, p);
        }

        {
            OFX::BooleanParamDescriptor* p =
                DefineBool(p_Desc, page, g, StrokeParam(kStrokeClosed, s), "Closed",
                           "Join the last point back to the first.", false);
            p->setAnimates(false);
            p->setIsSecret(DefaultStrokeSecret(s));
        }

        // Topology (corner/smooth flags) is a STRING, not a parameter per point.
        // The split is the whole design: anything that animates gets a real
        // keyframable slot, anything structural gets one lenient text field. A
        // corner is not something anyone keyframes, so it does not deserve a
        // Double2D of its own multiplied by a hundred and twenty-eight.
        {
            OFX::StringParamDescriptor* p =
                p_Desc.defineStringParam(StrokeParam(kStrokeTopology, s));
            p->setLabels("Corners", "Corners", "Corners");
            p->setHint("Indices of corner points, comma separated. Anything that is "
                       "not a digit separates, so a half-typed entry is harmless.");
            p->setStringType(OFX::eStringTypeSingleLine);
            p->setDefault("");
            p->setAnimates(false);
            p->setParent(*g);
            AddParam(page, p);
        }

        for (int i = 0; i < kMaxPoints; ++i)
        {
            const std::string n = std::to_string(i + 1);

            OFX::Double2DParamDescriptor* pos =
                p_Desc.defineDouble2DParam(PointParam(kPointPos, s, i));
            pos->setLabels("P" + n, "P" + n, "P" + n);
            pos->setHint("Position, normalised: 0,0 is bottom-left and 1,1 is top-right.");
            // eDoubleTypePlain, NOT eDoubleTypeNormalisedXY: the normalised
            // types were deprecated in OFX 1.4 and their interpretation in
            // Resolve is unverified. Plain doubles mean the plugin owns the
            // convention and no host can disagree with it.
            pos->setDoubleType(OFX::eDoubleTypePlain);
            pos->setDefault(s == 0 ? (i == 0 ? 0.25 : 0.75) : 0.5, 0.5);
            pos->setRange(-10.0, -10.0, 10.0, 10.0);
            pos->setDisplayRange(0.0, 0.0, 1.0, 1.0);
            pos->setIncrement(0.001);
            pos->setIsSecret(DefaultPointSecret(s, i));
            pos->setParent(*g);
            AddParam(page, pos);

            OFX::Double2DParamDescriptor* tan =
                p_Desc.defineDouble2DParam(PointParam(kPointTan, s, i));
            tan->setLabels("P" + n + " Handle", "P" + n + " Handle", "P" + n + " Handle");
            tan->setHint("Outgoing bezier handle, as an offset. Zero makes a corner. "
                         "The incoming handle is its mirror.");
            tan->setDoubleType(OFX::eDoubleTypePlain);
            tan->setDefault(0.0, 0.0);
            tan->setRange(-10.0, -10.0, 10.0, 10.0);
            tan->setDisplayRange(-0.5, -0.5, 0.5, 0.5);
            tan->setIncrement(0.001);
            tan->setIsSecret(DefaultPointSecret(s, i));
            tan->setParent(*g);
            AddParam(page, tan);
        }
    }

    // --- shape generator -----------------------------------------------------
    //
    // Writes into an ordinary stroke slot and then gets out of the way: what it
    // produces is points, keyframable and editable like any others. A shape that
    // stayed "a rectangle" would need its own parameters, its own overlay and its
    // own animation path, and would still be less useful the moment someone
    // wanted to drag one corner.
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "shapeGroup", "Add Shape");

        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamShapeKind);
            p->setLabels("Shape", "Shape", "Shape");
            p->appendOption("Rectangle");
            p->appendOption("Ellipse");
            // No separate Triangle: it is a three-sided polygon, and two controls
            // for one idea is one control too many.
            p->appendOption("Polygon");
            p->setDefault(kShapeRectangle);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::IntParamDescriptor* p = p_Desc.defineIntParam(kParamShapeSides);
            p->setLabels("Sides", "Sides", "Sides");
            p->setHint("Number of sides. Set 3 for a triangle. Rounded corners cost two "
                       "points each, so the maximum halves once Corner Radius leaves zero.");
            p->setDefault(3);
            p->setRange(3, kMaxPoints);
            p->setDisplayRange(3, kMaxPoints);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::DoubleParamDescriptor* p = p_Desc.defineDoubleParam(kParamShapeCorner);
            p->setLabels("Corner Radius", "Corner Radius", "Corner Radius");
            // Relative, not absolute: the whole point of the generator is that the
            // shape gets resized afterwards by Transform, and an absolute radius
            // would swallow a shape that was later scaled down.
            p->setHint("Corner rounding as a percentage of the shape's inscribed radius, "
                       "so 100% is as round as the shape gets and a square becomes a "
                       "circle. Relative, so it survives being scaled afterwards. "
                       "Ignored by Ellipse.");
            p->setDefault(0.0);
            p->setRange(0.0, 100.0);
            p->setDisplayRange(0.0, 100.0);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        // Two axes, not one Size plus Transform Scale. A non-uniform Transform
        // scale stretches the corner fillets from circular arcs into elliptical
        // ones, so a wide rounded rectangle cannot be made by squashing a square
        // one -- the proportions have to exist before the corners are rounded.
        // Both are a percentage of frame HEIGHT, so equal values are a true
        // square on any timeline.
        {
            OFX::DoubleParamDescriptor* p = p_Desc.defineDoubleParam(kParamShapeWidth);
            p->setLabels("Width", "Width", "Width");
            p->setHint("Half-width as a percentage of frame height. Set equal to Height "
                       "for a square or a circle.");
            p->setDefault(40.0);
            p->setRange(1.0, 400.0);
            p->setDisplayRange(5.0, 120.0);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::DoubleParamDescriptor* p = p_Desc.defineDoubleParam(kParamShapeSize);
            p->setLabels("Height", "Height", "Height");
            p->setHint("Half-height as a percentage of frame height.");
            p->setDefault(40.0);
            p->setRange(1.0, 400.0);
            p->setDisplayRange(5.0, 120.0);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamShapeSlot);
            p->setLabels("Into", "Into", "Into");
            p->setHint("Which stroke to write. Next Free picks the first stroke that is "
                       "switched off, so adding shapes never overwrites your work.");
            p->appendOption("Next Free");
            for (int s = 0; s < kMaxStrokes; ++s)
                p->appendOption("Stroke " + std::to_string(s + 1));
            p->setDefault(0);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamShapeAdd);
            p->setLabels("Add Shape", "Add Shape", "Add Shape");
            p->setHint("Writes the shape into a stroke as ordinary control points. "
                       "Clears any keyframes on that stroke's points first.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
    }

    // --- imported paths ------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "importGroup", "Import");

        DefineBool(p_Desc, page, g, kParamImportShow, "Show Imported Path",
                   "Draw the imported geometry. Turn off to work on the hand-drawn "
                   "strokes without deleting the import.", true);

        // Escapes back to the global style. Without these, importing artwork
        // that carries its own colour would silently disconnect the Colour and
        // Thickness controls from it, and "why can't I recolour my logo" is a
        // worse problem than the silhouette this replaces.
        DefineBool(p_Desc, page, g, kParamImportCol, "Use Imported Colours",
                   "On, each imported path keeps the colour it was drawn with. Off, the "
                   "whole import takes the global Colour.", true);
        DefineBool(p_Desc, page, g, kParamImportWid, "Use Imported Widths",
                   "On, each imported path keeps the stroke width it was drawn with, "
                   "and a path drawn with no stroke gets none. Off, EVERY imported path "
                   "takes the global Thickness -- which is how fill-only artwork gets "
                   "an outline for Trim to draw on.", true);
        DefineDouble(p_Desc, page, g, kParamImportScale, "Imported Width Scale",
                     "Multiplies every imported stroke width, so a logo can be made "
                     "heavier or finer without losing its relative line weights.",
                     1.0, 0.0, 100.0, 0.0, 4.0);

        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamImportSvg);
            p->setLabels("Import SVG...", "Import SVG...", "Import SVG...");
            p->setHint("Reads path geometry with its flat fill and stroke paint. "
                       "Gradients, filters and text are ignored. Imported paths have no "
                       "point limit, but are edited as a whole through Transform and "
                       "Trim rather than point by point.");
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::PushButtonParamDescriptor* p = p_Desc.definePushButtonParam(kParamClearImport);
            p->setLabels("Clear Import", "Clear Import", "Clear Import");
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::StringParamDescriptor* p = p_Desc.defineStringParam(kParamImportInfo);
            p->setLabels("Imported", "Imported", "Imported");
            p->setStringType(OFX::eStringTypeSingleLine);
            p->setDefault("nothing imported");
            p->setEnabled(false);
            p->setIsPersistant(false);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            // The geometry itself. Registered normally, so it travels in presets
            // and in the project like any other value -- it IS the artwork.
            //
            // Not animatable: it is topology, and a shape that changed its point
            // count between frames would make every arc-length derived feature
            // (trim, dashes, gradients) mean something different each frame.
            OFX::StringParamDescriptor* p = p_Desc.defineStringParam(kParamPathBlob);
            p->setLabels("Path Data", "Path Data", "Path Data");
            p->setHint("Imported geometry. Editable by hand, but it is normally written "
                       "by Import SVG.");
            p->setStringType(OFX::eStringTypeSingleLine);
            p->setDefault("");
            p->setAnimates(false);
            p->setParent(*g);
            AddParam(page, p);
        }
    }

    // --- global transform ----------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "xformGroup", "Transform");

        auto xy = [&](const char* name, const char* label, const char* hint,
                      double dx, double dy, double lo, double hi) {
            OFX::Double2DParamDescriptor* p = p_Desc.defineDouble2DParam(name);
            p->setLabels(label, label, label);
            p->setHint(hint);
            p->setDoubleType(OFX::eDoubleTypePlain);
            p->setDefault(dx, dy);
            p->setRange(-1000.0, -1000.0, 1000.0, 1000.0);
            p->setDisplayRange(lo, lo, hi, hi);
            p->setIncrement(0.001);
            p->setParent(*g);
            AddParam(page, p);
        };

        xy(kParamXfPos, "Position",
           "Moves every stroke together, as a fraction of the frame. Keyframe it to "
           "fly the whole shape around without touching a single point.",
           0.0, 0.0, -1.0, 1.0);

        DefineDouble(p_Desc, page, g, kParamXfRot, "Rotation",
                     "Degrees, about the Anchor.", 0.0, -100000.0, 100000.0, -180.0, 180.0);

        // Two separate scalars rather than one Double2D with the axes mirrored.
        //
        // Mirroring meant writing the other axis back through the host on every
        // change: a round trip you can watch happen, and one that loses track if
        // the host coalesces events -- leaving the two stuck out of step. There
        // is nothing to write here. When linked, Scale Y is simply not read, and
        // it is hidden so the panel cannot show a number that is not in force.
        DefineDouble(p_Desc, page, g, kParamXfScale, "Scale",
                     "About the Anchor.", 1.0, -1000.0, 1000.0, 0.0, 3.0);
        DefineDouble(p_Desc, page, g, kParamXfScaleY, "Scale Y",
                     "Only used when Link Scale is off.", 1.0, -1000.0, 1000.0, 0.0, 3.0);

        DefineBool(p_Desc, page, g, kParamXfLink, "Link Scale",
                   "Scales proportionally from the one Scale value. Turn off to stretch "
                   "one axis, which reveals Scale Y.", true);

        xy(kParamXfAnchor, "Anchor",
           "The point rotation and scale pivot around, as a fraction of the frame. "
           "0.5, 0.5 is the centre.", 0.5, 0.5, 0.0, 1.0);
    }

    // --- style ---------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "styleGroup", "Style");

        DefineColor(p_Desc, page, g, kParamColor, "Colour", 1.0, 1.0, 1.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamOpacity, "Opacity",
                     "Scales all four channels, since Resolve works premultiplied.",
                     1.0, 0.0, 1.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamThickness, "Thickness",
                     "Line width as a percentage of frame height, so a design "
                     "authored at 1080p lands identically at UHD.",
                     0.6, 0.0, 100.0, 0.0, 5.0);
        DefineDouble(p_Desc, page, g, kParamFeather, "Softness",
                     "Antialiasing width in pixels. Deliberately in pixels and not "
                     "a percentage: one screen pixel of softness should stay one "
                     "screen pixel at any timeline resolution.",
                     1.0, 0.0, 64.0, 0.0, 8.0);

        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamCap);
            p->setLabels("Caps", "Caps", "Caps");
            p->appendOption("Round");
            p->appendOption("Butt");
            p->appendOption("Square");
            p->setDefault(kCapRound);
            p->setParent(*g);
            AddParam(page, p);
        }

        // Three stops, not two. With only the ends, setting both thin
        // interpolates thin-to-thin and the line vanishes -- so the shape a
        // taper most obviously ought to make, swelling through the body and
        // pointed at both tips, was the one it could not.
        DefineDouble(p_Desc, page, g, kParamTaperStart, "Taper Start",
                     "Width multiplier at the start of the path.", 1.0, 0.0, 8.0, 0.0, 2.0);
        DefineDouble(p_Desc, page, g, kParamTaperBody, "Taper Body",
                     "Width multiplier at the MIDDLE of the path. Set Start and End "
                     "to 0 and leave this at 1 for a leaf or brush shape. Set it to "
                     "the average of Start and End for a dead straight taper.",
                     1.0, 0.0, 8.0, 0.0, 2.0);
        DefineDouble(p_Desc, page, g, kParamTaperEnd, "Taper End",
                     "Width multiplier at the end of the path.", 1.0, 0.0, 8.0, 0.0, 2.0);
    }

    // --- outline -------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "outlineGroup", "Outline");
        DefineBool(p_Desc, page, g, kParamOutline, "Outline",
                   "A second stroke behind the first, for readability over footage.", false);
        DefineDouble(p_Desc, page, g, kParamOutlineWidth, "Outline Width",
                     "Added outside the line, as a percentage of frame height.",
                     0.2, 0.0, 100.0, 0.0, 2.0);
        DefineColor(p_Desc, page, g, kParamOutlineColor, "Outline Colour", 0.0, 0.0, 0.0, 1.0);
    }

    // --- glow ----------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "glowGroup", "Glow");
        DefineBool(p_Desc, page, g, kParamGlow, "Glow",
                   "A distance falloff rather than a blur, so it costs no extra pass "
                   "and is never clipped at a tile edge.", false);
        DefineDouble(p_Desc, page, g, kParamGlowRadius, "Glow Radius",
                     "Percentage of frame height.", 1.0, 0.0, 100.0, 0.0, 10.0);
        DefineDouble(p_Desc, page, g, kParamGlowIntensity, "Glow Intensity",
                     "", 1.0, 0.0, 4.0, 0.0, 2.0);
        DefineColor(p_Desc, page, g, kParamGlowColor, "Glow Colour", 1.0, 1.0, 1.0, 1.0);
    }

    // --- fill ----------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "fillGroup", "Fill");
        DefineBool(p_Desc, page, g, kParamFill, "Fill Closed Paths",
                   "Fills any stroke marked Closed. Uses the whole contour rather than "
                   "the trimmed or dashed geometry, so a draw-on reveal draws the line "
                   "without the shape flickering in and out behind it.", false);
        DefineColor(p_Desc, page, g, kParamFillColor, "Fill Colour", 0.2, 0.4, 0.9, 1.0);
        DefineDouble(p_Desc, page, g, kParamFillOpacity, "Fill Opacity",
                     "Keyframe this to fade the fill in behind a draw-on line.",
                     1.0, 0.0, 1.0, 0.0, 1.0);
    }

    // --- drop shadow ---------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "shadowGroup", "Drop Shadow");
        DefineBool(p_Desc, page, g, kParamShadow, "Drop Shadow",
                   "Reads the same distance field at an offset position, so it costs "
                   "no blur pass and never clips at a tile edge.", false);
        {
            OFX::Double2DParamDescriptor* p = p_Desc.defineDouble2DParam(kParamShadowOffset);
            p->setLabels("Shadow Offset", "Shadow Offset", "Shadow Offset");
            p->setHint("Percentage of frame height. Positive Y is up.");
            p->setDoubleType(OFX::eDoubleTypePlain);
            p->setDefault(0.4, -0.4);
            p->setRange(-100.0, -100.0, 100.0, 100.0);
            p->setDisplayRange(-5.0, -5.0, 5.0, 5.0);
            p->setIncrement(0.01);
            p->setParent(*g);
            AddParam(page, p);
        }
        DefineDouble(p_Desc, page, g, kParamShadowSoft, "Shadow Softness",
                     "Percentage of frame height.", 0.5, 0.0, 100.0, 0.0, 5.0);
        DefineDouble(p_Desc, page, g, kParamShadowOpacity, "Shadow Opacity",
                     "", 0.6, 0.0, 1.0, 0.0, 1.0);
        DefineColor(p_Desc, page, g, kParamShadowColor, "Shadow Colour", 0.0, 0.0, 0.0, 1.0);
    }

    // --- arrowheads ----------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "arrowGroup", "Arrowheads");
        DefineBool(p_Desc, page, g, kParamArrowEnd, "Arrow at End",
                   "Rides the trimmed end, so it leads a draw-on reveal rather than "
                   "waiting where the line has not reached yet.", false);
        DefineBool(p_Desc, page, g, kParamArrowStart, "Arrow at Start", "", false);
        DefineDouble(p_Desc, page, g, kParamArrowLength, "Arrow Length",
                     "Tip to base, as a percentage of frame height.", 2.0, 0.0, 100.0, 0.0, 10.0);
        DefineDouble(p_Desc, page, g, kParamArrowWidth, "Arrow Width",
                     "Full width at the base, as a percentage of frame height.",
                     1.6, 0.0, 100.0, 0.0, 10.0);
        DefineDouble(p_Desc, page, g, kParamArrowPos, "Arrow Position",
                     "How much of the head sits past the end of the line. "
                     "1 attaches the base to the end so the head continues the line "
                     "(the default), 0.5 centres it on the end, 0 puts the tip on it.",
                     1.0, 0.0, 1.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamArrowRound, "Arrow Roundness",
                     "Corner radius, as a percentage of frame height. The triangle is "
                     "inset to compensate, so rounding softens the corners without "
                     "growing the head.", 0.0, 0.0, 100.0, 0.0, 2.0);
    }

    // --- gradient ------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "gradGroup", "Gradient");
        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamGradMode);
            p->setLabels("Gradient", "Gradient", "Gradient");
            p->appendOption("None");
            p->appendOption("Along Length");
            p->appendOption("Across Width");
            p->appendOption("Centre Out");
            p->setDefault(kGradNone);
            p->setParent(*g);
            AddParam(page, p);
        }
        DefineColor(p_Desc, page, g, kParamGradColor, "Gradient Colour", 0.0, 0.5, 1.0, 1.0);

        // Ranges run past 0..1 on purpose. Confining them to the stroke means the
        // ramp must always span the whole of it, which is what made the gradient
        // feel like a fixed effect rather than a control; letting the stops sit
        // outside lets the visible part be any slice of the ramp.
        DefineDouble(p_Desc, page, g, kParamGradStart, "Gradient Start",
                     "Where the ramp begins. Everything before it is the base colour. "
                     "Put Start above End to reverse the gradient.",
                     0.0, -4.0, 5.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamGradEnd, "Gradient End",
                     "Where the ramp finishes. Drag it towards Start to squeeze the "
                     "blend into a narrow band.",
                     1.0, -4.0, 5.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamGradMid, "Gradient Midpoint",
                     "Biases where the two colours are evenly mixed.",
                     0.5, 0.02, 0.98, 0.02, 0.98);

        DefineBool(p_Desc, page, g, kParamGradLoop, "Seamless on Closed Paths",
                   "For Along Path on a closed shape, runs the ramp out and back so it "
                   "reaches the far colour halfway round and returns. Without it the "
                   "start and end of the ramp meet at the path's start point and show "
                   "as a hard seam across a contour that has no edge there. "
                   "Ignored by open paths.", true);
    }

    // --- dashes --------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "dashGroup", "Dashes");
        DefineBool(p_Desc, page, g, kParamDash, "Dashed", "", false);
        DefineDouble(p_Desc, page, g, kParamDashLen, "Dash Length",
                     "Percentage of frame height.", 2.0, 0.0, 100.0, 0.0, 10.0);
        DefineDouble(p_Desc, page, g, kParamDashGap, "Gap Length",
                     "Percentage of frame height.", 1.5, 0.0, 100.0, 0.0, 10.0);
        DefineDouble(p_Desc, page, g, kParamDashPhase, "Dash Offset",
                     "Slides the pattern along the path. Animate for a marching-ants effect.",
                     0.0, -1000.0, 1000.0, 0.0, 10.0);
    }

    // --- animation -----------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "animGroup", "Animation");

        // Drives the viewer overlay only, so it deliberately does NOT invalidate
        // the render cache -- showing an editing aid should not re-render frames.
        {
            OFX::BooleanParamDescriptor* p = p_Desc.defineBooleanParam(kParamShowCurve);
            p->setLabels("Show Curve Editor", "Show Curve Editor", "Show Curve Editor");
            p->setHint("Draws an editable easing curve in the viewer. Resolve's OFX host "
                       "supports no parametric parameters, so the viewer is the only "
                       "surface a curve editor can live on.");
            p->setDefault(false);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }
        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamCurveStage);
            p->setLabels("Curve Editor Stage", "Curve Editor Stage", "Curve Editor Stage");
            p->appendOption("In");
            p->appendOption("Out");
            p->setDefault(0);
            p->setAnimates(false);
            p->setParent(*g);
            AddParamUI(page, p);
        }

        DefineBool(p_Desc, page, g, kParamAnimEnable, "Animate",
                   "Plugin-owned in/out timing anchored to the clip. Needs no "
                   "keyframes: drop it on a clip of any length and the line draws "
                   "itself on at the head and off at the tail.", false);

        // The host does not reliably say where a clip starts, and gets it
        // differently wrong on different clip types, so both of these exist as
        // escape hatches rather than as tuning.
        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamTimeOrigin);
            p->setLabels("Time Origin", "Time Origin", "Time Origin");
            p->setHint("How the render time relates to the clip's first frame. Auto "
                       "detects it. Resolve hands Fusion clips a small clip-relative "
                       "time but hands stills, adjustment clips and footage an "
                       "absolute timeline frame -- if the animation sits fully drawn "
                       "or fully gone and never moves, force Timeline Absolute.");
            p->appendOption("Auto");
            p->appendOption("Clip Relative");
            p->appendOption("Timeline Absolute");
            p->setDefault(kOriginAuto);
            p->setAnimates(false);
            p->setParent(*g);
            AddParam(page, p);
        }

        DefineDouble(p_Desc, page, g, kParamTimeOffset, "Time Offset",
                     "Frames added after the origin is resolved. The reported clip "
                     "start includes any head handle, so this is the nudge that puts "
                     "frame 0 back on the first visible frame.",
                     0.0, -1000000.0, 1000000.0, -120.0, 120.0)
            ->setAnimates(false);

        DefineDouble(p_Desc, page, g, kParamClipLength, "Clip Length",
                     "Frames. 0 auto-detects from the host, which is unreliable in "
                     "Resolve -- set it by hand if a Clip End or Stretch anchor lands "
                     "in the wrong place.", 0.0, 0.0, 1000000.0, 0.0, 600.0)
            ->setAnimates(false);

        // In and Out are the same shape of thing, so they are defined by the same
        // code. Two hand-written copies drift: a control gets added to one and
        // forgotten on the other, and the asymmetry reads as a bug in the effect.
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool        isOut = (pass == 1);
            const std::string tag   = isOut ? "Out " : "In ";

            DefineBool(p_Desc, page, g, StageParam(kStageEnable, isOut), tag + "Enabled",
                       "", true);

            {
                OFX::ChoiceParamDescriptor* p =
                    p_Desc.defineChoiceParam(StageParam(kStageTarget, isOut));
                p->setLabels(tag + "Target", tag + "Target", tag + "Target");
                p->setHint("Which property this stage drives. From and To are in that "
                           "property's own units.");
                p->appendOption("Trim End");        // kTargetTrimEnd
                p->appendOption("Trim Start");      // kTargetTrimStart
                p->appendOption("Trim Offset");     // kTargetTrimOffset
                p->appendOption("Opacity");         // kTargetOpacity
                p->appendOption("Thickness (%)");   // kTargetThickness
                p->appendOption("Dash Offset (%)"); // kTargetDashOffset
                p->appendOption("Fill Opacity");    // kTargetFillOpacity
                p->setDefault(isOut ? kTargetTrimStart : kTargetTrimEnd);
                p->setParent(*g);
                AddParam(page, p);
            }

            DefineDouble(p_Desc, page, g, StageParam(kStageFrom, isOut), tag + "From",
                         "Value held before the stage begins.", 0.0, -1000.0, 1000.0, 0.0, 1.0);
            DefineDouble(p_Desc, page, g, StageParam(kStageTo, isOut), tag + "To",
                         "Value held after the stage ends.", 1.0, -1000.0, 1000.0, 0.0, 1.0);

            {
                OFX::ChoiceParamDescriptor* p =
                    p_Desc.defineChoiceParam(StageParam(kStageAnchor, isOut));
                p->setLabels(tag + "Anchor", tag + "Anchor", tag + "Anchor");
                p->setHint("What Start is measured from. Clip End counts backwards, so "
                           "an exit is written with a negative Start.");
                p->appendOption("Clip Start");
                p->appendOption("Clip End");
                p->appendOption("Stretch (% of clip)");
                p->setDefault(isOut ? kAnchorClipEnd : kAnchorClipStart);
                p->setParent(*g);
                AddParam(page, p);
            }

            DefineDouble(p_Desc, page, g, StageParam(kStageStart, isOut), tag + "Start",
                         "Frames from the anchor.", isOut ? -24.0 : 0.0,
                         -100000.0, 100000.0, -60.0, 60.0);
            DefineDouble(p_Desc, page, g, StageParam(kStageDuration, isOut), tag + "Duration",
                         "Frames.", 24.0, 0.0, 100000.0, 0.0, 120.0);

            {
                OFX::ChoiceParamDescriptor* p =
                    p_Desc.defineChoiceParam(StageParam(kStagePreset, isOut));
                p->setLabels(tag + "Easing", tag + "Easing", tag + "Easing");
                p->appendOption("Linear");
                p->appendOption("Smooth");
                p->appendOption("Ease In");
                p->appendOption("Ease Out");
                p->appendOption("Back Out (overshoot)");
                p->appendOption("Back In (anticipate)");
                p->appendOption("Custom");
                p->setDefault(kEaseSmooth);
                p->setParent(*g);
                AddParam(page, p);
            }

            // The four human controls. A preset writes these; editing one selects
            // Custom, so the panel never shows a preset name next to values that
            // are no longer that preset.
            DefineDouble(p_Desc, page, g, StageParam(kStageEaseIn, isOut), tag + "Ease In %",
                         "0 starts at full speed, 100 starts very gradually.",
                         42.0, 0.0, 100.0, 0.0, 100.0);
            DefineDouble(p_Desc, page, g, StageParam(kStageEaseOut, isOut), tag + "Ease Out %",
                         "0 stops abruptly, 100 glides to a halt.",
                         42.0, 0.0, 100.0, 0.0, 100.0);
            // Ranges deliberately allow NEGATIVE values. The curve editor drives
            // these by dragging bezier handles, and a handle can legitimately go
            // either side of the rails -- a negative anticipation is an
            // overshoot at the START of the move. Clamping these at zero would
            // make half the curve space unreachable by dragging, and the handle
            // would stick against an invisible wall.
            DefineDouble(p_Desc, page, g, StageParam(kStageAntic, isOut), tag + "Anticipation %",
                         "Pulls back before moving. Negative overshoots at the start.",
                         0.0, -200.0, 200.0, -100.0, 100.0);
            DefineDouble(p_Desc, page, g, StageParam(kStageOver, isOut), tag + "Overshoot %",
                         "Goes past the target and settles back. Negative undershoots.",
                         0.0, -200.0, 200.0, -100.0, 100.0);
        }
    }

    // --- stagger -------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "staggerGroup", "Stagger");
        DefineDouble(p_Desc, page, g, kParamStagger, "Stagger",
                     "Frames each stroke waits behind the one before it. Trim is a "
                     "single window shared by every stroke, so this is the only way "
                     "to draw them one after another.", 0.0, -10000.0, 10000.0, 0.0, 30.0);
        {
            OFX::ChoiceParamDescriptor* p = p_Desc.defineChoiceParam(kParamStaggerOrder);
            p->setLabels("Stagger Order", "Stagger Order", "Stagger Order");
            p->appendOption("Forward");
            p->appendOption("Reverse");
            p->appendOption("Centre Out");
            p->appendOption("Random");
            p->setDefault(kOrderForward);
            p->setParent(*g);
            AddParam(page, p);
        }
        {
            OFX::IntParamDescriptor* p = p_Desc.defineIntParam(kParamStaggerSeed);
            p->setLabels("Stagger Seed", "Stagger Seed", "Stagger Seed");
            p->setHint("Only used by the Random order.");
            p->setDefault(1);
            p->setRange(0, 100000);
            p->setDisplayRange(0, 100);
            p->setAnimates(false);
            p->setParent(*g);
            AddParam(page, p);
        }
    }

    // --- wiggle --------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "wiggleGroup", "Wiggle");
        DefineBool(p_Desc, page, g, kParamWiggle, "Wiggle",
                   "Seeded wobble along the path, for a hand-drawn feel. Deterministic, "
                   "so it does not shimmer between frames.", false);
        DefineDouble(p_Desc, page, g, kParamWiggleAmp, "Wiggle Amount",
                     "Peak displacement, as a percentage of frame height.",
                     0.4, 0.0, 100.0, 0.0, 3.0);
        DefineDouble(p_Desc, page, g, kParamWiggleWave, "Wiggle Wavelength",
                     "Distance per wobble, as a percentage of frame height. Smaller is "
                     "scribblier.", 6.0, 0.01, 1000.0, 0.5, 30.0);
        DefineDouble(p_Desc, page, g, kParamWigglePhase, "Wiggle Phase",
                     "Advances the noise. Keyframe it to make the line crawl.",
                     0.0, -100000.0, 100000.0, 0.0, 10.0);
        {
            OFX::IntParamDescriptor* p = p_Desc.defineIntParam(kParamWiggleSeed);
            p->setLabels("Wiggle Seed", "Wiggle Seed", "Wiggle Seed");
            p->setDefault(1);
            p->setRange(0, 100000);
            p->setDisplayRange(0, 100);
            p->setAnimates(false);
            p->setParent(*g);
            AddParam(page, p);
        }
    }

    // --- trim ----------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "trimGroup", "Trim Path");
        DefineDouble(p_Desc, page, g, kParamTrimStart, "Trim Start",
                     "Fraction of the path length where drawing begins.", 0.0, -10.0, 10.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamTrimEnd, "Trim End",
                     "Fraction of the path length where drawing ends. Keyframe this "
                     "from 0 to 1 for a draw-on reveal.", 1.0, -10.0, 10.0, 0.0, 1.0);
        DefineDouble(p_Desc, page, g, kParamTrimOffset, "Trim Offset",
                     "Slides the whole trim window along the path. Sweep it from -1 to 1 "
                     "to fly a short window along the line and off the end.",
                     0.0, -10.0, 10.0, -1.0, 1.0);
        DefineBool(p_Desc, page, g, kParamTrimWrap, "Wrap on Closed Paths",
                   "On, a closed path's trim carries on past the seam -- right for a ring "
                   "drawing itself on. Off, it trims exactly like an open path, so an "
                   "Offset sweep can carry the window off the end instead of circling "
                   "forever.", true);
    }

    {
        std::ostringstream o;
        o << "params: " << g_paramCount << " defined"
          << "  (" << kMaxStrokes << " strokes x " << kMaxPoints << " points = "
          << kPointParamCount << " of them are point slots)"
          << "\n    Every slot costs the host allocation, undo snapshots and a panel row "
             "whether or not a point is using it. If creating an instance is slow, this "
             "number is the lever -- kMaxStrokes / kMaxPoints in src/ParamNames.h.";
        bl::ProbeLog(o.str());
    }
}

OFX::ImageEffect* BetterLinePluginFactory::createInstance(OfxImageEffectHandle p_Handle,
                                                          OFX::ContextEnum /*p_Context*/)
{
    return new BetterLinePlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& p_FactoryArray)
{
    static BetterLinePluginFactory betterLinePlugin;
    p_FactoryArray.push_back(&betterLinePlugin);
}
