#include "display.h"
#include "gpu_gl.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>

namespace display {
namespace {

GLFWwindow* g_win = nullptr;

// Read framebuffers for the blit, rebuilt whenever the pipeline reallocates
// the texture behind one (a resize, or a new source).
struct ReadFbo {
    GLuint fbo = 0;
    GLuint tex = 0;
    GLuint get(gpu::Texture t) {
        const GLuint id = gpu::gl::textureId(t);
        if (id != tex || !fbo) {
            if (!fbo) glCreateFramebuffers(1, &fbo);
            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, id, 0);
            tex = id;
        }
        return fbo;
    }
    void release() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        fbo = 0;
        tex = 0;
    }
};

ReadFbo g_srcFbo, g_canvasFbo;

// The canvas blit, in GL's bottom-left window coordinates.
void blitToScreen(const Pipeline& pipe, int x, int y, int w, int h,
                  ViewMode mode, const ViewXform& xf) {
    const int iw = pipe.width(), ih = pipe.height();
    if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0) return;

    // Fit preserves aspect. It did not before, which stretched every preview to
    // the shape of the window -- and stroke *shape* is most of what is being
    // judged here, so a horizontal squash reads as a change to the painting.
    const float fit = std::min(float(w) / float(iw), float(h) / float(ih));
    const float scale = fit * std::max(xf.zoom, 0.01f);
    const float fx = xf.focusX < 0.f ? float(iw) * 0.5f : xf.focusX;
    const float fy = xf.focusY < 0.f ? float(ih) * 0.5f : xf.focusY;

    // Window y grows upwards and image y downwards, hence the sign flip on the
    // y term: windowY(iy) = cy - (iy - fy) * scale.
    const float cx = float(x) + float(w) * 0.5f;
    const float cy = float(y) + float(h) * 0.5f;
    const int dx0   = int(std::lround(cx - fx * scale));
    const int dx1   = int(std::lround(cx + (float(iw) - fx) * scale));
    const int dyTop = int(std::lround(cy + fy * scale));
    const int dyBot = int(std::lround(cy - (float(ih) - fy) * scale));

    // Past 1:1 the point of zooming is to inspect brush texture and impasto
    // relief, and a linear filter there invents detail the canvas does not
    // have. Below 1:1 linear is the honest choice instead.
    const GLenum filter = (scale > 1.f) ? GL_NEAREST : GL_LINEAR;

    // The destination rect runs outside the viewport whenever the view is
    // zoomed in, and glBlitFramebuffer clips only against the framebuffer --
    // without a scissor the image would paint straight over the control panel.
    // The scissor is also what makes the split a wipe rather than two blits.
    GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint oldScissor[4];
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glEnable(GL_SCISSOR_TEST);

    // Source rect is flipped in y: canvas texel row 0 is image row 0, while the
    // default framebuffer's origin is its bottom-left. Orientation is applied
    // here, at the edge, never inside the pipeline.
    auto blitRegion = [&](GLuint fbo, int sx, int sw) {
        if (sw <= 0) return;
        glScissor(sx, y, sw, h);
        glBlitNamedFramebuffer(fbo, 0, 0, ih, iw, 0, dx0, dyBot, dx1, dyTop,
                               GL_COLOR_BUFFER_BIT, filter);
    };

    // Letterbox. Nothing cleared here before, because the blit always covered
    // the whole viewport; an aspect-correct one does not.
    glScissor(x, y, w, h);
    glClearColor(0.09f, 0.09f, 0.10f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    const GLuint srcFbo = g_srcFbo.get(pipe.sourceTexture());
    const GLuint canvasFbo = g_canvasFbo.get(pipe.canvasTexture());
    switch (mode) {
        case ViewMode::Source: blitRegion(srcFbo, x, w); break;
        case ViewMode::Split: {
            // A wipe, not two shrunken copies side by side: both halves share
            // one framing, so a feature sits at the same place on either side
            // of the divider. Two independently fitted copies cannot be
            // compared at all once the view is zoomed in.
            const int cut = std::clamp(int(float(w) * xf.wipe), 0, w);
            blitRegion(srcFbo, x, cut);
            blitRegion(canvasFbo, x + cut, w - cut);
            break;
        }
        default: blitRegion(canvasFbo, x, w); break;
    }

    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (!hadScissor) glDisable(GL_SCISSOR_TEST);
}

} // namespace

bool open(bool interactive, int width, int height, const char* title, std::string* err) {
    if (!glfwInit()) { if (err) *err = "glfwInit failed"; return false; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    if (!interactive) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    g_win = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!g_win) {
        if (err) *err = "need an OpenGL 4.6 core context";
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(g_win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        if (err) *err = "glad failed";
        return false;
    }
    glfwSwapInterval(0);   // vsync off: the overlay is meant to show real cost

    gpu::InitOptions opt;
    return gpu::init(opt, err);
}

void close() {
    g_srcFbo.release();
    g_canvasFbo.release();
    gpu::shutdown();
    if (g_win) glfwDestroyWindow(g_win);
    g_win = nullptr;
    glfwTerminate();
}

GLFWwindow* window() { return g_win; }

void imguiInit() {
    ImGui_ImplGlfw_InitForOpenGL(g_win, true);
    ImGui_ImplOpenGL3_Init("#version 460");
}

void imguiNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void imguiShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
}

void present(const Pipeline& pipe, Rect area, ViewMode mode, const ViewXform& xf,
             ImDrawData* ui) {
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(g_win, &fbW, &fbH);
    blitToScreen(pipe, area.x, fbH - (area.y + area.h), area.w, area.h, mode, xf);
    if (ui) ImGui_ImplOpenGL3_RenderDrawData(ui);
    glfwSwapBuffers(g_win);
}

} // namespace display
