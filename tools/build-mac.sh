#!/usr/bin/env bash
# Builds gpu-sbr on macOS: Apple Silicon (M1 and later) or Intel, macOS 11+.
#
# The renderer runs on Vulkan through MoltenVK there -- Apple's OpenGL stops at
# 4.1 and has no compute shaders -- so the dependencies come from Homebrew
# rather than vcpkg: MoltenVK, the Vulkan loader, glslang and GLFW. Dear ImGui,
# stb and Syphon are fetched by CMake at pinned versions.
#
#   tools/build-mac.sh            install what is missing, then build
#   tools/build-mac.sh --no-deps  build only
set -euo pipefail
cd "$(dirname "$0")/.."

if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode Command Line Tools are needed. Installing; re-run this afterwards."
    xcode-select --install || true
    exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is needed: https://brew.sh" >&2
    exit 1
fi

if [[ "${1:-}" != "--no-deps" ]]; then
    # ffmpeg is only for --video and the GUI's video input, but it is the
    # common case, so it comes along.
    for f in cmake ninja glfw glslang molten-vk vulkan-loader vulkan-headers ffmpeg; do
        brew list --versions "$f" >/dev/null 2>&1 || brew install "$f"
    done
fi

BREW="$(brew --prefix)"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BREW"
cmake --build build

echo
echo "Built build/gpu-sbr. Run it with ./run-mac.command, or:"
echo "  build/gpu-sbr --in photo.jpg"
