#include "syphon_live.h"

#import <Foundation/Foundation.h>
#import <Syphon/Syphon.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <unistd.h>

namespace {

bool processAlive(uint32_t pid) {
    if (pid == 0) return true;
    return kill(pid_t(pid), 0) == 0 || errno != ESRCH;
}

} // namespace

int runLiveSyphon(id<MTLDevice> device, MetalPipeline& pipe, TuningParams params,
                  const RenderConfig& render, const LiveSyphonConfig& live) {
    @autoreleasepool {
        NSString* inputName = [NSString stringWithUTF8String:live.inputName.c_str()];
        NSString* outputName = [NSString stringWithUTF8String:live.outputName.c_str()];

        SyphonMetalServer* server = [[SyphonMetalServer alloc] initWithName:outputName
                                                                     device:device
                                                                    options:nil];
        if (!server) {
            std::fprintf(stderr, "Syphon: could not start server '%s'\n", live.outputName.c_str());
            return 1;
        }

        SyphonMetalClient* client = nil;
        std::atomic<bool> newFrame{false};
        std::atomic<bool>* newFrameFlag = &newFrame;

        bool announced = false;
        bool everConnected = false;
        bool forceRepaint = false;
        bool havePainting = false;
        unsigned long long paintedFrames = 0;
        auto lastConnected = std::chrono::steady_clock::now();
        RenderConfig currentRender = render;
        double targetFps = live.fps;
        auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(1.0, targetFps)));
        auto nextFrame = std::chrono::steady_clock::now();
        std::printf("Paintify live (Metal): %s -> %s, %.1f fps\n",
                    live.inputName.c_str(), live.outputName.c_str(), live.fps);
        std::fflush(stdout);

        int result = 0;
        while (true) {
            @autoreleasepool {
                // Syphon learns about servers through distributed notifications,
                // which only arrive while the run loop is turning.
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];

                if (!processAlive(live.parentPid)) break;
                if (!live.stopFile.empty() && std::filesystem::exists(live.stopFile)) break;

                const auto now = std::chrono::steady_clock::now();
                if (now < nextFrame) continue;

                if (live.reloadSettings && live.reloadSettings(params, currentRender, targetFps)) {
                    interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(1.0 / std::max(1.0, targetFps)));
                    havePainting = false;
                    pipe.resetTemporal();
                    forceRepaint = true;   // a still input never sends a new frame
                }
                // Advance from the previous deadline so scheduling jitter does
                // not accumulate and quietly turn 12 fps into 11.
                nextFrame += interval;
                if (nextFrame < now) nextFrame = now;

                if (client && ![client isValid]) {
                    [client stop];
                    client = nil;
                    havePainting = false;
                    pipe.resetTemporal();
                }
                if (!client) {
                    NSArray* servers = [[SyphonServerDirectory sharedDirectory]
                                        serversMatchingName:inputName appName:nil];
                    if (servers.count == 0) {
                        // The TD component may have been removed. Avoid leaving a
                        // hidden renderer running after its sender vanishes.
                        if (everConnected && now - lastConnected > std::chrono::seconds(5)) break;
                        if (!announced) {
                            std::printf("Waiting for Syphon server '%s'...\n", live.inputName.c_str());
                            std::fflush(stdout);
                            announced = true;
                        }
                        continue;
                    }
                    client = [[SyphonMetalClient alloc]
                              initWithServerDescription:servers.lastObject
                                                 device:device
                                                options:nil
                                        newFrameHandler:^(SyphonMetalClient*) {
                                            newFrameFlag->store(true);
                                        }];
                    if (!client || ![client isValid]) {
                        client = nil;
                        continue;
                    }
                    announced = false;
                    everConnected = true;
                    forceRepaint = true;
                    std::printf("Paintify live: connected to Syphon server '%s'\n",
                                live.inputName.c_str());
                    std::fflush(stdout);
                }
                lastConnected = now;

                if (!newFrame.exchange(false) && !forceRepaint) continue;

                id<MTLTexture> frame = [client newFrameImage];
                if (!frame) continue;
                forceRepaint = false;
                const int w = int(frame.width), h = int(frame.height);
                if (w <= 0 || h <= 0) continue;

                if (!pipe.setSourceTexture(frame, w, h, live.flipIn)) continue;
                pipe.render(params, currentRender, havePainting && params.frameDiffThreshold > 0.f);
                havePainting = true;
                params.frame += 1.f;

                id<MTLTexture> out = pipe.encodeOutput();
                [server publishFrameTexture:out
                            onCommandBuffer:pipe.commandBuffer()
                                imageRegion:NSMakeRect(0, 0, w, h)
                                    flipped:live.flipOut ? YES : NO];
                pipe.commit(true);

                if (++paintedFrames % 12 == 0) {
                    std::printf("Paintify live: %llu frames, %dx%d, GPU %.1f ms\n",
                                paintedFrames, w, h, pipe.lastGpuMs());
                    std::fflush(stdout);
                }
            }
        }

        [client stop];
        [server stop];
        return result;
    }
}
