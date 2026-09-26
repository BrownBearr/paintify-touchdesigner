// The TouchDesigner live bridge on macOS, over Syphon.
//
// TouchDesigner's Syphon Spout In/Out TOPs speak Syphon on a Mac, so the
// component the installer builds is the same one Windows uses; only this side
// changes. Frames cross as IOSurfaces through Syphon's base classes, read and
// written by the CPU: the renderer's canvas lives in a Vulkan image behind
// MoltenVK, and a CPU hop keeps this file free of any Metal/Vulkan interop.
// At the default 12 painted frames per second that costs a few milliseconds a
// frame on Apple Silicon's unified memory, well under the painting itself.
#include "live.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonClientBase.h>
#import <Syphon/SyphonServerBase.h>
#import <Syphon/SyphonServerDirectory.h>
#import <Syphon/SyphonSubclassing.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <signal.h>
#include <unistd.h>
#include <vector>

namespace {

// IOSurface pixel format codes, as in CoreVideo's kCVPixelFormatType_*.
constexpr OSType kBGRA = 'BGRA';
constexpr OSType kRGBA = 'RGBA';

// Rows of `h` pixels from `src` to `dst`, swapping red and blue when asked.
void copyRows(const uint8_t* src, size_t srcStride, uint8_t* dst, size_t dstStride,
              size_t w, size_t h, bool swapRB) {
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* s = src + y * srcStride;
        uint8_t* d = dst + y * dstStride;
        if (!swapRB) {
            std::memcpy(d, s, w * 4);
            continue;
        }
        for (size_t x = 0; x < w; ++x) {
            d[x * 4 + 0] = s[x * 4 + 2];
            d[x * 4 + 1] = s[x * 4 + 1];
            d[x * 4 + 2] = s[x * 4 + 0];
            d[x * 4 + 3] = s[x * 4 + 3];
        }
    }
}

} // namespace

// A Syphon server fed from CPU memory. Syphon hands out a BGRA IOSurface; the
// painting is RGBA, so red and blue swap on the way in.
@interface PaintifySyphonServer : SyphonServerBase
- (void)publishRGBA:(const uint8_t*)pixels width:(size_t)width height:(size_t)height;
@end

@implementation PaintifySyphonServer
- (void)publishRGBA:(const uint8_t*)pixels width:(size_t)width height:(size_t)height {
    IOSurfaceRef surface = [self newSurfaceForWidth:width height:height options:nil];
    if (!surface) return;
    IOSurfaceLock(surface, 0, nullptr);
    copyRows(pixels, width * 4, static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface)),
             IOSurfaceGetBytesPerRow(surface), width, height, /*swapRB=*/true);
    IOSurfaceUnlock(surface, 0, nullptr);
    CFRelease(surface);
    [self publish];
}
@end

// A Syphon client that reads the server's current frame into CPU memory.
@interface PaintifySyphonClient : SyphonClientBase
- (BOOL)readRGBA:(std::vector<uint8_t>*)out width:(int*)width height:(int*)height;
@end

@implementation PaintifySyphonClient
- (BOOL)readRGBA:(std::vector<uint8_t>*)out width:(int*)width height:(int*)height {
    // newSurface also marks the frame as seen, which is what hasNewFrame
    // compares against.
    IOSurfaceRef surface = [self newSurface];
    if (!surface) return NO;
    const OSType format = IOSurfaceGetPixelFormat(surface);
    const size_t w = IOSurfaceGetWidth(surface), h = IOSurfaceGetHeight(surface);
    BOOL ok = NO;
    if ((format == kBGRA || format == kRGBA) && IOSurfaceGetBytesPerElement(surface) == 4 &&
        w > 0 && h > 0) {
        out->resize(w * h * 4);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
        copyRows(static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface)),
                 IOSurfaceGetBytesPerRow(surface), out->data(), w * 4, w, h,
                 /*swapRB=*/format == kBGRA);
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        *width = int(w);
        *height = int(h);
        ok = YES;
    } else {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr, "Syphon input is not 8-bit BGRA/RGBA (format %08x); "
                                 "set the Syphon Spout Out TOP to 8-bit\n", unsigned(format));
            warned = true;
        }
    }
    CFRelease(surface);
    return ok;
}
@end

