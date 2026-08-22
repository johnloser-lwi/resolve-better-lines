#include "PresetIO.h"

#include "HostProbe.h"
#include "Preset.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace bl {

namespace {

// IFileDialog is a wide-character API and the rest of this plugin is UTF-8.
std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

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

namespace {

/** @brief One IFileDialog for all three pickers, started IN the preset folder.
 *
 * Deliberately not GetOpenFileName/GetSaveFileName, and this is lifted from
 * resolve-multi-transform, which hit the same wall first.
 *
 * `lpstrInitialDir` has only been advisory since Vista: Windows prefers the
 * calling EXECUTABLE's last-visited-folder MRU, and that executable is Resolve,
 * whose MRU is shared with every other dialog it opens. So Load Preset opened
 * wherever Resolve last browsed to -- an export folder, a media folder -- and
 * only sometimes in the preset folder, which is exactly how it was reported.
 * The old workaround of smuggling the directory in through `lpstrFile` beats
 * the MRU and then breaks Open outright: with OFN_FILEMUSTEXIST the dialog
 * validates that string as a FILE name, finds a path ending in a backslash,
 * and returns FALSE before it ever appears.
 *
 * IFileDialog has SetFolder, so the folder can simply be stated. SetFolder,
 * not SetDefaultFolder: the latter is a suggestion the last-visited folder
 * overrides, which is the trap above all over again. In folder-pick mode it
 * also replaces SHBrowseForFolder, whose cramped tree has no address bar and
 * no way to paste a path.
 *
 * The host's UI thread is already COM-initialised, but this must not assume
 * it: initialise, tolerate a different existing mode, and only uninitialise if
 * this call is what initialised it.
 */
enum DialogMode { kDlgOpen, kDlgSave, kDlgFolder };

bool RunFileDialog(DialogMode mode, const std::string& folder,
                   const std::string& suggestedName, std::string& outPath)
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(coInit);

    bool ok = false;
    IFileDialog* dialog = nullptr;

    HRESULT hr = CoCreateInstance(mode == kDlgSave ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                                  nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr))
    {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
        {
            flags |= FOS_FORCEFILESYSTEM;
            switch (mode)
            {
                case kDlgOpen:   flags |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST; break;
                case kDlgSave:   flags |= FOS_OVERWRITEPROMPT;                   break;
                case kDlgFolder: flags |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;   break;
            }
            dialog->SetOptions(flags);
        }

        if (mode != kDlgFolder)
        {
            static const COMDLG_FILTERSPEC kFilter[] = {
                { L"Better Line preset (*.json)", L"*.json" },
                { L"All files (*.*)",             L"*.*"    }
            };
            dialog->SetFileTypes(2, kFilter);
            dialog->SetDefaultExtension(L"json");
        }
        dialog->SetTitle(mode == kDlgSave   ? L"Save Better Line Preset"
                       : mode == kDlgOpen   ? L"Load Better Line Preset"
                                            : L"Choose a folder for Better Line presets");

        if (!folder.empty())
        {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(Widen(folder).c_str(), nullptr,
                                                      IID_PPV_ARGS(&start))))
            {
                dialog->SetFolder(start);
                start->Release();
            }
        }

        if (mode == kDlgSave && !suggestedName.empty())
            dialog->SetFileName(Widen(suggestedName).c_str());

        hr = dialog->Show(GetActiveWindow());
        if (SUCCEEDED(hr))
        {
            IShellItem* picked = nullptr;
            if (SUCCEEDED(dialog->GetResult(&picked)))
            {
                PWSTR wide = nullptr;
                if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide)
                {
                    outPath = Narrow(wide);
                    ok = !outPath.empty();
                    CoTaskMemFree(wide);
                }
                picked->Release();
            }
        }
        dialog->Release();
    }

    if (weInitialised) CoUninitialize();

    // Cancelling is not a failure and must stay silent. Anything else is a
    // dialog that refused to appear -- precisely the failure that went
    // unnoticed with the old API -- so it is recorded rather than swallowed.
    if (!ok && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        ProbeLog(std::string("preset dialog failed: hr=") + buf);
    }
    return ok;
}

} // namespace

bool ChooseFolder(const std::string& current, std::string& outPath)
{
    return RunFileDialog(kDlgFolder, current, std::string(), outPath);
}

bool ChoosePresetToOpen(const std::string& folder, std::string& outPath)
{
    return RunFileDialog(kDlgOpen, folder, std::string(), outPath);
}

bool ChoosePresetToSave(const std::string& folder, const std::string& suggestedName,
                        std::string& outPath)
{
    return RunFileDialog(kDlgSave, folder, SanitiseFileName(suggestedName), outPath);
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
