#!/bin/bash
# Install BetterLine.ofx.bundle into the system OFX folder on macOS -- the
# counterpart of install.ps1.
#
#   scripts/install-mac.sh             install the Release build
#   scripts/install-mac.sh --build     build first
#   scripts/install-mac.sh --force     quit Resolve instead of aborting
#   scripts/install-mac.sh Debug       install the Debug build
#
# /Library/OFX/Plugins is the one place Resolve looks on macOS, and it is owned
# by root, so the copy runs through sudo and asks for your password.

set -euo pipefail

CONFIG=Release
BUILD_FIRST=0
FORCE=0
for arg in "$@"; do
    case "$arg" in
        Debug|Release) CONFIG="$arg" ;;
        --build)       BUILD_FIRST=1 ;;
        --force)       FORCE=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$REPO/build-mac/$CONFIG/BetterLine.ofx.bundle"
DEST="/Library/OFX/Plugins"

if [ "$BUILD_FIRST" = 1 ]; then
    "$REPO/scripts/build-mac.sh" "$CONFIG"
fi

if [ ! -f "$BUNDLE/Contents/MacOS/BetterLine.ofx" ]; then
    echo "No built bundle at $BUNDLE -- run scripts/build-mac.sh, or pass --build." >&2
    exit 1
fi

# Resolve holds the loaded binary open and caches the plugin list at launch, so
# replacing it underneath a running Resolve either fails or is silently ignored
# until the next launch. Same rule as the Windows installer: refuse, or quit it.
if pgrep -x Resolve >/dev/null; then
    if [ "$FORCE" = 1 ]; then
        echo "==> Quitting DaVinci Resolve"
        osascript -e 'quit app "DaVinci Resolve"' || true
        for _ in $(seq 1 30); do pgrep -x Resolve >/dev/null || break; sleep 1; done
        if pgrep -x Resolve >/dev/null; then
            echo "Resolve did not quit within 30s -- quit it by hand and re-run." >&2
            exit 1
        fi
    else
        echo "DaVinci Resolve is running. Quit it first, or re-run with --force." >&2
        exit 1
    fi
fi

echo "==> Installing to $DEST (sudo)"
sudo mkdir -p "$DEST"
# Remove, then copy: copying over an existing bundle merges directories, and a
# stale file left inside is exactly the kind of thing that is never noticed.
sudo rm -rf "$DEST/BetterLine.ofx.bundle"
sudo cp -R "$BUNDLE" "$DEST/"
# A bundle that arrived by download or AirDrop carries the quarantine flag, and
# a quarantined plugin is refused without a word. Harmless when absent.
sudo xattr -dr com.apple.quarantine "$DEST/BetterLine.ofx.bundle" 2>/dev/null || true

echo "    Installed $DEST/BetterLine.ofx.bundle"
echo "    Launch Resolve. The probe log is ~/Library/Application Support/BetterLine/probe.log"
