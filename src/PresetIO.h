#pragma once

// Where preset files live, and the dialogs that find them.
//
// Separated from Preset.cpp because everything here is Win32 and none of it is
// testable without a desktop -- keeping the schema and the parser free of that
// is what lets them be exercised offline.

#include <string>
#include <vector>

namespace bl {

/** @brief Where the preset dialogs open.
 *
 * The folder set in preferences if there is one and it still exists, otherwise
 * DefaultPresetFolder(). A configured folder that has since been deleted or
 * unplugged falls back rather than sending the dialog somewhere that is not
 * there.
 */
std::string PresetFolder();

/** @brief Built-in default, created on demand.
 *
 * `Documents\BetterLine\Presets` -- somewhere the user can browse to, copy from
 * and back up. Falls back to LocalAppData only if Documents cannot be resolved.
 */
std::string DefaultPresetFolder();

/** @brief Preferences file, `%LOCALAPPDATA%\BetterLine\settings.json`.
 *
 * Stays in LocalAppData even though presets live in Documents: "where my presets
 * are" is per-machine bookkeeping, not something to browse or share.
 *
 * It cannot be an OFX parameter. A parameter lives inside the PROJECT, so it
 * travels with the timeline and belongs to one instance of the effect. This
 * belongs to the person and the machine, and has to apply to every instance ever
 * created -- including ones in projects that do not exist yet.
 */
std::string SettingsFilePath();

/** @brief The configured preset folder, or empty for "use the default". */
std::string LoadPresetFolderSetting();

/** @brief Record the preset folder. False with a message on failure. */
bool SavePresetFolderSetting(const std::string& folder, std::string& error);

/** @brief Ask the user to pick a folder. False if cancelled. */
bool ChooseFolder(const std::string& current, std::string& outPath);

/** @brief Ask the user for a preset file to read. False if cancelled. */
bool ChoosePresetToOpen(const std::string& folder, std::string& outPath);

/** @brief Ask the user where to write a preset. False if cancelled. */
bool ChoosePresetToSave(const std::string& folder, const std::string& suggestedName,
                        std::string& outPath);

/** @brief Whole-file read. False with a message if the file cannot be read. */
bool ReadTextFile(const std::string& path, std::string& outText, std::string& error);

/** @brief Whole-file write, creating parent folders. False with a message. */
bool WriteTextFile(const std::string& path, const std::string& text, std::string& error);

/** @brief File name without directory or extension. */
std::string StemOf(const std::string& path);

} // namespace bl
