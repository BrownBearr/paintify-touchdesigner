// Paintify for macOS: the Metal build of gpu-sbr.
//
//   paintify-mac --live-syphon --spout-in <name> --spout-out <name> ...   (TouchDesigner)
//   paintify-mac --in photo.jpg --out painted.png [--preset wash ...]      (one still)
//
// Option parsing, presets and the settings-file protocol are shared with the
// Windows renderer (src/options.cpp), so the TouchDesigner component drives
// both the same way.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include "metal_pipeline.h"
#include "syphon_live.h"
#include "options.h"
#include "paramfile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <mach-o/dyld.h>

namespace {

namespace fs = std::filesystem;

std::string executableDir() {
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return (ec ? fs::path(buf) : p).parent_path().string();
}

// paintify.metallib: --metallib, then PAINTIFY_SHADER_DIR (set by the TOX),
// then beside the executable.
std::string findMetallib(const Options& opt) {
    if (!opt.metallib.empty()) return opt.metallib;
    std::vector<fs::path> candidates;
    if (const char* dir = std::getenv("PAINTIFY_SHADER_DIR"); dir && *dir)
        candidates.push_back(fs::path(dir) / "paintify.metallib");
    const fs::path exe = executableDir();
    candidates.push_back(exe / "paintify.metallib");
    candidates.push_back(exe / "shaders" / "paintify.metallib");
    for (const fs::path& c : candidates)
        if (fs::exists(c)) return c.string();
    return candidates.front().string();
}

bool loadImage(const std::string& path, std::vector<unsigned char>* rgba, int* w, int* h) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    *w = int(CGImageGetWidth(img));
    *h = int(CGImageGetHeight(img));
    rgba->assign(size_t(*w) * size_t(*h) * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba->data(), size_t(*w), size_t(*h), 8,
                                             size_t(*w) * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(cs);
    if (!ctx) { CGImageRelease(img); return false; }
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    return true;
}

bool writePng(const std::string& path, const std::vector<unsigned char>& rgba, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault,
                                   provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!img) return false;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                                CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(img);
    return ok;
}

// Same fallback subject as the OpenGL build, so stills are comparable.
std::vector<unsigned char> syntheticImage(int w, int h) {
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = float(x) / w, v = float(y) / h;
            const float swirl = std::sin((u * 6.0f + v * 3.0f) * 3.14159f)
                              * std::cos((v * 5.0f - u * 2.0f) * 3.14159f);
            float r = 0.5f + 0.45f * swirl;
            float g = 0.45f + 0.4f * std::sin(u * 9.0f + swirl * 2.0f);
            float b = 0.55f + 0.4f * std::cos(v * 7.0f - swirl * 2.0f);
            const float dx = u - 0.35f, dy = v - 0.45f;
            if (dx * dx + dy * dy < 0.02f) { r = 0.95f; g = 0.35f; b = 0.2f; }
            if (std::fabs(u - 0.72f) < 0.06f && v > 0.25f && v < 0.8f) { r = 0.1f; g = 0.15f; b = 0.3f; }
            const size_t i = (size_t(y) * w + x) * 4;
            px[i + 0] = (unsigned char)(r * 255.f);
            px[i + 1] = (unsigned char)(g * 255.f);
            px[i + 2] = (unsigned char)(b * 255.f);
            px[i + 3] = 255;
        }
    }
    return px;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const Options opt = parseArgs(argc, argv);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::fprintf(stderr, "no Metal device available\n");
            return 1;
        }
        std::printf("Metal | %s\n", [[device name] UTF8String]);

        MetalPipeline pipe;
        std::string err;
        const std::string metallib = findMetallib(opt);
        if (!pipe.init(device, metallib, &err)) {
            std::fprintf(stderr, "Metal pipeline init failed: %s\n", err.c_str());
            return 1;
        }

        TuningParams params;
        RenderConfig cfg;
        std::string radiiText = paramfile::radiiToString(cfg.radii);
        resolveLook(opt, params, cfg, &radiiText, nullptr);

        if (!opt.saveParamsFile.empty()) {
            std::string perr;
            if (!paramfile::save(opt.saveParamsFile, params, cfg, &perr))
                std::fprintf(stderr, "%s\n", perr.c_str());
            else
                std::printf("wrote %s\n", opt.saveParamsFile.c_str());
        }

        if (opt.liveSpout) {
            LiveSyphonConfig live;
            live.inputName = opt.spoutIn;
            live.outputName = opt.spoutOut;
            live.fps = opt.targetFps;
            live.stopFile = opt.liveStopFile;
            live.parentPid = opt.liveParentPid;
            live.flipIn = opt.flipIn;
            live.flipOut = opt.flipOut;
            if (!opt.liveSettingsFile.empty())
                live.reloadSettings = makeLiveReloader(opt.liveSettingsFile);
            const int result = runLiveSyphon(device, pipe, params, cfg, live);
            pipe.shutdown();
            return result;
        }

        // One still image.
        std::vector<unsigned char> px;
        int w = 0, h = 0;
        if (opt.in.empty() || !loadImage(opt.in, &px, &w, &h)) {
            if (!opt.in.empty())
                std::fprintf(stderr, "could not read %s; using the synthetic subject\n", opt.in.c_str());
            w = 1920; h = 1080;
            px = syntheticImage(w, h);
        }
        pipe.setSource(px.data(), w, h);
        pipe.render(params, cfg, false);
        const std::vector<unsigned char> out = pipe.readCanvas();
        std::printf("painted %dx%d, GPU %.1f ms\n", w, h, pipe.lastGpuMs());
        if (!writePng(opt.out, out, w, h)) {
            std::fprintf(stderr, "could not write %s\n", opt.out.c_str());
            return 1;
        }
        std::printf("wrote %s\n", opt.out.c_str());
        pipe.shutdown();
        return 0;
    }
}
