#include "options.h"
#include "paramfile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using paramfile::parseRadii;
using paramfile::radiiToString;

// ── Presets ────────────────────────────────────────────────────────────────
// The Hertzmann presets from PainterlyImageCreatorWeb's main.js PRESETS, value
// for value, so a look chosen in the web UI can be reproduced here by name.
// Fields the web preset leaves unset fall back to its PRESET_DEFAULTS, which is
// why several have brushTexture 0. `detail` is the one entry not from the web.


const Preset kPresets[] = {
    // name           radii       maxL minL curv  thr   grid   op     hue    sat    val    tex    underpaint
    {"impressionist", "8,4,2",    16,  4,   1.0f, 50.f, 1.0f,  0.90f, 0.05f, 0.10f, 0.10f, 0.45f, Underpaint::Blur},
    {"expressionist", "12,6,3",   28,  8,   1.0f, 40.f, 0.9f,  0.95f, 0.15f, 0.20f, 0.15f, 0.60f, Underpaint::Blur},
    // 'average' matters here: with a blur underpaint the radius-4 layer paints
    // the colour that is already on the canvas and the dabs never show.
    {"pointillist",   "4,2",       3,  1,   0.0f, 30.f, 0.75f, 0.85f, 0.10f, 0.15f, 0.10f, 0.00f, Underpaint::Average},
    {"wash",          "20,10",    32, 10,   0.7f, 80.f, 1.5f,  0.40f, 0.20f, 0.30f, 0.20f, 0.00f, Underpaint::Blur},
    // gpu-sbr's own: a tight threshold and a fine finest layer, for detail.
    {"detail",        "6,3,1.5",  10,  4,   1.0f, 20.f, 1.0f,  1.00f, 0.00f, 0.00f, 0.00f, 0.50f, Underpaint::Blur},
};
const int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// parseRadii / radiiToString live in paramfile.cpp: the GUI text box and a
// saved file have to agree on what "8, 4, 2" means, so there is one copy.
void applyPreset(const Preset& pr, TuningParams& p, RenderConfig& cfg, std::string* radiiText) {
    cfg.radii = parseRadii(pr.radii);
    cfg.underpaint = pr.underpaint;
    if (radiiText) *radiiText = radiiToString(cfg.radii);
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



void usage() {
    printf(
        "gpu-sbr -- GPU port of PainterlyImageCreatorWeb's Hertzmann renderer\n\n"
        "  --in <path>            source image (omitted: synthetic subject)\n"
        "  --out <path>            output PNG for --headless and the S key\n"
        "  --headless              render and exit, no window\n"
        "  --live-spout            paint live Spout (Windows) or Syphon (macOS) input and\n"
        "                          publish the result the same way (alias: --live-syphon)\n"
        "  --spout-in <name>       live input sender name (Paintify Input)\n"
        "  --spout-out <name>      live output sender name (Paintify Output)\n"
        "  --target-fps <n>        live painted frames per second (12)\n"
        "  --flip-in <0|1>         macOS: flip the Syphon input vertically (1)\n"
        "  --flip-out <0|1>        macOS: publish the output flipped (1)\n"
        "  --metallib <path>       macOS: shader library (default: next to the exe)\n"
        "  --live-stop-file <path> exit live mode when this file appears\n"
        "  --live-settings-file <path> reload live controls from this file\n"
        "  --live-parent-pid <n>  exit if the owning process closes\n"
        "  --preset <name>         impressionist | expressionist | pointillist |\n"
        "                          wash   (the web's, value for value)\n"
        "                          detail (this project's own)\n"
        "  --save-params <file>    write the fully resolved parameters and carry\n"
        "                          on, so a preset plus a few flags becomes an\n"
        "                          editable file to tweak by hand\n"
        "  --params <file>         load a parameter file saved from the GUI.\n"
        "                          Applied after --preset, before the individual\n"
        "                          overrides below, so a flag still wins.\n"
        "  --radii \"8,4,2\"         brush radii, coarse to fine (max 8)\n"
        "  --threshold <f>         Lab error a cell must exceed to earn a stroke\n"
        "  --curvature <0..1>      Hertzmann's fc filter\n"
        "  --opacity <0..1>        per-stroke alpha\n"
        "  --grid-factor <f>       seed grid pitch, in radii\n"
        "  --max-len / --min-len   stroke length bounds, in steps\n"
        "  --tensor-sigma <f>      > 0 swaps in the structure-tensor flow field\n"
        "  --etf <n>               Edge Tangent Flow iterations over the flow\n"
        "                          field (Kang 2007). Edge-aware smoothing: weak\n"
        "                          pixels defer to strong neighbours and nothing\n"
        "                          is smoothed across a perpendicular flow, which\n"
        "                          a Gaussian cannot express. 0 (default) leaves\n"
        "                          the field exactly as the web version has it;\n"
        "                          2-3 converges.\n"
        "  --etf-radius <f>        ETF neighbourhood radius in px (default 5)\n"
        "  --brush-texture <0..1>  bristle texture strength\n"
        "  --bristle-density <f>   bristles across the stroke (default 10)\n"
        "  --texture-taper <0..1>  how much the stroke tips narrow\n"
        "  --impasto <f>           paint height accumulated per stroke\n"
        "  --impasto-light <f>     relief lighting strength\n"
        "  --light-angle <deg>     relief light direction\n"
        "  --dry-brush <0..1>      opacity falloff along the stroke\n"
        "  --size/--angle/--opacity-jitter <f>   per-stroke variation\n"
        "  --underpaint <mode>     blur | none | average\n"
        "\n Video and batch\n"
        "  --video <in> --out <out.mp4>    paint a video; frames stream through\n"
        "                          ffmpeg as raw RGBA, nothing hits the disk.\n"
        "                          Audio is copied from the input when present.\n"
        "  --batch <dir> --outdir <dir>    paint every image in a directory\n"
        "  --frames <dir> --outdir <dir>   image sequence, temporal coherence on\n"
        "  --fps <f>               output frame rate (default: the input's)\n"
        "  --crf <n>               x264 quality, lower is better (default 18)\n"
        "  --vcodec / --vpreset    encoder and speed preset (libx264, medium)\n"
        "  --no-audio              drop the input's audio track\n"
        "  --suffix <s>            batch output suffix (default _painted)\n"
        "  --format <ext>          batch output format: png | jpg | bmp | tga\n"
        "  --temporal-diff <f>     per-cell source change needed to repaint\n"
        "                          (0..255). 0 paints every frame from scratch,\n"
        "                          which makes strokes shimmer; 12 is a good\n"
        "                          start for video.\n"
        "  --flow <n>              pyramidal Lucas-Kanade levels for temporal\n"
        "                          advection. Carries the painting along the\n"
        "                          motion instead of leaving it pinned to the\n"
        "                          pixel grid, which is what --temporal-diff\n"
        "                          alone cannot fix on a moving camera. 0 (the\n"
        "                          default) is the web version's behaviour; 4\n"
        "                          tracks roughly 32 px/frame. Needs\n"
        "                          --temporal-diff > 0 to do anything.\n"
        "  --flow-iters <n>        LK refinements per level (default 3)\n"
        "  --passes <n>            painting passes per layer (default 8). The web\n"
        "                          version paints one stroke at a time and each\n"
        "                          stroke sees the last one's paint, which is what\n"
        "                          sets stroke length; more passes approximate it\n"
        "                          more closely. 8 lands within 1%%.\n"
        "  --debug-cells           print each layer's error distribution\n"
        "  --dump <n>              print the first n traced polylines\n"
        "\n"
        " Relaxation (Hertzmann 2001, \"Paint By Relaxation\"). After the greedy\n"
        " pass places the strokes, iteratively improve them against\n"
        "   E = sum|Lab(canvas)-Lab(source)| + area-weight * sum(stroke area)\n"
        "  --relax <n>             iterations; 0 (default) = plain Hertzmann 1998\n"
        "  --relax-area <f>        w_area: higher = fewer, larger, bolder strokes\n"
        "  --relax-move <f>        perturbation amplitude, in stroke radii\n"
        "  --relax-candidates <n>  trial moves per stroke per iteration\n"
        "  --relax-remove <f>      > 0 deletes strokes that do not pay for their\n"
        "                          area; 1.0 is break-even\n"
        "  --relax-subpasses <n>   hash-scattered subsets per iteration\n"
        "  --relax-log             print the global energy after each iteration\n");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--headless")        o.headless = true;
        else if (a == "--live-spout" || a == "--live-syphon") o.liveSpout = true;
        else if (a == "--flip-in")         o.flipIn = atoi(next(i)) != 0;
        else if (a == "--flip-out")        o.flipOut = atoi(next(i)) != 0;
        else if (a == "--metallib")        o.metallib = next(i);
        else if (a == "--spout-in")        o.spoutIn = next(i);
        else if (a == "--spout-out")       o.spoutOut = next(i);
        else if (a == "--target-fps")      o.targetFps = atof(next(i));
        else if (a == "--live-stop-file")   o.liveStopFile = next(i);
        else if (a == "--live-settings-file") o.liveSettingsFile = next(i);
        else if (a == "--live-parent-pid")   o.liveParentPid = uint32_t(std::strtoul(next(i), nullptr, 10));
        else if (a == "--in")              o.in = next(i);
        else if (a == "--out")             o.out = next(i);
        else if (a == "--dump")            o.dump = atoi(next(i));
        else if (a == "--debug-cells")     o.debugCells = true;
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
        else if (a == "--relax-log")       o.relaxLog = true;
        else if (a == "--etf-log")         o.etfLog = true;
        else if (a == "--flow-log")        o.flowLog = true;
        else if (a == "--jitter-per-frame") o.jitterPerFrame = true;
        else if (a == "--frames")        { o.framesDir = next(i); o.headless = true; }
        else if (a == "--video")         { o.videoIn = next(i); o.headless = true; }
        else if (a == "--batch")         { o.batchDir = next(i); o.headless = true; }
        else if (a == "--fps")             o.fps = atof(next(i));
        else if (a == "--crf")             o.crf = atoi(next(i));
        else if (a == "--vcodec")          o.vcodec = next(i);
        else if (a == "--vpreset")         o.vpreset = next(i);
        else if (a == "--no-audio")        o.noAudio = true;
        else if (a == "--suffix")          o.batchSuffix = next(i);
        else if (a == "--format")          o.batchFormat = next(i);
        else if (a == "--outdir")          o.outDir = next(i);
        else if (a == "--preset")          o.preset = next(i);
        else if (a == "--params")          o.paramsFile = next(i);
        else if (a == "--save-params")     o.saveParamsFile = next(i);
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

void applyOverrides(const Options& o, TuningParams& p, RenderConfig& cfg,
                    std::string* radiiText) {
    auto set = [](float& dst, float v) { if (!std::isnan(v)) dst = v; };
    if (!o.radii.empty()) {
        cfg.radii = parseRadii(o.radii);
        if (radiiText) *radiiText = radiiToString(cfg.radii);
    }
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

void resolveLook(const Options& opt, TuningParams& params, RenderConfig& cfg,
                 std::string* radiiText, int* presetIndex) {
    int selected = 0;
    applyPreset(kPresets[0], params, cfg, radiiText);
    if (!opt.preset.empty()) {
        bool found = false;
        for (int i = 0; i < kPresetCount; ++i) {
            if (opt.preset == kPresets[i].name) {
                selected = i;
                applyPreset(kPresets[i], params, cfg, radiiText);
                found = true;
                break;
            }
        }
        if (!found) fprintf(stderr, "unknown preset '%s'; using impressionist\n",
                            opt.preset.c_str());
    }
    // A saved file sits between a preset and explicit command-line overrides.
    if (!opt.paramsFile.empty()) {
        std::string perr;
        if (!paramfile::load(opt.paramsFile, &params, &cfg, &perr))
            fprintf(stderr, "%s\n", perr.c_str());
        else {
            if (!perr.empty()) fprintf(stderr, "%s: %s\n", opt.paramsFile.c_str(),
                                       perr.c_str());
            if (radiiText) *radiiText = radiiToString(cfg.radii);
        }
    }
    applyOverrides(opt, params, cfg, radiiText);
    if (presetIndex) *presetIndex = selected;
}

std::function<bool(TuningParams&, RenderConfig&, double&)>
makeLiveReloader(const std::string& path) {
    return [path, previous = std::string{}]
           (TuningParams& current, RenderConfig& config, double& fps) mutable -> bool {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        const std::string contents((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
        if (contents.empty() || contents == previous) return false;
        std::vector<std::string> arguments{"gpu-sbr"};
        std::istringstream lines(contents);
        std::string value;
        while (std::getline(lines, value)) {
            if (!value.empty() && value.back() == '\r') value.pop_back();
            if (!value.empty()) arguments.push_back(value);
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size());
        for (std::string& argument : arguments)
            argv.push_back(argument.data());
        const Options updated = parseArgs(int(argv.size()), argv.data());
        TuningParams fresh;
        RenderConfig freshConfig;
        resolveLook(updated, fresh, freshConfig, nullptr, nullptr);
        fresh.frame = current.frame;
        current = fresh;
        config = freshConfig;
        fps = updated.targetFps;
        previous = contents;
        std::printf("Paintify live settings updated\n");
        std::fflush(stdout);
        return true;
    };
}
