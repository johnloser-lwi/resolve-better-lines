// Windows implementation of Platform.h: Win32 / COM.
//
// The dialog code is the IFileDialog path that shipped in PresetIO.cpp, moved
// here unchanged when the macOS port split the OS layer out.

#include "platform/Platform.h"

#include "HostProbe.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>

namespace bl {
namespace platform {

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

std::string KnownFolder(int csidl)
{
    char buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf))) return buf;
    return std::string();
}

} // namespace

std::string AppDataFolder()
{
    const std::string base = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty()) return std::string();
    const std::string dir = base + "\\BetterLine";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string DocumentsFolder()
{
    return KnownFolder(CSIDL_PERSONAL);
}

char PathSeparator() { return '\\'; }

bool MakeDirectory(const std::string& path)
{
    if (path.empty()) return false;
    CreateDirectoryA(path.c_str(), nullptr);
    return DirectoryExists(path);
}

bool DirectoryExists(const std::string& path)
{
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

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
bool RunFileDialog(DialogMode mode, const std::string& folder,
                   const std::string& suggestedName, std::string& outPath)
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(coInit);

    bool ok = false;
    IFileDialog* dialog = nullptr;

    HRESULT hr = CoCreateInstance(mode == kDialogSave ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                                  nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr))
    {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
        {
            flags |= FOS_FORCEFILESYSTEM;
            switch (mode)
            {
                case kDialogOpen:   flags |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST; break;
                case kDialogSave:   flags |= FOS_OVERWRITEPROMPT;                   break;
                case kDialogFolder: flags |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;   break;
            }
            dialog->SetOptions(flags);
        }

        if (mode != kDialogFolder)
        {
            static const COMDLG_FILTERSPEC kFilter[] = {
                { L"Better Line preset (*.json)", L"*.json" },
                { L"All files (*.*)",             L"*.*"    }
            };
            dialog->SetFileTypes(2, kFilter);
            dialog->SetDefaultExtension(L"json");
        }
        dialog->SetTitle(mode == kDialogSave ? L"Save Better Line Preset"
                       : mode == kDialogOpen ? L"Load Better Line Preset"
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

        if (mode == kDialogSave && !suggestedName.empty())
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

std::string LogTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return stamp;
}

} // namespace platform
} // namespace bl
