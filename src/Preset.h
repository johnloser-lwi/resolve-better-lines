#pragma once

// Preset serialisation.
//
// Deliberately free of any OFX or Win32 dependency, so the schema and the parser
// can be exercised without a host. A preset that silently loads the wrong values
// is worse than one that refuses to load, so the parser is strict: any failure
// leaves the output untouched rather than applying half a file.

#include <map>
#include <string>

#include "ParamRegistry.h"

namespace bl {

/** Bumped only on an incompatible schema change. Unknown keys are ignored and
 *  missing ones fall back to parameter defaults, so most additions need no
 *  bump -- which is the point of a registry-driven format. */
constexpr int kPresetSchemaVersion = 1;

/** @brief One parameter's value, in whichever shape its type needs. */
struct PresetValue
{
    int         kind  = kPKNone;
    double      n[4]  = {0.0, 0.0, 0.0, 0.0};
    int         count = 0;      ///< how many of n are meaningful
    bool        b     = false;
    std::string s;
};

using PresetMap = std::map<std::string, PresetValue>;

struct PresetFile
{
    int         schemaVersion = kPresetSchemaVersion;
    std::string name;
    PresetMap   params;
};

/** @brief Serialise. Round-trips exactly through PresetFromJson. */
std::string PresetToJson(const PresetFile& p);

/** @brief Parse a preset file.
 *
 * Strict: on any parse failure returns false with a message and leaves @p out
 * untouched, rather than applying a partially-read preset.
 */
bool PresetFromJson(const std::string& json, PresetFile& out, std::string& error);

/** @brief Parse a flat JSON object into name/value pairs.
 *
 * Shared by the preset reader and the preferences file, so there is one parser
 * to get right rather than two to keep in agreement.
 */
bool ParseJsonObject(const std::string& json, PresetMap& out, std::string& error);

/** @brief Quote and escape a string for JSON. */
std::string JsonQuote(const std::string& s);

/** @brief Strip characters Windows will not accept in a file name.
 *
 * A preset is named by its file, so a name with a slash in it does not fail at
 * save time -- it silently writes somewhere else, or not at all.
 */
std::string SanitiseFileName(const std::string& raw);

} // namespace bl
