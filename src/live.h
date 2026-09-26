#pragma once

#include "params.h"
#include "pipeline.h"

#include <string>
#include <cstdint>

// The TouchDesigner live bridge: receive frames from a TouchDesigner sender,
// paint them, and send the painting back. Spout on Windows (live_spout.cpp),
// Syphon on macOS (live_syphon.mm) -- TouchDesigner's Syphon Spout In/Out
// TOPs speak whichever one the platform has, so the component is the same.
struct LiveConfig {
    std::string inputName = "Paintify Input";
    std::string outputName = "Paintify Output";
    std::string stopFile; // optional per-process shutdown signal
    uint32_t parentPid = 0; // exit if the owning TouchDesigner process exits
    double fps = 12.0;
};

// Runs until stopped: the stop file appears, the parent process exits, or the
// sender has been gone for a while after having connected.
int runLive(Pipeline& pipe, TuningParams params, const RenderConfig& render,
            const LiveConfig& live);
