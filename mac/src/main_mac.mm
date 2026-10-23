// macOS entry point for the Paintify renderer.
//
// Two modes, matching the parts of src/main.cpp a Mac build needs:
//   --in a.png --out b.png     paint one still (verification, benchmarks)
//   --live-syphon              the TouchDesigner bridge, over Syphon
//
// The presets, option parsing and live settings protocol are main.cpp's,
// value for value and flag for flag, so the TouchDesigner component drives
// both platforms with the same arguments. `--live-spout`, `--spout-in` and
// `--spout-out` are accepted as aliases for the Syphon spellings.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/Syphon.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "mtl_pipeline.h"
#include "paramfile.h"
#include "params.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mach-o/dyld.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ── Presets: copied from src/main.cpp; keep the two tables identical. ─────
struct Preset {
    const char* name;
    const char* radii;
    float maxLen, minLen, curvature, threshold, gridFactor, opacity;
    float hue, sat, val;
    float brushTexture;
    Underpaint underpaint;
};

const Preset kPresets[] = {
    {"impressionist", "8,4,2",    16,  4,   1.0f, 50.f, 1.0f,  0.90f, 0.05f, 0.10f, 0.10f, 0.45f, Underpaint::Blur},
    {"expressionist", "12,6,3",   28,  8,   1.0f, 40.f, 0.9f,  0.95f, 0.15f, 0.20f, 0.15f, 0.60f, Underpaint::Blur},
    {"pointillist",   "4,2",       3,  1,   0.0f, 30.f, 0.75f, 0.85f, 0.10f, 0.15f, 0.10f, 0.00f, Underpaint::Average},
    {"wash",          "20,10",    32, 10,   0.7f, 80.f, 1.5f,  0.40f, 0.20f, 0.30f, 0.20f, 0.00f, Underpaint::Blur},
    {"detail",        "6,3,1.5",  10,  4,   1.0f, 20.f, 1.0f,  1.00f, 0.00f, 0.00f, 0.00f, 0.50f, Underpaint::Blur},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

void applyPreset(const Preset& pr, TuningParams& p, RenderConfig& cfg) {
    cfg.radii = paramfile::parseRadii(pr.radii);
    cfg.underpaint = pr.underpaint;
    p.maxStrokeLength = pr.maxLen;
    p.minStrokeLength = pr.minLen;
    p.curvature = pr.curvature;
    p.threshold = pr.threshold;
    p.gridFactor = pr.gridFactor;
    p.opacity = pr.opacity;
    p.jitterHue = pr.hue;
    p.jitterSat = pr.sat;
    p.jitterVal = pr.val;
    p.texStrength = pr.brushTexture;
}

struct Options {
    bool live = false;
    std::string syphonIn = "Paintify Input";
    std::string syphonOut = "Paintify Output";
    std::string liveStopFile;
    std::string liveSettingsFile;
    uint32_t liveParentPid = 0;
    double targetFps = 12.0;
    bool flipIn = false, flipOut = false;
    std::string in;
    std::string out = "out.png";
    int repeat = 1;
    int passes = 0;
    std::string preset;
    std::string paramsFile;
    std::string radii;
    bool haveUnderpaint = false;
    Underpaint underpaint = Underpaint::Blur;
    float threshold = NAN, curvature = NAN, opacity = NAN, gridFactor = NAN;
    float maxLen = NAN, minLen = NAN, tensorSigma = NAN;
    float brushTexture = NAN, bristleDensity = NAN, textureTaper = NAN;
    float impasto = NAN, impastoLight = NAN, lightAngle = NAN, dryBrush = NAN;
    float sizeJitter = NAN, angleJitter = NAN, opacityJitter = NAN;
    float temporalDiff = NAN;
    int relax = -1, relaxSubs = 0;
    int etf = -1;
    int flow = -1, flowIters = 0;
    float etfRadius = NAN;
    float relaxArea = NAN, relaxMove = NAN, relaxCands = NAN, relaxRemove = NAN;
    bool jitterPerFrame = false;
};

void usage() {
    printf(
        "gpu-sbr (macOS / Metal)\n\n"
        "  --in <png> --out <png>   paint one image\n"
        "  --repeat <n>             render it n times (timing; temporal with --temporal-diff)\n"
        "  --live-syphon            paint live Syphon input and publish Syphon output\n"
        "  --syphon-in <name>       input server name (Paintify Input)\n"
        "  --syphon-out <name>      output server name (Paintify Output)\n"
        "  --flip-in / --flip-out   flip rows on the way in / out\n"
        "  --target-fps <n>         live painted frames per second (12)\n"
        "  --live-stop-file <path>  exit live mode when this file appears\n"
        "  --live-settings-file <p> reload live controls from this file\n"
        "  --live-parent-pid <n>    exit if the owning process closes\n"
        "  --preset <name>          impressionist | expressionist | pointillist | wash | detail\n"
        "  --params <file>          load a saved look (between preset and flags)\n"
        "\n"
        "Painting flags are the Windows renderer's: --radii --threshold --curvature\n"
        "--opacity --grid-factor --max-len --min-len --tensor-sigma --brush-texture\n"
        "--bristle-density --texture-taper --impasto --impasto-light --light-angle\n"
        "--dry-brush --size-jitter --angle-jitter --opacity-jitter --temporal-diff\n"
        "--underpaint --passes --relax --relax-* --etf --etf-radius --flow\n"
        "--flow-iters --jitter-per-frame\n");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--live-syphon" || a == "--live-spout") o.live = true;
        else if (a == "--syphon-in" || a == "--spout-in")     o.syphonIn = next(i);
        else if (a == "--syphon-out" || a == "--spout-out")   o.syphonOut = next(i);
        else if (a == "--flip-in")         o.flipIn = true;
        else if (a == "--flip-out")        o.flipOut = true;
        else if (a == "--target-fps")      o.targetFps = atof(next(i));
        else if (a == "--live-stop-file")  o.liveStopFile = next(i);
        else if (a == "--live-settings-file") o.liveSettingsFile = next(i);
        else if (a == "--live-parent-pid") o.liveParentPid = uint32_t(std::strtoul(next(i), nullptr, 10));
        else if (a == "--in")              o.in = next(i);
        else if (a == "--out")             o.out = next(i);
        else if (a == "--repeat")          o.repeat = std::max(1, atoi(next(i)));
        else if (a == "--passes")          o.passes = atoi(next(i));
        else if (a == "--relax")           o.relax = atoi(next(i));
        else if (a == "--etf")             o.etf = atoi(next(i));
        else if (a == "--flow")            o.flow = atoi(next(i));
        else if (a == "--flow-iters")      o.flowIters = atoi(next(i));
        else if (a == "--etf-radius")      o.etfRadius = std::strtof(next(i), nullptr);
        else if (a == "--relax-subpasses") o.relaxSubs = atoi(next(i));
        else if (a == "--relax-area")      o.relaxArea = std::strtof(next(i), nullptr);
        else if (a == "--relax-move")      o.relaxMove = std::strtof(next(i), nullptr);
        else if (a == "--relax-candidates") o.relaxCands = std::strtof(next(i), nullptr);
        else if (a == "--relax-remove")    o.relaxRemove = std::strtof(next(i), nullptr);
        else if (a == "--jitter-per-frame") o.jitterPerFrame = true;
        else if (a == "--preset")          o.preset = next(i);
        else if (a == "--params")          o.paramsFile = next(i);
        else if (a == "--radii")           o.radii = next(i);
        else if (a == "--threshold")       o.threshold = std::strtof(next(i), nullptr);
        else if (a == "--curvature")       o.curvature = std::strtof(next(i), nullptr);
        else if (a == "--opacity")         o.opacity = std::strtof(next(i), nullptr);
        else if (a == "--grid-factor")     o.gridFactor = std::strtof(next(i), nullptr);
        else if (a == "--max-len")         o.maxLen = std::strtof(next(i), nullptr);
        else if (a == "--min-len")         o.minLen = std::strtof(next(i), nullptr);
        else if (a == "--tensor-sigma")    o.tensorSigma = std::strtof(next(i), nullptr);
        else if (a == "--brush-texture")   o.brushTexture = std::strtof(next(i), nullptr);
        else if (a == "--bristle-density") o.bristleDensity = std::strtof(next(i), nullptr);
        else if (a == "--texture-taper")   o.textureTaper = std::strtof(next(i), nullptr);
        else if (a == "--impasto")         o.impasto = std::strtof(next(i), nullptr);
        else if (a == "--impasto-light")   o.impastoLight = std::strtof(next(i), nullptr);
        else if (a == "--light-angle")     o.lightAngle = std::strtof(next(i), nullptr);
        else if (a == "--dry-brush")       o.dryBrush = std::strtof(next(i), nullptr);
        else if (a == "--size-jitter")     o.sizeJitter = std::strtof(next(i), nullptr);
        else if (a == "--angle-jitter")    o.angleJitter = std::strtof(next(i), nullptr);
        else if (a == "--opacity-jitter")  o.opacityJitter = std::strtof(next(i), nullptr);
        else if (a == "--temporal-diff")   o.temporalDiff = std::strtof(next(i), nullptr);
        else if (a == "--underpaint") {
            const std::string m = next(i);
            o.haveUnderpaint = true;
            o.underpaint = (m == "none") ? Underpaint::None
                         : (m == "average") ? Underpaint::Average : Underpaint::Blur;
        }
        else if (a == "--no-underpaint") { o.haveUnderpaint = true; o.underpaint = Underpaint::None; }
        else if (a == "--help" || a == "-h") { usage(); exit(0); }
        else fprintf(stderr, "unknown option %s (try --help)\n", a.c_str());
    }
    return o;
}

