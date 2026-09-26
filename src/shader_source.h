#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Shader files are read from the source tree at runtime rather than baked into
// the executable, so F5 reloads an edited shader without a rebuild. Both GPU
// backends load them through here.
namespace shadersrc {

// The shaders/ directory, with a trailing slash: $PAINTIFY_ROOT/shaders/ when
// that is set, otherwise the source tree the executable was built from.
std::string dir();

// Reads a shader from shaders/, splicing any `#include "x.glsl"` lines and
// emitting #line directives so compiler errors still point at the real file.
std::string load(const std::string& file, std::string* err);

// Modification time of shaders/<file>, 0 when it cannot be read.
int64_t mtime(const std::string& file);

// What a program's sources looked like when it was built: one entry per
// file, then the shared include.
std::vector<int64_t> mtimes(const std::vector<std::string>& files);

// common.glsl is included by every stage, so it is tracked as a dependency of
// every program rather than of the one file that names it.
extern const char* const kSharedInclude;

} // namespace shadersrc
