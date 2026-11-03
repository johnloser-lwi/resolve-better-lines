#!/bin/bash
# Build BetterLine.ofx.bundle on macOS -- the counterpart of build.ps1.
#
#   scripts/build-mac.sh            Release, universal arm64 + x86_64
#   scripts/build-mac.sh Debug
#   scripts/build-mac.sh --clean    wipe the build directory first
#
# Needs the Xcode command line tools (xcode-select --install) and CMake 3.24+
# (brew install cmake). Builds into build-mac/, not build/, so a repo shared
# with a Windows checkout never mixes the two.

set -euo pipefail

CONFIG=Release
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        Debug|Release) CONFIG="$arg" ;;
        --clean)       CLEAN=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build-mac"

command -v cmake >/dev/null || { echo "cmake not found -- brew install cmake" >&2; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "no Xcode command line tools -- xcode-select --install" >&2; exit 1; }

if [ "$CLEAN" = 1 ] && [ -d "$BUILD" ]; then
    echo "Removing $BUILD"
    rm -rf "$BUILD"
fi

cmake -S "$REPO" -B "$BUILD" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD" --config "$CONFIG" -j "$(sysctl -n hw.ncpu)"

# The offline checks: stagger ordering, preset round-trip, SVG parsing, shape
# generators. There is no GPU parity test on macOS -- there is no GPU path.
(cd "$BUILD" && ctest -C "$CONFIG" --output-on-failure)

BUNDLE="$BUILD/$CONFIG/BetterLine.ofx.bundle"
BIN="$BUNDLE/Contents/MacOS/BetterLine.ofx"
echo
echo "Bundle: $BUNDLE"
# Worth seeing on every build: a slice missing here is a plugin Resolve
# silently does not list on that kind of Mac.
lipo -info "$BIN" || true
codesign --verify --verbose "$BIN" 2>&1 | tail -1 || true
