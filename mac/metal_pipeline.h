#pragma once
// Metal port of src/pipeline.cpp. Same stages, same order, same shaders (the
// GLSL in shaders/ is translated to MSL at build time by tools/glsl2msl.py).
//
// Not ported: the interactive overlay's telemetry and the diagnostic readbacks
// (energy, flow coherence, stroke dumps). They exist to verify the algorithms
// on the OpenGL build and cost a stall each.
//
// Objective-C++ only.

#import <Metal/Metal.h>

#include "params.h"

#include <string>
#include <vector>

class MetalPipeline {
public:
    bool init(id<MTLDevice> device, const std::string& metallibPath, std::string* err);
    void shutdown();

    // Uploads an RGBA8 image (top row first) and (re)sizes every render target.
    bool setSource(const unsigned char* rgba, int w, int h);

    // GPU-only live input. `tex` is any 8-bit RGBA/BGRA texture; flipY turns a
    // Syphon (bottom-row-first) frame into the pipeline's top-row-first order.
    bool setSourceTexture(id<MTLTexture> tex, int w, int h, bool flipY);

    // Encodes a whole frame onto the open command buffer. Call commit() to run it.
    void render(const TuningParams& base, const RenderConfig& cfg, bool temporal = false);

    void resetTemporal() { m_havePrev = false; }

    // Converts the canvas to BGRA8 on the open command buffer and returns it.
    // The texture is owned by the pipeline and rewritten by the next frame.
    id<MTLTexture> encodeOutput();

    // The command buffer frame work is being encoded on (created on demand).
    // Syphon needs it to append its publish blit.
    id<MTLCommandBuffer> commandBuffer();

    // Ends encoding and submits. With wait, blocks until the GPU is done.
    void commit(bool wait);

    // Canvas as RGBA8, top row first. Submits and waits.
    std::vector<unsigned char> readCanvas();

    int width() const { return m_w; }
    int height() const { return m_h; }
    double lastGpuMs() const { return m_gpuMs; }

private:
    struct ArgInfo { MTLArgumentType type; int index; };
    struct Kernel {
        id<MTLComputePipelineState> pso = nil;
        std::vector<ArgInfo> args;
        MTLSize threads = {1, 1, 1};
    };
    struct Bind;

    bool makeKernel(Kernel& k, const char* name, int tx, int ty, std::string* err);
    void resize(int w, int h);
    void buildBrushTiles(const std::vector<float>& radii, float bristleDensity);
    void ensureCellCapacity(uint32_t cells);

    // --- encoding ---
    id<MTLComputeCommandEncoder> computeEncoder();
    id<MTLBlitCommandEncoder> blitEncoder();
    void endEncoders();
    void dispatch(const Kernel& k, uint32_t gx, uint32_t gy, const Bind& b,
                  const void* loose = nullptr, size_t looseSize = 0);
    void copyTex(id<MTLTexture> src, id<MTLTexture> dst);
    void clearTex(id<MTLTexture> t);
    void clearU32(id<MTLBuffer> buf, uint32_t count);
    void generateMips(id<MTLTexture> t);
    void drawStrokes(id<MTLBuffer> vertexBuf, id<MTLBuffer> headerBuf);
    // Submits the open frame, waits, and copies `bytes` of `src` back.
    void readBuffer(id<MTLBuffer> src, size_t bytes, void* dst);

    // --- stages ---
    void uploadParams(const TuningParams& p) { m_params = p; }
    void gaussian(id<MTLTexture> src, id<MTLTexture> dst);
    void buildReference(TuningParams& p, float radius);
    void buildGradients(const TuningParams& p);
    void applyEtf(const TuningParams& p, const RenderConfig& cfg);
    void computeFlow(const TuningParams& p, const RenderConfig& cfg);
    void layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                       const std::vector<float>& radii, bool temporal,
                       bool allowCache = false);
    void repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                         const std::vector<float>& radii);
    void relax(TuningParams& p, const RenderConfig& cfg,
               const std::vector<float>& radii);

    id<MTLDevice> m_dev = nil;
    id<MTLCommandQueue> m_queue = nil;
    id<MTLLibrary> m_lib = nil;
    id<MTLCommandBuffer> m_cb = nil;
    id<MTLComputeCommandEncoder> m_ce = nil;
    id<MTLBlitCommandEncoder> m_be = nil;
    id<MTLRenderCommandEncoder> m_re = nil;

    Kernel m_blur, m_features, m_error, m_seeds, m_trace, m_impasto, m_canvasProg,
           m_pool, m_relax, m_etf, m_flow;
    Kernel m_clearU32, m_clearTex, m_copyTex, m_ingest, m_egress;
    id<MTLRenderPipelineState> m_stroke = nil;
    id<MTLSamplerState> m_sampLinear = nil, m_sampRepeat = nil;
    id<MTLTexture> m_dummy = nil;
    id<MTLBuffer> m_dummyBuf = nil;

    TuningParams m_params;
    int m_w = 0, m_h = 0;

    id<MTLTexture> m_srcTex = nil, m_refTex = nil, m_tmpTex = nil, m_tensorTex = nil;
    id<MTLTexture> m_gradTex = nil, m_errTex = nil, m_diffTex = nil, m_etfTex = nil;
    id<MTLTexture> m_lumaPrevTex = nil, m_lumaCurTex = nil;
    id<MTLTexture> m_flowTex = nil, m_flowTmpTex = nil;
    int m_flowW = 0, m_flowH = 0;
    bool m_flowValid = false;
    id<MTLTexture> m_canvasTex = nil, m_heightTex = nil;
    id<MTLTexture> m_prevSrcTex = nil, m_prevCanvasTex = nil, m_underTex = nil;
    bool m_underValid = false;
    id<MTLTexture> m_brushTex = nil;
    id<MTLTexture> m_outTex = nil;

    id<MTLBuffer> m_cellBuf = nil, m_seedBuf = nil, m_vertexBuf = nil;
    id<MTLBuffer> m_headerBuf = nil, m_counterBuf = nil, m_indirectBuf = nil;
    id<MTLBuffer> m_poolVertexBuf = nil, m_poolHeaderBuf = nil, m_poolCounterBuf = nil;
    id<MTLBuffer> m_energyBuf = nil, m_etfMaxBuf = nil;
    uint32_t m_cellCapacity = 0;

    std::vector<int> m_brushRows;
    std::vector<float> m_brushRadii;
    float m_brushDensity = -1.f;

    bool m_havePrev = false;
    uint32_t m_poolCount = 0;
    double m_gpuMs = 0.0;
};
