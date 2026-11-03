#!/bin/bash
# Remove BetterLine.ofx.bundle from the system OFX folder on macOS -- the
# counterpart of uninstall.ps1. Presets and settings are left alone: they are
# the user's, in ~/Documents/BetterLine and ~/Library/Application Support.

set -euo pipefail

TARGET="/Library/OFX/Plugins/BetterLine.ofx.bundle"

if pgrep -x Resolve >/dev/null; then
    echo "DaVinci Resolve is running. Quit it first." >&2
    exit 1
fi

if [ -d "$TARGET" ]; then
    sudo rm -rf "$TARGET"
    echo "Removed $TARGET"
else
    echo "Not installed: $TARGET"
fi
