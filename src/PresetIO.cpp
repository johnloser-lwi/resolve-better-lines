#include "PresetIO.h"

#include "Preset.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>

namespace bl {

namespace {

/** Create a directory and every parent it needs. */
bool EnsureFolder(const std::string& path)
{
    if (path.empty()) return false;
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;

    const size_t cut = path.find_last_of("\\/");
    if (cut != std::string::npos && cut > 2) EnsureFolder(path.substr(0, cut));

    return CreateDirectoryA(path.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FolderExists(const std::string& path)
{
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string KnownFolder(int csidl)
{
    char buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf))) return buf;
    return std::string();
}

} // namespace

std::string DefaultPresetFolder()
{
    std::string base = KnownFolder(CSIDL_PERSONAL);          // Documents
    if (base.empty()) base = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty()) return std::string();

    const std::string dir = base + "\\BetterLine\\Presets";
    EnsureFolder(dir);
    return dir;
}

std::string SettingsFilePath()
{
    const std::string base = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty()) return std::string();

    const std::string dir = base + "\\BetterLine";
    EnsureFolder(dir);
    return dir + "\\settings.json";
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
    if (!configured.empty() && FolderExists(configured)) return configured;
    return DefaultPresetFolder();
}

// =============================================================================
// Dialogs
//
// Every one of these pumps a modal message loop, which is why none of them may
// be called from inside an OFX parameter edit block: the host would be left
// holding an open block while an unbounded amount of other work ran through its
// own event loop.
// =============================================================================

bool ChooseFolder(const std::string& current, std::string& outPath)
{
    // SHBrowseForFolder rather than IFileDialog: it needs no COM apartment
    // negotiation, and a plugin does not own the thread's apartment model --
    // the host does, and it may already have initialised it either way.
    BROWSEINFOA bi = {};
    char display[MAX_PATH] = {0};

    bi.lpszTitle = "Choose a folder for Better Line presets";
    bi.pszDisplayName = display;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    if (!id) return false;

    char path[MAX_PATH] = {0};
    const bool ok = SHGetPathFromIDListA(id, path) != 0;
    CoTaskMemFree(id);

    if (!ok) return false;
    outPath = path;
    (void)current;
    return true;
}

namespace {

void FillOfn(OPENFILENAMEA& ofn, char* buf, size_t bufSize,
             const std::string& folder, const char* title)
{
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Better Line preset (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(bufSize);
    ofn.lpstrTitle  = title;
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrDefExt = "json";
}

} // namespace

bool ChoosePresetToOpen(const std::string& folder, std::string& outPath)
{
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    FillOfn(ofn, buf, sizeof(buf), folder, "Load Better Line Preset");
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn)) return false;
    outPath = buf;
    return true;
}

bool ChoosePresetToSave(const std::string& folder, const std::string& suggestedName,
                        std::string& outPath)
{
    char buf[MAX_PATH] = {0};
    const std::string stem = SanitiseFileName(suggestedName);
    if (!stem.empty()) strncpy_s(buf, stem.c_str(), _TRUNCATE);

    OPENFILENAMEA ofn = {};
    FillOfn(ofn, buf, sizeof(buf), folder, "Save Better Line Preset");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&ofn)) return false;
    outPath = buf;
    return true;
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
