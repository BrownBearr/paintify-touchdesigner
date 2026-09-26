// The backend-independent half of gpu::Program: where a program's sources
// are and whether they changed. Compiling them is each backend's build().
#include "gpu.h"
#include "shader_source.h"

namespace gpu {

bool Program::loadCompute(const std::string& comp) {
    paths.clear();
    paths.push_back(comp);
    return build();
}

bool Program::loadRaster(const std::string& vert, const std::string& frag) {
    paths.clear();
    paths.push_back(vert);
    paths.push_back(frag);
    return build();
}

bool Program::sourcesChanged() const {
    return shadersrc::mtimes(paths) != mtimes;
}

bool Program::reloadIfChanged() {
    if (!sourcesChanged()) return false;
    return build();
}

} // namespace gpu