void applyOverrides(const Options& o, TuningParams& p, RenderConfig& cfg) {
    auto set = [](float& dst, float v) { if (!std::isnan(v)) dst = v; };
    if (!o.radii.empty()) cfg.radii = paramfile::parseRadii(o.radii);
    if (o.haveUnderpaint) cfg.underpaint = o.underpaint;
    if (o.passes > 0) cfg.passesPerLayer = o.passes;
    set(p.threshold, o.threshold);
    set(p.curvature, o.curvature);
    set(p.opacity, o.opacity);
    set(p.gridFactor, o.gridFactor);
    set(p.maxStrokeLength, o.maxLen);
    set(p.minStrokeLength, o.minLen);
    set(p.tensorSigma, o.tensorSigma);
    set(p.texStrength, o.brushTexture);
    set(cfg.bristleDensity, o.bristleDensity);
    set(p.texTaper, o.textureTaper);
    set(p.impastoStrength, o.impasto);
    set(p.impastoLight, o.impastoLight);
    set(p.lightAngle, o.lightAngle);
    set(p.dryBrush, o.dryBrush);
    set(p.sizeJitter, o.sizeJitter);
    set(p.angleJitter, o.angleJitter);
    set(p.opacityJitter, o.opacityJitter);
    set(p.frameDiffThreshold, o.temporalDiff);
    if (o.etf >= 0) cfg.etfIterations = o.etf;
    if (o.flow >= 0) cfg.flowLevels = o.flow;
    if (o.flowIters > 0) cfg.flowIterations = o.flowIters;
    set(p.etfRadius, o.etfRadius);
    if (o.relax >= 0) cfg.relaxIterations = o.relax;
    if (o.relaxSubs > 0) cfg.relaxSubPasses = o.relaxSubs;
    set(p.relaxAreaWeight, o.relaxArea);
    set(p.relaxMoveScale, o.relaxMove);
    set(p.relaxCandidates, o.relaxCands);
    set(p.relaxRemove, o.relaxRemove);
    p.jitterPerFrame = o.jitterPerFrame ? 1.f : 0.f;
}