namespace {

// The server TouchDesigner publishes under `name`. Syphon names are not
// unique and different hosts label them differently, so an exact server name
// wins, then "App - Name" / "App: Name" forms that some hosts show.
NSDictionary* findServer(NSString* name) {
    SyphonServerDirectory* dir = [SyphonServerDirectory sharedDirectory];
    NSArray* exact = [dir serversMatchingName:name appName:nil];
    if (exact.count) return exact.lastObject;
    for (NSDictionary* d in dir.servers) {
        NSString* server = d[SyphonServerDescriptionNameKey] ?: @"";
        NSString* app = d[SyphonServerDescriptionAppNameKey] ?: @"";
        NSString* dash = [NSString stringWithFormat:@"%@ - %@", app, server];
        NSString* colon = [NSString stringWithFormat:@"%@: %@", app, server];
        if ([dash isEqualToString:name] || [colon isEqualToString:name]) return d;
    }
    return nil;
}

bool parentGone(uint32_t pid) {
    if (!pid) return false;
    // Reparented to launchd means the parent is gone, whatever kill() says
    // about a PID that may since have been reused.
    if (getppid() == 1) return true;
    return kill(pid_t(pid), 0) != 0 && errno == ESRCH;
}

// Syphon announces servers through distributed notifications, which are
// delivered on this thread's run loop -- so waiting has to run it rather than
// sleep, or the directory never learns TouchDesigner's sender exists.
void runLoopFor(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

} // namespace

int runLive(Pipeline& pipe, TuningParams params, const RenderConfig& render,
            const LiveConfig& live) {
    @autoreleasepool {
        NSString* inputName = [NSString stringWithUTF8String:live.inputName.c_str()];
        NSString* outputName = [NSString stringWithUTF8String:live.outputName.c_str()];
        // Touching the directory sends the discovery request, so servers
        // that are already running announce themselves while we start up.
        (void)[SyphonServerDirectory sharedDirectory];
        PaintifySyphonServer* server =
            [[PaintifySyphonServer alloc] initWithName:outputName options:nil];
        if (!server) {
            std::fprintf(stderr, "could not start the Syphon server '%s'\n",
                         live.outputName.c_str());
            return 1;
        }
        PaintifySyphonClient* client = nil;

        std::vector<uint8_t> frame;
        int width = 0, height = 0;
        bool havePainting = false;
        bool announced = false;
        bool everConnected = false;
        unsigned long long paintedFrames = 0;
        auto lastConnected = std::chrono::steady_clock::now();
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / std::max(1.0, live.fps)));
        auto nextFrame = std::chrono::steady_clock::now();
        std::printf("Paintify live (Syphon): %s -> %s, %.1f fps\n",
                    live.inputName.c_str(), live.outputName.c_str(), live.fps);
        std::fflush(stdout);

        while (true) {
            @autoreleasepool {
                if (parentGone(live.parentPid)) break;
                if (!live.stopFile.empty() && std::filesystem::exists(live.stopFile)) break;

                const auto now = std::chrono::steady_clock::now();
                if (now < nextFrame) {
                    runLoopFor(0.002);
                    continue;
                }
                // Advance from the previous deadline so scheduling jitter does
                // not accumulate into a lower rate than asked for.
                nextFrame += interval;
                if (nextFrame < now) nextFrame = now;
                runLoopFor(0.0);

                if (client && !client.isValid) {
                    [client stop];
                    client = nil;
                    havePainting = false;
                }
                if (!client) {
                    NSDictionary* desc = findServer(inputName);
                    if (desc) {
                        client = [[PaintifySyphonClient alloc] initWithServerDescription:desc
                                                                                 options:nil
                                                                         newFrameHandler:nil];
                    }
                    if (!client) {
                        // The TD component may have been removed. Do not leave
                        // a hidden renderer running after its sender vanishes.
                        if (everConnected && now - lastConnected > std::chrono::seconds(5))
                            break;
                        if (!announced) {
                            std::printf("Waiting for Syphon server '%s'...\n",
                                        live.inputName.c_str());
                            std::fflush(stdout);
                            announced = true;
                        }
                        continue;
                    }
                    announced = false;
                    everConnected = true;
                }
                lastConnected = now;
                if (!client.hasNewFrame) continue;

                int w = 0, h = 0;
                if (![client readRGBA:&frame width:&w height:&h]) continue;
                if (w != width || h != height) {
                    // A new size is a new subject as far as temporal
                    // coherence goes; setSource reallocates for it.
                    width = w;
                    height = h;
                    havePainting = false;
                    pipe.resetTemporal();
                }

                if (!pipe.setSource(frame.data(), width, height)) continue;
                pipe.render(params, render, havePainting && params.frameDiffThreshold > 0.f);
                havePainting = true;
                params.frame += 1.f;

                const std::vector<unsigned char> painted = pipe.readCanvas();
                [server publishRGBA:painted.data() width:size_t(width) height:size_t(height)];
                if (++paintedFrames % 12 == 0) {
                    std::printf("Paintify live: %llu frames, %dx%d, GPU %.1f ms\n",
                                paintedFrames, width, height, pipe.msTotal());
                    std::fflush(stdout);
                }
            }
        }

        [client stop];
        [server stop];
    }
    return 0;
}
