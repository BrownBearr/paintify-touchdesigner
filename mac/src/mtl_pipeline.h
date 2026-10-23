#pragma once
// Metal port of src/pipeline.h for macOS.
//
// The stages, their order and every shader are the Windows renderer's: the
// GLSL in shaders/ is translated to MSL by mac/tools/glsl_to_msl.py, with its
// bindings shifted so that one GL binding maps to exactly one Metal slot:
//
//   GL                          Metal
//   sampler unit n          ->  texture(n) + sampler(n)
//   image binding n         ->  texture(8 + n)
//   SSBO binding n          ->  buffer(1 + n)
//   Params UBO              ->  buffer(0)
//   loose `uniform` values  ->  buffer(12), the "Locals" struct
//   .length() of an SSBO    ->  buffer(25), sizes indexed by buffer slot
//
// That mapping is what lets this file read like pipeline.cpp: the host keeps
// GL-style binding state and applies it at each dispatch.
#import <Metal/Metal.h>

#include "params.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mtl {

// A texture plus the sampler GL would have attached to it.
struct Tex {
    id<MTLTexture> t = nil;
    id<MTLSamplerState> s = nil;
    explicit operator bool() const { return t != nil; }
};

// One compute stage. `locals` mirrors the GL program's loose uniforms, which
// persist between dispatches exactly as GL program uniforms do.
struct Kernel {
    id<MTLComputePipelineState> pso = nil;
    MTLSize group = MTLSizeMake(1, 1, 1);
    struct Field { uint32_t offset; uint32_t size; };
    std::map<std::string, Field> fields;
    std::vector<uint8_t> locals;

    void set(const char* name, const void* v, size_t n);
    void seti(const char* name, int v) { set(name, &v, 4); }
    void setu(const char* name, uint32_t v) { set(name, &v, 4); }
    void set2i(const char* name, int x, int y) { int v[2] = {x, y}; set(name, v, 8); }
    void set3f(const char* name, float x, float y, float z) { float v[3] = {x, y, z}; set(name, v, 12); }
};

} // namespace mtl

class MetalPipeline {
public:
    bool init(id<MTLDevice> device, const std::string& shaderDir, std::string* err);
    void shutdown();

    id<MTLDevice> device() const { return m_dev; }
    id<MTLCommandQueue> queue() const { return m_queue; }

    // Uploads an RGBA8 image and (re)sizes every render target to match.
    bool setSource(const unsigned char* rgba, int w, int h);
    // GPU-only input path for live Syphon frames. `flipY` turns a bottom-up
    // texture into the pipeline's top-row-first convention.
    bool setSourceTexture(id<MTLTexture> texture, bool flipY);

    void render(const TuningParams& base, const RenderConfig& cfg, bool temporal = false);
    void resetTemporal() { m_havePrev = false; }

    // RGBA8, top-row-first, like Pipeline::readCanvas.
    std::vector<unsigned char> readCanvas();
    // Canvas converted to BGRA8 for publication; valid until the next render.
    id<MTLTexture> outputTexture(bool flipY);
    // What outputTexture last produced, without converting again.
    id<MTLTexture> lastOutputTexture() const { return m_outTex.t; }

    int width() const { return m_w; }
    int height() const { return m_h; }

    double msTotal() const { return m_msTotal; }
    uint32_t lastStrokeCount() const { return m_lastStrokes; }
    uint32_t lastDrawnCount() const { return m_lastDrawn; }
    uint32_t lastPoolCount() const { return m_poolCount; }

private:
    // --- GL-style state, applied at each dispatch ---------------------------
    void bindTex(int unit, const mtl::Tex& t) { m_unit[unit] = t; }
    void bindImage(int binding, const mtl::Tex& t) { m_image[binding] = t.t; }
    void bindBuf(int binding, id<MTLBuffer> b) { m_ssbo[binding] = b; }
    void dispatch(mtl::Kernel& k, uint32_t gx, uint32_t gy = 1);

    // --- command buffer / encoders ------------------------------------------
    id<MTLComputeCommandEncoder> computeEncoder();
    id<MTLBlitCommandEncoder> blitEncoder();
    void endEncoder();
    // Commits the frame's work so far. `wait` is the equivalent of the GL
    // readbacks that stall: everything before it is finished on return.
    void flush(bool wait);