void resolveLook(const Options& opt, TuningParams& params, RenderConfig& cfg) {
    applyPreset(kPresets[0], params, cfg);
    if (!opt.preset.empty()) {
        bool found = false;
        for (int i = 0; i < kPresetCount; ++i) {
            if (opt.preset == kPresets[i].name) {
                applyPreset(kPresets[i], params, cfg);
                found = true;
                break;
            }
        }
        if (!found) fprintf(stderr, "unknown preset '%s'; using impressionist\n", opt.preset.c_str());
    }
    if (!opt.paramsFile.empty()) {
        std::string perr;
        if (!paramfile::load(opt.paramsFile, &params, &cfg, &perr))
            fprintf(stderr, "%s\n", perr.c_str());
    }
    applyOverrides(opt, params, cfg);
}

// Shaders live next to the executable unless PAINTIFY_SHADER_DIR says
// otherwise -- the TouchDesigner runtime sets it to its extraction folder.
std::string shaderDir() {
    const char* dir = std::getenv("PAINTIFY_SHADER_DIR");
    if (dir && *dir) return dir;
    char buf[4096];
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) == 0)
        return (std::filesystem::path(buf).parent_path() / "shaders").string();
    return "shaders";
}

bool parentAlive(uint32_t pid) {
    if (!pid) return true;
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;
}

