#pragma once
#include "pipeline.h"

#include <string>

struct GLFWwindow;
struct ImDrawData;

// The window, the GPU context behind it, ImGui, and putting the canvas on
// screen. One implementation per GPU backend (display_gl.cpp, display_vk.cpp),
// so main() does not need to know which one it is running on.
namespace display {

// Brings up the GPU and, for an interactive run, a window to show it in.
// On OpenGL a headless run still gets a hidden window, because that is what
// owns the context. On Vulkan a headless run creates no window at all, which
// is what lets the renderer run with no display attached.
bool open(bool interactive, int width, int height, const char* title, std::string* err);
void close();

// Null for a headless run on the Vulkan backend.
GLFWwindow* window();

void imguiInit();
// The backend's and GLFW's NewFrame. The caller then calls ImGui::NewFrame().
void imguiNewFrame();
void imguiShutdown();

// A rectangle of the window, in framebuffer pixels, with a top-left origin.
struct Rect { int x, y, w, h; };

// Shows the canvas in `area` framed by `xf` (letterboxed, and as a wipe
// against the source in Split mode), draws `ui` over the whole window, and
// presents. `ui` may be null.
void present(const Pipeline& pipe, Rect area, ViewMode mode, const ViewXform& xf,
             ImDrawData* ui);

} // namespace display