    void copyTex(const mtl::Tex& src, const mtl::Tex& dst);
    void clearBuf(id<MTLBuffer> b, size_t bytes);
    void fillTex(const mtl::Tex& t, float r, float g = 0.f, float b = 0.f, float a = 0.f);
    void genMips(const mtl::Tex& t);
    void convert(id<MTLTexture> src, id<MTLTexture> dst, bool flipY);

    mtl::Tex makeTex(int w, int h, MTLPixelFormat f, bool mips = false);
    id<MTLBuffer> makeBuf(size_t bytes);

    bool buildKernel(mtl::Kernel& k, const std::string& file, MTLSize group, std::string* err);
    id<MTLLibrary> loadLibrary(const std::string& file, std::string* err);

    // --- ported from Pipeline -------------------------------------------------
    void resize(int w, int h);
    void uploadParams(const TuningParams& p) { m_p = p; }
    void ensureCellCapacity(uint32_t cells);
    void buildBrushTiles(const std::vector<float>& radii, float bristleDensity);
    void readStats();
    void gaussian(const mtl::Tex& src, const mtl::Tex& dst);
    void buildReference(TuningParams& p, float radius);
    void buildGradients(const TuningParams& p);
    void applyEtf(const TuningParams& p, const RenderConfig& cfg);
    void computeFlow(const TuningParams& p, const RenderConfig& cfg);
    void layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                       const std::vector<float>& radii, bool temporal,
                       bool allowCache = false);
    void drawStrokes(id<MTLBuffer> indirect);
    void repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                         const std::vector<float>& radii);
    void relax(TuningParams& p, const RenderConfig& cfg, const std::vector<float>& radii);

    id<MTLDevice> m_dev = nil;
    id<MTLCommandQueue> m_queue = nil;
    id<MTLCommandBuffer> m_cb = nil;
    id<MTLComputeCommandEncoder> m_ce = nil;
    id<MTLBlitCommandEncoder> m_be = nil;
    std::string m_shaderDir;
    double m_gpuMs = 0.0;

    mtl::Tex m_unit[8];
    id<MTLTexture> m_image[4] = {nil, nil, nil, nil};
    id<MTLBuffer> m_ssbo[12] = {nil};
    TuningParams m_p{};

    mtl::Kernel m_blur, m_features, m_error, m_seeds, m_trace, m_impasto,
                m_canvasProg, m_pool, m_relax, m_etf, m_flow;
    mtl::Kernel m_fill, m_convert;
    mtl::Tex m_dummyTex;
    id<MTLBuffer> m_dummyBuf = nil;
    id<MTLRenderPipelineState> m_stroke = nil;

    id<MTLSamplerState> m_linear = nil, m_linearMip = nil, m_brushSampler = nil;

    int m_w = 0, m_h = 0;
    mtl::Tex m_srcTex, m_refTex, m_tmpTex, m_tensorTex, m_gradTex, m_errTex, m_diffTex;
    mtl::Tex m_etfTex;
    mtl::Tex m_lumaPrevTex, m_lumaCurTex, m_flowTex, m_flowTmpTex;
    int m_flowW = 0, m_flowH = 0;
    bool m_flowValid = false;
    mtl::Tex m_canvasTex, m_heightTex;
    mtl::Tex m_prevSrcTex, m_prevCanvasTex, m_underTex;
    mtl::Tex m_outTex, m_readTex;
    bool m_underValid = false;
    mtl::Tex m_brushTex;

    id<MTLBuffer> m_cellBuf = nil, m_seedBuf = nil, m_vertexBuf = nil;
    id<MTLBuffer> m_headerBuf = nil, m_counterBuf = nil, m_indirectBuf = nil;
    id<MTLBuffer> m_poolVertexBuf = nil, m_poolHeaderBuf = nil, m_poolCounterBuf = nil;
    id<MTLBuffer> m_energyBuf = nil, m_etfMaxBuf = nil, m_statsBuf = nil;
    uint32_t m_cellCapacity = 0;

    std::vector<int> m_brushRows;
    std::vector<float> m_brushRadii;
    float m_brushDensity = -1.f;

    bool m_havePrev = false;
    size_t m_layerCount = 0;
    uint32_t m_poolCount = 0;
    double m_msTotal = 0.0;
    uint32_t m_lastStrokes = 0, m_lastDrawn = 0, m_lastPoints = 0;
};