// The live settings file is the Windows protocol: one argument per line,
// re-resolved from scratch (preset, then overrides) whenever it changes.
bool reloadSettings(const std::string& path, std::string& previous,
                    TuningParams& current, RenderConfig& config, double& fps) {
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (contents.empty() || contents == previous) return false;
    std::vector<std::string> arguments{"gpu-sbr"};
    std::istringstream lines(contents);
    std::string value;
    while (std::getline(lines, value)) {
        if (!value.empty() && value.back() == '\r') value.pop_back();
        if (!value.empty()) arguments.push_back(value);
    }
    std::vector<char*> argv;
    for (std::string& a : arguments) argv.push_back(a.data());
    const Options updated = parseArgs(int(argv.size()), argv.data());
    TuningParams fresh;
    RenderConfig freshConfig;
    resolveLook(updated, fresh, freshConfig);
    fresh.frame = current.frame;
    current = fresh;
    config = freshConfig;
    fps = updated.targetFps;
    previous = contents;
    printf("Paintify live settings updated\n");
    fflush(stdout);
    return true;
}

int runLive(MetalPipeline& pipe, TuningParams params, RenderConfig cfg, const Options& opt) {
    id<MTLDevice> dev = pipe.device();
    SyphonMetalServer* server = [[SyphonMetalServer alloc] initWithName:@(opt.syphonOut.c_str())
                                                                 device:dev options:nil];
    SyphonMetalClient* client = nil;
    NSString* inputName = @(opt.syphonIn.c_str());

    bool havePainting = false, announced = false, everConnected = false;
    unsigned long long paintedFrames = 0;
    int width = 0, height = 0;
    double targetFps = opt.targetFps;
    std::string previousSettings;
    reloadSettings(opt.liveSettingsFile, previousSettings, params, cfg, targetFps);

    using clock = std::chrono::steady_clock;
    auto lastConnected = clock::now();
    auto interval = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1.0, targetFps)));
    auto nextFrame = clock::now();
    printf("Paintify live: %s -> %s, %.1f fps, %s\n", opt.syphonIn.c_str(),
           opt.syphonOut.c_str(), targetFps, dev.name.UTF8String);
    fflush(stdout);

    for (;;) @autoreleasepool {
        // Syphon's server directory hears about servers through distributed
        // notifications, which are only delivered while a run loop turns --
        // this is the loop's sleep as well.
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
        if (!parentAlive(opt.liveParentPid)) break;
        if (!opt.liveStopFile.empty() && std::filesystem::exists(opt.liveStopFile)) break;

        const auto now = clock::now();
        if (now < nextFrame) continue;
        if (reloadSettings(opt.liveSettingsFile, previousSettings, params, cfg, targetFps)) {
            interval = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / std::max(1.0, targetFps)));
            havePainting = false;
            pipe.resetTemporal();
        }
        nextFrame += interval;
        if (nextFrame < now) nextFrame = now;

        if (!client || !client.isValid) {
            client = nil;
            NSArray* found = [[SyphonServerDirectory sharedDirectory] serversMatchingName:inputName appName:nil];
            if (found.count == 0) {
                // The TD component may have been removed. Do not leave a
                // renderer running after its sender vanishes.
                if (everConnected && now - lastConnected > std::chrono::seconds(5)) break;
                if (!announced) {
                    printf("Waiting for Syphon server '%s'...\n", opt.syphonIn.c_str());
                    fflush(stdout);
                    announced = true;
                }
                continue;
            }
            client = [[SyphonMetalClient alloc] initWithServerDescription:found.firstObject
                                                                   device:dev options:nil
                                                          newFrameHandler:nil];
            if (!client.isValid) { client = nil; continue; }
            announced = false;
            everConnected = true;
            havePainting = false;
            pipe.resetTemporal();
            printf("Paintify connected to '%s'\n", opt.syphonIn.c_str());
            fflush(stdout);
        }
        lastConnected = now;
        // TouchDesigner publishes a still input once and never again, and a
        // Syphon client only sees frames published after it attached. So the
        // first frame after connecting is painted whether or not Syphon calls
        // it new, and the last painting is republished on every tick.
        id<MTLTexture> painted = nil;
        const bool fresh = !havePainting || client.hasNewFrame;
        id<MTLTexture> frame = fresh ? [client newFrameImage] : nil;
        if (frame) {
            if (int(frame.width) != width || int(frame.height) != height) {
                width = int(frame.width);
                height = int(frame.height);
                havePainting = false;
                pipe.resetTemporal();
            }
            if (!pipe.setSourceTexture(frame, opt.flipIn)) continue;
            pipe.render(params, cfg, havePainting && params.frameDiffThreshold > 0.f);
            havePainting = true;
            params.frame += 1.f;
            painted = pipe.outputTexture(opt.flipOut);
        } else if (havePainting) {
            painted = pipe.lastOutputTexture();
        }
        if (!painted) continue;
        id<MTLCommandBuffer> cb = [pipe.queue() commandBuffer];
        [server publishFrameTexture:painted onCommandBuffer:cb
                        imageRegion:NSMakeRect(0, 0, width, height) flipped:NO];
        [cb commit];
        if (!frame) continue;

        if (++paintedFrames % 12 == 0) {
            printf("Paintify live: %llu frames, %dx%d, GPU %.1f ms, %u strokes\n",
                   paintedFrames, width, height, pipe.msTotal(), pipe.lastDrawnCount());
            fflush(stdout);
        }
    }

    [client stop];
    [server stop];
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const Options opt = parseArgs(argc, argv);
        signal(SIGPIPE, SIG_IGN);

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }

        MetalPipeline pipe;
        std::string err;
        if (!pipe.init(dev, shaderDir(), &err)) {
            fprintf(stderr, "Metal pipeline init failed:\n%s\n", err.c_str());
            return 1;
        }

        TuningParams params;
        RenderConfig cfg;
        resolveLook(opt, params, cfg);

        if (opt.live) {
            const int rc = runLive(pipe, params, cfg, opt);
            pipe.shutdown();
            return rc;
        }

        if (opt.in.empty()) { usage(); return 1; }
        // --in takes a comma-separated list, cycled over --repeat frames, so a
        // short sequence can exercise the temporal and optical-flow paths.
        std::vector<std::string> inputs;
        {
            std::stringstream ss(opt.in);
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) inputs.push_back(item);
        }
        int w = 0, h = 0;
        for (int i = 0; i < opt.repeat; ++i) {
            const std::string& path = inputs[size_t(i) % inputs.size()];
            int n = 0;
            unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
            if (!px) { fprintf(stderr, "could not read %s\n", path.c_str()); return 1; }
            pipe.setSource(px, w, h);
            stbi_image_free(px);
            pipe.render(params, cfg, i > 0 && params.frameDiffThreshold > 0.f);
            params.frame += 1.f;
            printf("frame %d: %dx%d  GPU %.1f ms  seeds %u  drawn %u  pool %u\n", i, w, h,
                   pipe.msTotal(), pipe.lastStrokeCount(), pipe.lastDrawnCount(),
                   pipe.lastPoolCount());
        }
        const std::vector<unsigned char> out = pipe.readCanvas();
        if (!stbi_write_png(opt.out.c_str(), w, h, 4, out.data(), w * 4)) {
            fprintf(stderr, "could not write %s\n", opt.out.c_str());
            return 1;
        }
        printf("wrote %s\n", opt.out.c_str());
        pipe.shutdown();
    }
    return 0;
}
