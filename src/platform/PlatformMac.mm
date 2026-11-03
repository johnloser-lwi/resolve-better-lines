// macOS implementation of Platform.h: Cocoa panels and the standard user
// folders. Compiled as Objective-C++ with ARC (see CMakeLists.txt).
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available, on
// the same pattern as resolve-multi-transform's PlatformMac.mm. The first
// things to check when it is: that the dialogs appear in front of Resolve and
// open in the preset folder, and that the folders below are created.

#include "platform/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <sys/stat.h>
#include <sys/time.h>

#include <cstdio>
#include <ctime>

namespace bl {
namespace platform {

namespace {

NSString* ToNS(const std::string& s)
{
    NSString* n = [NSString stringWithUTF8String:s.c_str()];
    return n ? n : @"";
}

std::string FromNS(NSString* s)
{
    if (!s) return std::string();
    const char* c = [s fileSystemRepresentation];
    return c ? std::string(c) : std::string();
}

std::string UserFolder(NSSearchPathDirectory which)
{
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(which, NSUserDomainMask, YES);
    return paths.count > 0 ? FromNS(paths[0]) : std::string();
}

/// AppKit panels belong to the main thread. The host calls instance-changed
/// there in practice; this keeps the call safe if it ever does not, at the cost
/// of blocking the caller until the user answers -- which a modal dialog does
/// anyway.
void OnMain(void (^block)(void))
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

} // namespace

std::string AppDataFolder()
{
    const std::string base = UserFolder(NSApplicationSupportDirectory);
    if (base.empty()) return std::string();
    const std::string dir = base + "/BetterLine";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string DocumentsFolder()
{
    return UserFolder(NSDocumentDirectory);
}

char PathSeparator() { return '/'; }

bool MakeDirectory(const std::string& path)
{
    if (path.empty()) return false;
    mkdir(path.c_str(), 0755);
    return DirectoryExists(path);
}

bool DirectoryExists(const std::string& path)
{
    if (path.empty()) return false;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/** @brief NSOpenPanel / NSSavePanel, started IN @p folder.
 *
 * Unlike Win32's lpstrInitialDir, directoryURL is honoured: macOS has no
 * per-executable last-visited MRU overriding it, so stating the folder is the
 * whole fix here -- the trap the Windows side had to route around does not
 * exist.
 */
bool RunFileDialog(DialogMode mode, const std::string& folder,
                   const std::string& suggestedName, std::string& outPath)
{
    __block bool        ok = false;
    __block std::string picked;
    NSString* start = folder.empty()        ? nil : ToNS(folder);
    NSString* name  = suggestedName.empty() ? nil : ToNS(suggestedName);

    OnMain(^{
        @autoreleasepool {
            NSSavePanel* panel = (mode == kDialogSave) ? [NSSavePanel savePanel]
                                                       : [NSOpenPanel openPanel];
            panel.title = (mode == kDialogSave) ? @"Save Better Line Preset"
                        : (mode == kDialogOpen) ? @"Load Better Line Preset"
                                                : @"Choose a folder for Better Line presets";
            panel.canCreateDirectories = YES;

            if (mode != kDialogFolder)
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                // allowedFileTypes rather than allowedContentTypes: the latter
                // needs the UniformTypeIdentifiers framework, and the extension
                // is all this needs to say. It also makes the save panel append
                // .json to a name typed without one.
                panel.allowedFileTypes = @[ @"json" ];
#pragma clang diagnostic pop
            }

            if (start) panel.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];

            if (mode == kDialogSave)
            {
                if (name) panel.nameFieldStringValue = [name stringByAppendingPathExtension:@"json"];
            }
            else
            {
                NSOpenPanel* open = (NSOpenPanel*)panel;
                open.canChooseFiles          = (mode == kDialogOpen);
                open.canChooseDirectories    = (mode == kDialogFolder);
                open.allowsMultipleSelection = NO;
            }

            if ([panel runModal] == NSModalResponseOK && panel.URL)
            {
                picked = FromNS(panel.URL.path);
                ok = !picked.empty();
            }
        }
    });

    if (ok) outPath = picked;
    return ok;
}

std::string LogTimestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm local;
    const time_t secs = tv.tv_sec;
    localtime_r(&secs, &local);

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
    return stamp;
}

} // namespace platform
} // namespace bl
