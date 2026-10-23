#!/bin/sh
# Build the macOS (Metal + Syphon) Paintify renderer into mac/build/runtime:
#   gpu-sbr                     the renderer
#   shaders/*.metal             shaders/ translated by tools/glsl_to_msl.py
#   Frameworks/Syphon.framework
#
# Needs Xcode (with its Metal toolchain: xcodebuild -downloadComponent
# MetalToolchain), and glslang + spirv-cross (brew install glslang spirv-cross).
set -eu
MAC="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$MAC")"
OUT="$MAC/build/runtime"
SYPHON_SRC="$MAC/third_party/Syphon-Framework"
SYPHON_BUILD="$MAC/build/syphon"
SYPHON_REV=f4761677a45b8034a3c2069ec0f3d2553da81fba

if [ ! -d "$SYPHON_SRC" ]; then
    git clone https://github.com/Syphon/Syphon-Framework.git "$SYPHON_SRC"
    git -C "$SYPHON_SRC" checkout -q "$SYPHON_REV"
fi
if [ ! -d "$SYPHON_BUILD/Build/Products/Release/Syphon.framework" ]; then
    xcodebuild -quiet -project "$SYPHON_SRC/Syphon.xcodeproj" -scheme Syphon \
        -configuration Release -derivedDataPath "$SYPHON_BUILD" \
        ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGN_IDENTITY=- build
fi

rm -rf "$OUT"
mkdir -p "$OUT/shaders" "$OUT/Frameworks"
python3 "$MAC/tools/glsl_to_msl.py" "$MAC/build/msl" >/dev/null
cp "$MAC"/build/msl/*.metal "$OUT/shaders/"
ditto "$SYPHON_BUILD/Build/Products/Release/Syphon.framework" "$OUT/Frameworks/Syphon.framework"

clang++ -std=c++17 -O2 -fobjc-arc -arch arm64 -mmacosx-version-min=13.0 \
    -Wall -Wno-unused-function -Wno-missing-braces \
    -I"$ROOT/src" -I"$MAC/src" -I"$MAC/third_party/stb" \
    -F"$OUT/Frameworks" \
    "$MAC/src/main_mac.mm" "$MAC/src/mtl_pipeline.mm" \
    "$ROOT/src/brush_atlas.cpp" "$ROOT/src/paramfile.cpp" \
    -framework Foundation -framework Metal -framework Syphon \
    -Wl,-rpath,@executable_path/Frameworks \
    -o "$OUT/gpu-sbr"
codesign --force --sign - "$OUT/gpu-sbr"
echo "built $OUT"
