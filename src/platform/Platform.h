#pragma once

// The operating-system layer: everything the plugin needs from the OS that is
// not image processing -- the user's folders, directories, the native file
// dialogs and a log timestamp. One implementation per platform:
//
//   PlatformWin.cpp   Win32 / COM (IFileDialog, SHGetFolderPath)
//   PlatformMac.mm    Cocoa (NSOpenPanel, NSSavePanel, NSSearchPath...)
//
// Kept small and string-based so the preset and settings logic above it
// (PresetIO.cpp) and the probe log (HostProbe.cpp) are shared code with one
// behaviour on both systems. Paths are UTF-8 throughout. Same shape as
// resolve-multi-transform's layer, trimmed to what this plugin uses.

#include <string>

namespace bl {
namespace platform {

/// The plugin's private data folder, created if missing; "" if the OS will not
/// say. Holds settings.json and probe.log.
///   Windows: %LOCALAPPDATA%\BetterLine
///   macOS:   ~/Library/Application Support/BetterLine
std::string AppDataFolder();

/// The user's Documents folder (following any redirection), or "".
std::string DocumentsFolder();

/// '\\' on Windows, '/' on macOS.
char PathSeparator();

/// Create one directory level. True if it exists afterwards.
bool MakeDirectory(const std::string& path);

bool DirectoryExists(const std::string& path);

enum DialogMode { kDialogOpen, kDialogSave, kDialogFolder };

/// The native picker -- open or save a .json preset, or choose a folder --
/// starting IN @p folder rather than wherever the host last browsed.
/// @p suggestedName is the save dialog's file name, without extension.
/// False when cancelled or when the dialog could not be shown.
bool RunFileDialog(DialogMode mode, const std::string& folder,
                   const std::string& suggestedName, std::string& outPath);

/// Local wall-clock time as "HH:MM:SS.mmm ", for log lines.
std::string LogTimestamp();

} // namespace platform

/// @p dir and @p name joined with the platform's separator.
inline std::string JoinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    const char sep  = platform::PathSeparator();
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == sep) return dir + name;
    return dir + sep + name;
}

} // namespace bl
