#include "PresetIO.h"

#include "Preset.h"
#include "platform/Platform.h"

#include <fstream>
#include <sstream>
#include <string>

// Shared by Windows and macOS. Everything that differs between them -- where
// the user's folders are, how a directory is made, the native dialogs -- is in
// platform/, so this file holds only the policy: which folder, which file,
// what falls back to what.

namespace bl {

namespace {

/** Create a directory and every parent it needs. */
bool EnsureFolder(const std::string& path)
{
    if (path.empty()) return false;
    if (platform::DirectoryExists(path)) return true;

    const size_t cut = path.find_last_of("\\/");
    if (cut != std::string::npos && cut > 0)
    {
        const std::string parent = path.substr(0, cut);
        // Stop at a drive root ("C:"), which cannot be created and always
        // exists if anything under it can.
        const bool driveRoot = parent.size() == 2 && parent[1] == ':';
        if (!driveRoot) EnsureFolder(parent);
    }
    return platform::MakeDirectory(path);
}

} // namespace

std::string DefaultPresetFolder()
{
    std::string base = platform::DocumentsFolder();
    if (base.empty()) base = platform::AppDataFolder();
    if (base.empty()) return std::string();

    const std::string dir = JoinPath(JoinPath(base, "BetterLine"), "Presets");
    EnsureFolder(dir);
    return dir;
}

std::string SettingsFilePath()
{
    const std::string dir = platform::AppDataFolder();
    if (dir.empty()) return std::string();
    return JoinPath(dir, "settings.json");
}

std::string LoadPresetFolderSetting()
{
    std::string text, err;
    if (!ReadTextFile(SettingsFilePath(), text, err)) return std::string();

    PresetMap m;
    // A corrupt preferences file falls back to the default rather than taking
    // the plugin down with it -- it is bookkeeping, not the user's work.
    if (!ParseJsonObject(text, m, err)) return std::string();

    auto it = m.find("presetFolder");
    if (it == m.end() || it->second.kind != kPKString) return std::string();
    return it->second.s;
}

bool SavePresetFolderSetting(const std::string& folder, std::string& error)
{
    std::ostringstream o;
    o << "{\n  \"presetFolder\": " << JsonQuote(folder) << "\n}\n";
    return WriteTextFile(SettingsFilePath(), o.str(), error);
}

std::string PresetFolder()
{
    const std::string configured = LoadPresetFolderSetting();
    if (!configured.empty() && platform::DirectoryExists(configured)) return configured;
    return DefaultPresetFolder();
}

// =============================================================================
// Dialogs
//
// Every one of these pumps a modal message loop, which is why none of them may
// be called from inside an OFX parameter edit block: the host would be left
// holding an open block while an unbounded amount of other work ran through its
// own event loop. The dialogs themselves are platform::RunFileDialog -- see
// PlatformWin.cpp for why Windows needs IFileDialog::SetFolder to open in the
// preset folder at all.
// =============================================================================

bool ChooseFolder(const std::string& current, std::string& outPath)
{
    return platform::RunFileDialog(platform::kDialogFolder, current, std::string(), outPath);
}

bool ChoosePresetToOpen(const std::string& folder, std::string& outPath)
{
    return platform::RunFileDialog(platform::kDialogOpen, folder, std::string(), outPath);
}

bool ChoosePresetToSave(const std::string& folder, const std::string& suggestedName,
                        std::string& outPath)
{
    return platform::RunFileDialog(platform::kDialogSave, folder,
                                   SanitiseFileName(suggestedName), outPath);
}

// =============================================================================
// Files
// =============================================================================

bool ReadTextFile(const std::string& path, std::string& outText, std::string& error)
{
    if (path.empty()) { error = "no path"; return false; }

    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "could not open " + path; return false; }

    std::ostringstream o;
    o << f.rdbuf();
    outText = o.str();
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string& error)
{
    if (path.empty()) { error = "no path"; return false; }

    const size_t cut = path.find_last_of("\\/");
    if (cut != std::string::npos) EnsureFolder(path.substr(0, cut));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "could not write " + path; return false; }

    f << text;
    if (!f) { error = "write failed for " + path; return false; }
    return true;
}

std::string StemOf(const std::string& path)
{
    const size_t cut = path.find_last_of("\\/");
    std::string name = (cut == std::string::npos) ? path : path.substr(cut + 1);

    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);

    return name;
}

} // namespace bl
