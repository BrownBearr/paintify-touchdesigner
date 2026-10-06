#pragma once
// Live bridge for macOS: Syphon in -> Metal painter -> Syphon out.
// The counterpart of src/live_spout.cpp. Objective-C++ only.

#import <Metal/Metal.h>

#include "metal_pipeline.h"
#include "params.h"

#include <cstdint>
#include <functional>
#include <string>

struct LiveSyphonConfig {
    std::string inputName = "Paintify Input";
    std::string outputName = "Paintify Output";
    std::string stopFile;       // optional per-process shutdown signal
    uint32_t parentPid = 0;     // exit if the owning TouchDesigner process exits
    double fps = 12.0;
    // Syphon frames are bottom-row-first; Metal textures are top-row-first.
    bool flipIn = true;         // flip received frames on ingest
    bool flipOut = true;        // publish with Syphon's `flipped:` set
    std::function<bool(TuningParams&, RenderConfig&, double&)> reloadSettings;
};

int runLiveSyphon(id<MTLDevice> device, MetalPipeline& pipe, TuningParams params,
                  const RenderConfig& render, const LiveSyphonConfig& live);
