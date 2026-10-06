#pragma once
#include "params.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

// Command-line options and look presets, shared by the OpenGL renderer
// (main.cpp) and the Metal renderer (mac/main_mac.mm).

struct Preset {
    const char* name;
    const char* radii;
    float maxLen, minLen, curvature, threshold, gridFactor, opacity;
    float hue, sat, val;
    float brushTexture;
    Underpaint underpaint;
};

extern const Preset kPresets[];
extern const int kPresetCount;

struct Options {
    bool headless = false;
    bool liveSpout = false;       // live bridge: Spout on Windows, Syphon on macOS
    std::string spoutIn = "Paintify Input";
    std::string spoutOut = "Paintify Output";
    std::string liveStopFile;
    std::string liveSettingsFile;
    uint32_t liveParentPid = 0;
    double targetFps = 12.0;
    std::string in;
    std::string out = "out.png";
    int dump = 0;
    bool debugCells = false;
    int passes = 0;
    std::string framesDir;
    std::string batchDir;
    std::string videoIn;
    std::string outDir = "frames_out";
    double fps = 0.0;          // 0 = take it from the input
    int crf = 18;
    std::string vcodec = "libx264";
    std::string vpreset = "medium";
    bool noAudio = false;
    std::string batchSuffix = "_painted";
    std::string batchFormat = "png";
    std::string preset;
    std::string paramsFile;
    std::string saveParamsFile;
    std::string radii;
    bool haveUnderpaint = false;
    Underpaint underpaint = Underpaint::Blur;
    // Overrides; NaN means "leave the preset's value alone".
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
    bool relaxLog = false;
    bool etfLog = false;
    bool flowLog = false;
    bool jitterPerFrame = false;
    // macOS only: Syphon frames are bottom-row-first, Metal textures top-row-first.
    bool flipIn = true, flipOut = true;
    std::string metallib;         // macOS only: path to paintify.metallib
};

void applyPreset(const Preset& pr, TuningParams& p, RenderConfig& cfg, std::string* radiiText);
void usage();
Options parseArgs(int argc, char** argv);
void applyOverrides(const Options& o, TuningParams& p, RenderConfig& cfg,
                    std::string* radiiText);
void resolveLook(const Options& opt, TuningParams& params, RenderConfig& cfg,
                 std::string* radiiText, int* presetIndex);

// Live mode: re-reads the settings file the TouchDesigner component rewrites
// (one command-line argument per line) and, when it changed, resolves it into
// `params` / `cfg` / `fps`. Returns true when something was applied.
std::function<bool(TuningParams&, RenderConfig&, double&)>
makeLiveReloader(const std::string& path);
