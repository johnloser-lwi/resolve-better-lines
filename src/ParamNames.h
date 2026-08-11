#pragma once

#include <string>

namespace bl {

/** @brief Compile-time ceiling on strokes and points per stroke.
 *
 * OFX cannot create parameters at runtime, and this plugin stores point geometry
 * in real, host-keyframable parameter slots so that Resolve can keyframe and
 * curve-edit every point natively. The two facts together force a hard cap: the
 * slots must all exist before the user has drawn anything.
 *
 * That is a deliberate trade. The alternative -- serialising all geometry into a
 * single string parameter -- has no ceiling at all, but no host keyframing
 * either, and the animation would have to be entirely plugin-owned.
 *
 * kMaxStrokes * kMaxPoints * 2 Double2D parameters exist unconditionally, so
 * raising these is not free. Check `maxParameters` in probe.log before doing it:
 * a host that reports a limit will refuse the ones past it, silently.
 */
// Reduced from 8x16 after the editor was reported stalling whenever an instance
// was created. These slots are not free at rest: the host allocates, tracks,
// snapshots for undo and panels every one of them, whether or not a point is
// using it. Halving the strokes removed ~148 parameters.
//
// kMaxPoints is the other dial if that is still not enough -- 16 points per
// stroke is generous when a line needs 2, a box 4 and a circle 4.
constexpr int kMaxStrokes = 4;
constexpr int kMaxPoints  = 16;

/// Total point parameters this plugin defines. Kept visible because it is the
/// number that decides whether the Inspector stays usable.
constexpr int kPointParamCount = kMaxStrokes * kMaxPoints * 2;

// --- naming ------------------------------------------------------------------
// Names are 0-based internally and 1-based in labels. The name is a persistence
// key that must never change; the label is cosmetic.

inline std::string StrokeParam(const char* base, int stroke)
{
    return "s" + std::to_string(stroke) + "_" + base;
}

inline std::string PointParam(const char* base, int stroke, int point)
{
    return "s" + std::to_string(stroke) + "p" + std::to_string(point) + "_" + base;
}

// --- shape -------------------------------------------------------------------
constexpr const char* kParamStrokeCount = "strokeCount";

constexpr const char* kStrokeEnable   = "enable";
constexpr const char* kStrokeCount    = "count";
constexpr const char* kStrokeClosed   = "closed";
constexpr const char* kStrokeTopology = "topology";

constexpr const char* kPointPos = "pos";
constexpr const char* kPointTan = "tan";

/** @brief Secrecy a freshly created instance should already have.
 *
 * The parameter DESCRIPTORS are given this state, so a new instance is born with
 * the right things hidden and the instance has nothing to correct. Previously
 * every parameter was described visible and the constructor then made several
 * hundred setIsSecret calls to hide them again -- paid on every instance
 * creation, which is exactly when the editor felt slow.
 *
 * These must agree with the parameter DEFAULTS below (stroke count 1, point
 * count 2), which is why both sides call this rather than each writing the rule
 * out and drifting.
 */
inline bool DefaultStrokeSecret(int stroke) { return stroke != 0; }
inline bool DefaultPointSecret(int stroke, int point)
{
    return !(stroke == 0 && point < 2);
}

// --- presets -----------------------------------------------------------------
constexpr const char* kParamPresetName   = "presetName";
constexpr const char* kParamPresetSave   = "presetSave";
constexpr const char* kParamPresetLoad   = "presetLoad";
constexpr const char* kParamPresetFolder = "presetFolder";
constexpr const char* kParamPresetSetDir = "presetSetFolder";
constexpr const char* kParamPresetReset  = "presetReset";

// --- imported paths ----------------------------------------------------------
constexpr const char* kParamImportSvg   = "importSvg";
constexpr const char* kParamClearImport = "clearImport";
constexpr const char* kParamPathBlob    = "pathData";
constexpr const char* kParamImportInfo  = "importInfo";
constexpr const char* kParamImportShow  = "importEnabled";
constexpr const char* kParamImportCol   = "importUseColours";
constexpr const char* kParamImportWid   = "importUseWidths";
constexpr const char* kParamImportScale = "importWidthScale";

// --- shape generator ---------------------------------------------------------
constexpr const char* kParamShapeKind   = "shapeKind";
constexpr const char* kParamShapeSides  = "shapeSides";
constexpr const char* kParamShapeCorner = "shapeCorner";
constexpr const char* kParamShapeSize   = "shapeSize";
constexpr const char* kParamShapeSlot   = "shapeTarget";
constexpr const char* kParamShapeAdd    = "shapeAdd";

// --- global transform --------------------------------------------------------
constexpr const char* kParamXfPos    = "xformPosition";
constexpr const char* kParamXfRot    = "xformRotation";
constexpr const char* kParamXfScale  = "xformScale";
constexpr const char* kParamXfScaleY = "xformScaleY";
constexpr const char* kParamXfLink   = "xformScaleLink";
constexpr const char* kParamXfAnchor = "xformAnchor";

// --- style -------------------------------------------------------------------
constexpr const char* kParamColor      = "color";
constexpr const char* kParamOpacity    = "opacity";
constexpr const char* kParamThickness  = "thickness";
constexpr const char* kParamFeather    = "feather";
constexpr const char* kParamCap        = "cap";
constexpr const char* kParamTaperStart = "taperStart";
constexpr const char* kParamTaperBody  = "taperBody";
constexpr const char* kParamTaperEnd   = "taperEnd";

constexpr const char* kParamOutline      = "outline";
constexpr const char* kParamOutlineWidth = "outlineWidth";
constexpr const char* kParamOutlineColor = "outlineColor";

constexpr const char* kParamGlow          = "glow";
constexpr const char* kParamGlowRadius    = "glowRadius";
constexpr const char* kParamGlowIntensity = "glowIntensity";
constexpr const char* kParamGlowColor     = "glowColor";

constexpr const char* kParamShadow        = "shadow";
constexpr const char* kParamShadowOffset  = "shadowOffset";
constexpr const char* kParamShadowSoft    = "shadowSoftness";
constexpr const char* kParamShadowOpacity = "shadowOpacity";
constexpr const char* kParamShadowColor   = "shadowColor";

constexpr const char* kParamArrowStart  = "arrowStart";
constexpr const char* kParamArrowEnd    = "arrowEnd";
constexpr const char* kParamArrowLength = "arrowLength";
constexpr const char* kParamArrowWidth  = "arrowWidth";
constexpr const char* kParamArrowRound  = "arrowRoundness";
constexpr const char* kParamArrowPos    = "arrowPosition";

constexpr const char* kParamFill        = "fill";
constexpr const char* kParamFillColor   = "fillColor";
constexpr const char* kParamFillOpacity = "fillOpacity";

constexpr const char* kParamGradMode  = "gradMode";
constexpr const char* kParamGradColor = "gradColor";
constexpr const char* kParamGradStart = "gradStart";
constexpr const char* kParamGradEnd   = "gradEnd";
constexpr const char* kParamGradMid   = "gradMidpoint";
constexpr const char* kParamGradLoop  = "gradSeamless";

constexpr const char* kParamDash      = "dash";
constexpr const char* kParamDashLen   = "dashLength";
constexpr const char* kParamDashGap   = "dashGap";
constexpr const char* kParamDashPhase = "dashPhase";

// --- animation ---------------------------------------------------------------
// Two stages, In and Out, each a suffix on these.
constexpr const char* kParamAnimEnable = "animate";
constexpr const char* kParamClipLength = "clipLength";
constexpr const char* kParamShowCurve  = "showCurveEditor";
constexpr const char* kParamCurveStage = "curveEditorStage";
constexpr const char* kParamTimeOrigin = "timeOrigin";
constexpr const char* kParamTimeOffset = "timeOffset";

inline std::string StageParam(const char* base, bool out)
{
    return std::string(out ? "out" : "in") + base;
}

constexpr const char* kStageEnable   = "Enable";
constexpr const char* kStageTarget   = "Target";
constexpr const char* kStageFrom     = "From";
constexpr const char* kStageTo       = "To";
constexpr const char* kStageStart    = "Start";
constexpr const char* kStageDuration = "Duration";
constexpr const char* kStageAnchor   = "Anchor";
constexpr const char* kStagePreset   = "Ease";
constexpr const char* kStageEaseIn   = "EaseIn";
constexpr const char* kStageEaseOut  = "EaseOut";
constexpr const char* kStageAntic    = "Anticipation";
constexpr const char* kStageOver     = "Overshoot";

constexpr const char* kParamStagger      = "stagger";
constexpr const char* kParamStaggerOrder = "staggerOrder";
constexpr const char* kParamStaggerSeed  = "staggerSeed";

// --- wiggle ------------------------------------------------------------------
constexpr const char* kParamWiggle      = "wiggle";
constexpr const char* kParamWiggleAmp   = "wiggleAmount";
constexpr const char* kParamWiggleWave  = "wiggleWavelength";
constexpr const char* kParamWigglePhase = "wigglePhase";
constexpr const char* kParamWiggleSeed  = "wiggleSeed";

// --- trim --------------------------------------------------------------------
constexpr const char* kParamTrimStart  = "trimStart";
constexpr const char* kParamTrimEnd    = "trimEnd";
constexpr const char* kParamTrimOffset = "trimOffset";
constexpr const char* kParamTrimWrap   = "trimWrap";

} // namespace bl
