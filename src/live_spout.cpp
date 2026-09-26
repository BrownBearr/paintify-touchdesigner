#include "live.h"

#ifdef _WIN32
// glad before anything else that might pull in the system GL headers.
#include <glad/glad.h>
#include <SpoutGL/Spout.h>
#include <GLFW/glfw3.h>

#include "display.h"
#include "gpu_gl.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#ifdef _WIN32
int runLive(Pipeline& pipe, TuningParams params, const RenderConfig& render,
            const LiveConfig& live) {
    GLFWwindow* window = display::window();
    Spout receiver;
    Spout sender;
    HANDLE parentProcess = live.parentPid ?
        OpenProcess(SYNCHRONIZE, FALSE, live.parentPid) : nullptr;
    receiver.SetReceiverName(live.inputName.c_str());
    sender.SetSenderName(live.outputName.c_str());

    gpu::Texture inputTexture = nullptr;
    int width = 0, height = 0;
    bool havePainting = false;
    bool announced = false;
    bool everConnected = false;
    unsigned long long paintedFrames = 0;
    auto lastConnected = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1.0, live.fps)));
    auto nextFrame = std::chrono::steady_clock::now();
    std::printf("Paintify live: %s -> %s, %.1f fps\n",
                live.inputName.c_str(), live.outputName.c_str(), live.fps);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (parentProcess && WaitForSingleObject(parentProcess, 0) == WAIT_OBJECT_0)
            break;
        if (!live.stopFile.empty() && std::filesystem::exists(live.stopFile))
            break;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        // Advance from the previous deadline so scheduling jitter does not
        // accumulate and quietly turn a 12 fps request into 11 fps.
        nextFrame += interval;
        if (nextFrame < now) nextFrame = now;

        // A null destination queries the sender first. On first connection or
        // resize, Spout requires us to allocate an RGBA texture and try again.
        if (!inputTexture) {
            if (!receiver.ReceiveTexture()) {
                // The TD component may have been removed. Avoid leaving a
                // hidden renderer process running after its sender vanishes.
                if (everConnected && now - lastConnected > std::chrono::seconds(5))
                    break;
                if (!announced) {
                    std::printf("Waiting for Spout sender '%s'...\n", live.inputName.c_str());
                    announced = true;
                }
                continue;
            }
            announced = false;
            everConnected = true;
            lastConnected = now;
            width = int(receiver.GetSenderWidth());
            height = int(receiver.GetSenderHeight());
            if (width <= 0 || height <= 0) continue;
            inputTexture = gpu::createTexture2D(width, height, gpu::Format::RGBA8);
            receiver.IsUpdated(); // reset Spout's resize flag
            havePainting = false;
            pipe.resetTemporal();
        }

        if (!receiver.ReceiveTexture(gpu::gl::textureId(inputTexture), GL_TEXTURE_2D)) {
            gpu::destroy(inputTexture);
            havePainting = false;
            sender.ReleaseSender();
            continue;
        }
        lastConnected = now;
        if (receiver.IsUpdated()) {
            gpu::destroy(inputTexture);
            havePainting = false;
            sender.ReleaseSender();
            continue;
        }
        if (!receiver.IsFrameNew()) continue;

        if (!pipe.setSourceTexture(inputTexture)) continue;
        pipe.render(params, render, havePainting && params.frameDiffThreshold > 0.f);
        havePainting = true;
        params.frame += 1.f;
        gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierFramebuffer);
        if (!sender.SendTexture(gpu::gl::textureId(pipe.canvasTexture()), GL_TEXTURE_2D,
                                unsigned(width), unsigned(height))) {
            std::fprintf(stderr, "Spout failed to publish painted frame\n");
        } else if (++paintedFrames % 12 == 0) {
            std::printf("Paintify live: %llu frames, %dx%d, GPU %.1f ms\n",
                        paintedFrames, width, height, pipe.msTotal());
            std::fflush(stdout);
        }
    }

    gpu::destroy(inputTexture);
    receiver.ReleaseReceiver();
    sender.ReleaseSender();
    if (parentProcess) CloseHandle(parentProcess);
    return 0;
}
#else
// Neither Spout nor Syphon: macOS builds live_syphon.mm instead of this file,
// so this is Linux, which has no TouchDesigner.
int runLive(Pipeline&, TuningParams, const RenderConfig&, const LiveConfig&) {
    std::fprintf(stderr, "Live mode needs Spout (Windows) or Syphon (macOS)\n");
    return 1;
}
#endif
