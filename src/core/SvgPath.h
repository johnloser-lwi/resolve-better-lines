#pragma once

// SVG geometry import.
//
// Deliberately free of OFX and Win32 so it can be exercised offline: a parser
// that silently produces the wrong shape is worse than one that refuses, and
// the only way to know which it did is to run it and look.
//
// Geometry only. Fills, gradients, strokes, masks, text and filters are all
// ignored -- this reads WHERE things are, not what they look like, because the
// look is what the plugin's own style controls are for.

#include "Geometry.h"

#include <string>
#include <vector>

namespace bl {

struct SvgImportResult
{
    std::vector<Stroke> strokes;   ///< in blob-normalised square units
    int    subpaths   = 0;
    int    points     = 0;
    bool   sawArc     = false;
    bool   truncated  = false;
    std::string error;             ///< empty on success
};

/** @brief Parse an SVG document into strokes, normalised and ready to store.
 *
 * @param maxPoints  hard ceiling across the whole import; past it parsing stops
 *                   and `truncated` is set. A pathological file should not be
 *                   able to allocate without bound inside a button press.
 *
 * The result is in the square-normalised space PathBlob.h describes: the
 * document is fitted to the frame height, centred, with its aspect preserved, so
 * a circle imports as a circle. SVG's y axis runs down and this plugin's runs
 * up, so the flip happens here, once.
 */
SvgImportResult ParseSvg(const std::string& svgText, int maxPoints = 20000);

/** @brief Parse just a path `d` attribute, in user units, without normalising.
 *
 * Exposed for testing: it is where every command and every relative/absolute
 * distinction lives, and it is worth being able to check one path in isolation.
 */
bool ParsePathData(const std::string& d, std::vector<Stroke>& out,
                   bool& sawArc, int maxPoints);

} // namespace bl
