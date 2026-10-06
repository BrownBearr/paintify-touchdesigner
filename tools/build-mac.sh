#!/usr/bin/env bash
# Build the Metal renderer for macOS and package it for the TouchDesigner TOX.
#
#   tools/build-mac.sh            build into build-mac/ and write the runtime tarball
#   tools/build-mac.sh --test     also paint the built-in test subject to build-mac/test.png
#
# Needs: Xcode (for the Metal compiler), and `brew install cmake glslang spirv-cross`.
# Syphon.framework is built from source unless SYPHON_FRAMEWORK_DIR points at a
# directory that already contains one. SYPHON_REF picks the Syphon-Framework
# branch or tag to build (default: main).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-mac"
PACKAGE="$BUILD/package"
DEPLOY="${CMAKE_OSX_DEPLOYMENT_TARGET:-12.0}"

for tool in cmake glslangValidator spirv-cross xcrun; do
    command -v "$tool" >/dev/null || { echo "missing: $tool (brew install cmake glslang spirv-cross; install Xcode)"; exit 1; }
done
xcrun -sdk macosx --find metal >/dev/null 2>&1 || {
    echo "The Metal compiler is part of full Xcode, not the Command Line Tools."
    echo "Install Xcode, then: sudo xcode-select -s /Applications/Xcode.app"
    exit 1
}

mkdir -p "$BUILD"

# --- Syphon ------------------------------------------------------------------
if [ -z "${SYPHON_FRAMEWORK_DIR:-}" ]; then
    SRC="$BUILD/deps/Syphon-Framework"
    if [ ! -d "$SRC/.git" ]; then
        mkdir -p "$BUILD/deps"
        git clone --depth 1 --branch "${SYPHON_REF:-main}" \
            https://github.com/Syphon/Syphon-Framework.git "$SRC"
    fi
    xcodebuild -project "$SRC/Syphon.xcodeproj" -target Syphon -configuration Release \
        SYMROOT="$BUILD/deps/out" CODE_SIGNING_ALLOWED=NO \
        MACOSX_DEPLOYMENT_TARGET="$DEPLOY" ONLY_ACTIVE_ARCH=NO build
    SYPHON_FRAMEWORK_DIR="$BUILD/deps/out/Release"
fi
[ -d "$SYPHON_FRAMEWORK_DIR/Syphon.framework" ] || { echo "no Syphon.framework in $SYPHON_FRAMEWORK_DIR"; exit 1; }

# --- renderer ----------------------------------------------------------------
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
    -DSYPHON_FRAMEWORK_DIR="$SYPHON_FRAMEWORK_DIR" \
    ${CMAKE_OSX_ARCHITECTURES:+-DCMAKE_OSX_ARCHITECTURES="$CMAKE_OSX_ARCHITECTURES"}
cmake --build "$BUILD" --config Release -j

# --- package -----------------------------------------------------------------
# The TOX carries one tarball: tar keeps the framework's symlinks and the
# executable bit, which TouchDesigner's virtual file system would not.
BIN="$BUILD/mac"
rm -rf "$PACKAGE"
mkdir -p "$PACKAGE/shaders" "$PACKAGE/Frameworks"
cp "$BIN/paintify-mac" "$PACKAGE/"
cp "$BIN/paintify.metallib" "$PACKAGE/shaders/"
cp -R "$SYPHON_FRAMEWORK_DIR/Syphon.framework" "$PACKAGE/Frameworks/"
# Code downloaded or copied around needs a signature to run on Apple Silicon.
# Ad hoc is enough for files the TOX extracts itself; replace with a Developer
# ID identity (CODESIGN_IDENTITY) to distribute a notarized build.
IDENTITY="${CODESIGN_IDENTITY:--}"
codesign --force --deep --sign "$IDENTITY" "$PACKAGE/Frameworks/Syphon.framework"
codesign --force --sign "$IDENTITY" "$PACKAGE/paintify-mac"
COPYFILE_DISABLE=1 tar -cf "$BUILD/paintify-mac-runtime.tar" -C "$PACKAGE" .
echo "runtime: $BUILD/paintify-mac-runtime.tar"

if [ "${1:-}" = "--test" ]; then
    "$PACKAGE/paintify-mac" --metallib "$PACKAGE/shaders/paintify.metallib" --out "$BUILD/test.png"
    echo "test render: $BUILD/test.png"
fi
