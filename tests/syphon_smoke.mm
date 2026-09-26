// End-to-end check of the macOS live bridge, the Syphon counterpart of
// spout_smoke.cpp: publishes a test pattern as a Syphon server, starts
// `gpu-sbr --live` beside it, and waits for painted frames to come back.
//
//   paintify-syphon-smoke          640x360, 3 frames
//   paintify-syphon-smoke --full   1920x1080 with relaxation, 48 frames
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonClientBase.h>
#import <Syphon/SyphonServerBase.h>
#import <Syphon/SyphonServerDirectory.h>
#import <Syphon/SyphonSubclassing.h>

#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern char** environ;

@interface SmokeServer : SyphonServerBase
- (void)publishBGRA:(const uint8_t*)pixels width:(size_t)w height:(size_t)h;
@end

@implementation SmokeServer
- (void)publishBGRA:(const uint8_t*)pixels width:(size_t)w height:(size_t)h {
    IOSurfaceRef s = [self newSurfaceForWidth:w height:h options:nil];
    if (!s) return;
    IOSurfaceLock(s, 0, nullptr);
    uint8_t* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(s));
    const size_t stride = IOSurfaceGetBytesPerRow(s);
    for (size_t y = 0; y < h; ++y) std::memcpy(base + y * stride, pixels + y * w * 4, w * 4);
    IOSurfaceUnlock(s, 0, nullptr);
    CFRelease(s);
    [self publish];
}
@end

@interface SmokeClient : SyphonClientBase
- (BOOL)sampleAtX:(size_t)x y:(size_t)y into:(uint8_t*)out width:(size_t*)w height:(size_t*)h;
@end

@implementation SmokeClient
- (BOOL)sampleAtX:(size_t)x y:(size_t)y into:(uint8_t*)out width:(size_t*)w height:(size_t*)h {
    IOSurfaceRef s = [self newSurface];
    if (!s) return NO;
    *w = IOSurfaceGetWidth(s);
    *h = IOSurfaceGetHeight(s);
    BOOL ok = x < *w && y < *h;
    if (ok) {
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
        const uint8_t* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(s));
        std::memcpy(out, base + y * IOSurfaceGetBytesPerRow(s) + x * 4, 4);
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    }
    CFRelease(s);
    return ok;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        const bool full = argc > 1 && std::strcmp(argv[1], "--full") == 0;
        const size_t W = full ? 1920 : 640, H = full ? 1080 : 360;
        const int target = full ? 48 : 3;

        std::vector<uint8_t> pixels(W * H * 4);   // BGRA
        for (size_t y = 0; y < H; ++y) for (size_t x = 0; x < W; ++x) {
            uint8_t* p = &pixels[(y * W + x) * 4];
            p[2] = uint8_t(x * 255 / W);
            p[1] = uint8_t(y * 255 / H);
            p[0] = ((x / 32 + y / 32) % 2) ? 220 : 40;
            p[3] = 255;
        }

        (void)[SyphonServerDirectory sharedDirectory];
        SmokeServer* server = [[SmokeServer alloc] initWithName:@"Paintify Smoke Input" options:nil];
        if (!server) { std::fprintf(stderr, "could not start a Syphon server\n"); return 2; }

        // The renderer sits next to this executable.
        char self[4096];
        uint32_t size = sizeof(self);
        if (_NSGetExecutablePath(self, &size) != 0) return 3;
        const std::string renderer =
            (std::filesystem::path(self).parent_path() / "gpu-sbr").string();
        std::vector<std::string> args = {renderer, "--live", "--live-in", "Paintify Smoke Input",
                                         "--live-out", "Paintify Smoke Output",
                                         "--target-fps", "12"};
        if (full) { args.push_back("--relax"); args.push_back("4"); }
        std::vector<char*> cargs;
        for (std::string& a : args) cargs.push_back(&a[0]);
        cargs.push_back(nullptr);
        pid_t child = 0;
        if (posix_spawn(&child, renderer.c_str(), nullptr, nullptr, cargs.data(), environ) != 0) {
            std::fprintf(stderr, "could not launch %s\n", renderer.c_str());
            return 4;
        }

        SmokeClient* client = nil;
        int received = 0, sourceStep = 0;
        uint8_t lastSample[4] = {};
        bool haveSample = false;
        std::chrono::steady_clock::time_point firstPainted, lastPainted;
        // Generous: the first frame includes compiling every shader, twice
        // over on MoltenVK (GLSL to SPIR-V here, SPIR-V to Metal there).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < deadline && received < target) {
            @autoreleasepool {
                // Change the right half each step, as live footage would.
                const uint8_t swatch = uint8_t((sourceStep++ * 5) % 256);
                for (size_t y = 0; y < H; ++y) for (size_t x = W / 2; x < W; ++x) {
                    uint8_t* p = &pixels[(y * W + x) * 4];
                    p[0] = 210; p[1] = 60; p[2] = swatch;
                }
                [server publishBGRA:pixels.data() width:W height:H];

                if (!client) {
                    NSArray* found = [[SyphonServerDirectory sharedDirectory]
                        serversMatchingName:@"Paintify Smoke Output" appName:nil];
                    if (found.count)
                        client = [[SmokeClient alloc] initWithServerDescription:found.lastObject
                                                                        options:nil
                                                                newFrameHandler:nil];
                } else if (client.hasNewFrame) {
                    uint8_t sample[4] = {};
                    size_t w = 0, h = 0;
                    if ([client sampleAtX:3 * W / 4 y:H / 2 into:sample width:&w height:&h] &&
                        w == W && h == H &&
                        (!haveSample || std::memcmp(sample, lastSample, 4) != 0)) {
                        std::memcpy(lastSample, sample, 4);
                        haveSample = true;
                        if (received == 0) firstPainted = std::chrono::steady_clock::now();
                        lastPainted = std::chrono::steady_clock::now();
                        ++received;
                    }
                }
                // Feed faster than the painter's 12 fps cap, as a 30 fps
                // camera would -- and run the run loop, which is where Syphon's
                // announcements arrive.
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
            }
        }

        kill(child, SIGTERM);
        int status = 0;
        waitpid(child, &status, 0);
        [client stop];
        [server stop];

        const double elapsed = received > 1 ?
            std::chrono::duration<double>(lastPainted - firstPainted).count() : 0.0;
        std::printf("Received %d painted Syphon frames (%.1f fps after startup)\n",
                    received, elapsed > 0.0 ? (received - 1) / elapsed : 0.0);
        return received >= target ? 0 : 5;
    }
}
