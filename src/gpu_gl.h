#pragma once
// What the OpenGL backend exposes beyond gpu.h, for the code that has to meet
// GL on its own terms: the on-screen blit and Spout, which shares textures by
// GL name.
#include "gpu.h"

namespace gpu {
namespace gl {

unsigned textureId(Texture t);

} // namespace gl
} // namespace gpu
