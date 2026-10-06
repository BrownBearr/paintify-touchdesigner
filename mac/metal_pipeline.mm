#include "metal_pipeline.h"
#include "brush_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>

// Resource numbering shared with tools/glsl2msl.py and shaders/util.metal.
namespace {

constexpr int IMAGE_BASE = 8;       // storage textures: [[texture(IMAGE_BASE + n)]]
constexpr int PARAMS_BUFFER = 11;   // the Params uniform block
constexpr int LOOSE_BUFFER = 12;    // per-dispatch constants

// SSBO binding points, mirrored in the shaders.
enum : int {
    BIND_CELLS = 0, BIND_SEEDS = 1, BIND_VERTS = 2, BIND_COUNTERS = 3,
    BIND_HEADERS = 4, BIND_INDIRECT = 5, BIND_POOL_VERTS = 6,
    BIND_POOL_HEADERS = 7, BIND_POOL_COUNTER = 8, BIND_ENERGY = 9,
    BIND_ETF_MAX = 10,
};

// Texture units, mirrored in the shaders.
enum : int {
    UNIT_A = 0, UNIT_B = 1, UNIT_C = 2, UNIT_D = 3, UNIT_BRUSH = 4,
    UNIT_E = 5, UNIT_F = 6,
};

constexpr uint32_t MAX_CHUNKS = 32u;

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Per-dispatch constants. Each mirrors the `Loose` block spirv-cross emits for
// the shader (see the structs in build-mac/mac/msl/<shader>_comp.metal) and is
// padded to 16 bytes so the bound range is never smaller than the MSL struct.
struct alignas(16) LooseBlur     { int32_t uPass; };
struct alignas(16) LooseFeatures { int32_t uPass; };
struct alignas(16) LooseEtf      { int32_t uPass; };
struct alignas(16) LooseSeeds    { int32_t uTemporal; };
struct alignas(16) LooseError    { int32_t uPass, uTemporal; uint32_t uCellCount; };
struct alignas(16) LoosePool     { int32_t uPass; uint32_t uPoolCapacity; };
struct alignas(16) LooseRelax    { uint32_t uPoolCapacity, uSubPass, uSubPassCount; };
struct alignas(16) LooseFlow     { int32_t uPass, uLevel; int32_t uFlowSize[2]; int32_t uWindow; };
// float3 uFill sits at offset 16 (std140 / MSL float3 alignment).
struct alignas(16) LooseCanvas   { int32_t uPass, uUseFlow, pad0, pad1; float uFill[3]; float pad2; };
static_assert(sizeof(LooseCanvas) == 32, "LooseCanvas must match the MSL layout");

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t e = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (((x >> 23) & 0xffu) == 0xffu) return uint16_t(sign | 0x7c00u | (m ? 0x200u : 0u));
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t hm = m >> shift;
        if ((m >> (shift - 1u)) & 1u) ++hm;
        return uint16_t(sign | hm);
    }
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;
    return uint16_t(h);
}

} // namespace

struct MetalPipeline::Bind {
    id<MTLTexture> tex[16] = {};
    id<MTLBuffer> buf[16] = {};
    Bind& t(int unit, id<MTLTexture> x) { tex[unit] = x; return *this; }
    Bind& img(int n, id<MTLTexture> x) { tex[IMAGE_BASE + n] = x; return *this; }
    Bind& b(int index, id<MTLBuffer> x) { buf[index] = x; return *this; }
};

// ── setup ───────────────────────────────────────────────────────────────────

// MTLArgument reflection is deprecated in favour of MTLBinding but still
// works everywhere this runs, and binding by reflected index is what keeps a
// shader/host mismatch from silently reading the wrong resource.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool MetalPipeline::makeKernel(Kernel& k, const char* name, int tx, int ty, std::string* err) {
    id<MTLFunction> fn = [m_lib newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (!fn) {
        if (err) *err = std::string("shader function not found in library: ") + name;
        return false;
    }
    NSError* e = nil;
    MTLComputePipelineReflection* refl = nil;
    k.pso = [m_dev newComputePipelineStateWithFunction:fn
                                               options:MTLPipelineOptionArgumentInfo
                                            reflection:&refl
                                                 error:&e];
    if (!k.pso) {
        if (err) *err = std::string(name) + ": " + (e ? [[e localizedDescription] UTF8String] : "pipeline creation failed");
        return false;
    }
    if (size_t(tx) * size_t(ty) > size_t([k.pso maxTotalThreadsPerThreadgroup])) {
        if (err) *err = std::string(name) + ": threadgroup too large for this GPU";
        return false;
    }
    if (!refl) {
        if (err) *err = std::string(name) + ": no argument reflection from the Metal driver";
        return false;
    }
    k.threads = MTLSizeMake(NSUInteger(tx), NSUInteger(ty), 1);
    k.args.clear();
    for (MTLArgument* a in refl.arguments)
        k.args.push_back({a.type, int(a.index)});
    return true;
}
#pragma clang diagnostic pop

bool MetalPipeline::init(id<MTLDevice> device, const std::string& metallibPath, std::string* err) {
    m_dev = device;
    m_queue = [m_dev newCommandQueue];
    NSError* e = nil;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:metallibPath.c_str()]];
    m_lib = [m_dev newLibraryWithURL:url error:&e];
    if (!m_lib) {
        if (err) *err = "cannot load " + metallibPath + ": " +
                        (e ? [[e localizedDescription] UTF8String] : "unknown error");
        return false;
    }

    if (!makeKernel(m_blur,       "blur_comp",     16, 16, err)) return false;
    if (!makeKernel(m_features,   "features_comp", 16, 16, err)) return false;
    if (!makeKernel(m_error,      "error_comp",    16, 16, err)) return false;
    if (!makeKernel(m_seeds,      "seeds_comp",     8,  8, err)) return false;
    if (!makeKernel(m_trace,      "trace_comp",    64,  1, err)) return false;
    if (!makeKernel(m_impasto,    "impasto_comp",  16, 16, err)) return false;
    if (!makeKernel(m_canvasProg, "canvas_comp",   16, 16, err)) return false;
    if (!makeKernel(m_pool,       "pool_comp",     64,  1, err)) return false;
    if (!makeKernel(m_relax,      "relax_comp",    64,  1, err)) return false;
    if (!makeKernel(m_etf,        "etf_comp",      16, 16, err)) return false;
    if (!makeKernel(m_flow,       "flow_comp",     16, 16, err)) return false;
    if (!makeKernel(m_clearU32,   "util_clear_u32", 64, 1, err)) return false;
    if (!makeKernel(m_clearTex,   "util_clear_tex", 16, 16, err)) return false;
    if (!makeKernel(m_copyTex,    "util_copy_tex",  16, 16, err)) return false;
    if (!makeKernel(m_ingest,     "util_ingest",    16, 16, err)) return false;
    if (!makeKernel(m_egress,     "util_egress",    16, 16, err)) return false;

    // Stroke ribbons: colour is premultiplied source-over, height accumulates.
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [m_lib newFunctionWithName:@"stroke_vert"];
    d.fragmentFunction = [m_lib newFunctionWithName:@"stroke_frag"];
    if (!d.vertexFunction || !d.fragmentFunction) {
        if (err) *err = "stroke_vert / stroke_frag not found in library";
        return false;
    }
    MTLRenderPipelineColorAttachmentDescriptor* a0 = d.colorAttachments[0];
    a0.pixelFormat = MTLPixelFormatRGBA16Float;
    a0.blendingEnabled = YES;
    a0.rgbBlendOperation = MTLBlendOperationAdd;
    a0.alphaBlendOperation = MTLBlendOperationAdd;
    a0.sourceRGBBlendFactor = MTLBlendFactorOne;
    a0.sourceAlphaBlendFactor = MTLBlendFactorOne;
    a0.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    a0.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    MTLRenderPipelineColorAttachmentDescriptor* a1 = d.colorAttachments[1];
    a1.pixelFormat = MTLPixelFormatR16Float;
    a1.blendingEnabled = YES;
    a1.rgbBlendOperation = MTLBlendOperationAdd;
    a1.alphaBlendOperation = MTLBlendOperationAdd;
    a1.sourceRGBBlendFactor = MTLBlendFactorOne;
    a1.sourceAlphaBlendFactor = MTLBlendFactorOne;
    a1.destinationRGBBlendFactor = MTLBlendFactorOne;
    a1.destinationAlphaBlendFactor = MTLBlendFactorOne;
    m_stroke = [m_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!m_stroke) {
        if (err) *err = std::string("stroke pipeline: ") +
                        (e ? [[e localizedDescription] UTF8String] : "creation failed");
        return false;
    }

    // Every texture the GL code samples is linear + clamp, the brush tile
    // wraps along the stroke and clamps across it.
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    m_sampLinear = [m_dev newSamplerStateWithDescriptor:sd];
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    m_sampRepeat = [m_dev newSamplerStateWithDescriptor:sd];

    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                   width:1 height:1 mipmapped:NO];
    dd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    dd.storageMode = MTLStorageModePrivate;
    m_dummy = [m_dev newTextureWithDescriptor:dd];
    m_dummyBuf = [m_dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];

    // Stroke buffers hold one chunk's worth of strokes and are never resized.
    const NSUInteger seedBytes   = NSUInteger(SBR_MAX_STROKES) * 32;
    const NSUInteger vertexBytes = NSUInteger(SBR_MAX_STROKES) * SBR_MAX_VERTS * 8;
    const NSUInteger headerBytes = NSUInteger(SBR_MAX_STROKES) * 16;
    auto priv = [&](NSUInteger n) {
        return [m_dev newBufferWithLength:n options:MTLResourceStorageModePrivate];
    };
    m_seedBuf = priv(seedBytes);
    m_vertexBuf = priv(vertexBytes);
    m_headerBuf = priv(headerBytes);
    m_counterBuf = priv(16);
    m_indirectBuf = priv(16);
    m_poolVertexBuf = priv(NSUInteger(SBR_POOL_MAX) * SBR_MAX_VERTS * 8);
    m_poolHeaderBuf = priv(NSUInteger(SBR_POOL_MAX) * 16);
    m_poolCounterBuf = priv(16);
    m_etfMaxBuf = priv(4);
    if (!m_seedBuf || !m_vertexBuf || !m_headerBuf || !m_poolVertexBuf || !m_poolHeaderBuf) {
        if (err) *err = "could not allocate stroke buffers";
        return false;
    }
    return true;
}

void MetalPipeline::shutdown() {
    commit(true);
    m_blur = m_features = m_error = m_seeds = m_trace = m_impasto = m_canvasProg =
        m_pool = m_relax = m_etf = m_flow = m_clearU32 = m_clearTex = m_copyTex =
        m_ingest = m_egress = Kernel();
    m_srcTex = m_refTex = m_tmpTex = m_tensorTex = m_gradTex = m_errTex = m_diffTex =
        m_etfTex = m_lumaPrevTex = m_lumaCurTex = m_flowTex = m_flowTmpTex =
        m_canvasTex = m_heightTex = m_prevSrcTex = m_prevCanvasTex = m_underTex =
        m_brushTex = m_outTex = nil;
    m_cellBuf = m_seedBuf = m_vertexBuf = m_headerBuf = m_counterBuf = m_indirectBuf =
        m_poolVertexBuf = m_poolHeaderBuf = m_poolCounterBuf = m_energyBuf = m_etfMaxBuf = nil;
    m_stroke = nil;
    m_lib = nil;
    m_queue = nil;
}

static id<MTLTexture> makeTexture(id<MTLDevice> dev, int w, int h, MTLPixelFormat fmt,
                                  bool mips, bool renderTarget) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                  width:NSUInteger(w)
                                                                                 height:NSUInteger(h)
                                                                              mipmapped:mips ? YES : NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (renderTarget || mips) d.usage |= MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:d];
}

void MetalPipeline::resize(int w, int h) {
    if (w == m_w && h == m_h) return;
    m_w = w; m_h = h;
    const MTLPixelFormat F16 = MTLPixelFormatRGBA16Float;
    // Mips on the source only so the 'average' underpaint can read the 1x1 level.
    m_srcTex        = makeTexture(m_dev, w, h, MTLPixelFormatRGBA8Unorm, true, false);
    m_refTex        = makeTexture(m_dev, w, h, F16, false, false);
    m_tmpTex        = makeTexture(m_dev, w, h, F16, false, false);
    m_tensorTex     = makeTexture(m_dev, w, h, F16, false, false);
    m_gradTex       = makeTexture(m_dev, w, h, F16, false, false);
    m_etfTex        = makeTexture(m_dev, w, h, F16, false, false);
    // The Lucas-Kanade pyramid is the mip chain.
    m_lumaPrevTex   = makeTexture(m_dev, w, h, MTLPixelFormatR16Float, true, false);
    m_lumaCurTex    = makeTexture(m_dev, w, h, MTLPixelFormatR16Float, true, false);
    m_flowW = std::max(1, w / 4);
    m_flowH = std::max(1, h / 4);
    m_flowTex       = makeTexture(m_dev, m_flowW, m_flowH, MTLPixelFormatRG16Float, false, false);
    m_flowTmpTex    = makeTexture(m_dev, m_flowW, m_flowH, MTLPixelFormatRG16Float, false, false);
    m_errTex        = makeTexture(m_dev, w, h, MTLPixelFormatR16Float, false, false);
    m_diffTex       = makeTexture(m_dev, w, h, MTLPixelFormatR16Float, false, false);
    m_canvasTex     = makeTexture(m_dev, w, h, F16, false, true);
    m_heightTex     = makeTexture(m_dev, w, h, MTLPixelFormatR16Float, false, true);
    m_prevSrcTex    = makeTexture(m_dev, w, h, MTLPixelFormatRGBA8Unorm, false, false);
    m_prevCanvasTex = makeTexture(m_dev, w, h, F16, false, false);
    m_underTex      = makeTexture(m_dev, w, h, F16, false, false);
    m_outTex        = makeTexture(m_dev, w, h, MTLPixelFormatBGRA8Unorm, false, false);

    m_havePrev = false;
    m_underValid = false;
    m_flowValid = false;

    // Two floats per 16x16 workgroup, shared by the ETF coherence pass.
    const uint32_t groups = divUp(uint32_t(w), 16) * divUp(uint32_t(h), 16);
    m_energyBuf = [m_dev newBufferWithLength:NSUInteger(groups) * 8
                                     options:MTLResourceStorageModePrivate];
}

// ── encoding helpers ────────────────────────────────────────────────────────

id<MTLCommandBuffer> MetalPipeline::commandBuffer() {
    if (!m_cb) m_cb = [m_queue commandBuffer];
    return m_cb;
}

void MetalPipeline::endEncoders() {
    if (m_ce) { [m_ce endEncoding]; m_ce = nil; }
    if (m_be) { [m_be endEncoding]; m_be = nil; }
    if (m_re) { [m_re endEncoding]; m_re = nil; }
}

id<MTLComputeCommandEncoder> MetalPipeline::computeEncoder() {
    if (m_ce) return m_ce;
    endEncoders();
    m_ce = [commandBuffer() computeCommandEncoder];
    for (NSUInteger i = 0; i < 8; ++i) [m_ce setSamplerState:m_sampLinear atIndex:i];
    return m_ce;
}

id<MTLBlitCommandEncoder> MetalPipeline::blitEncoder() {
    if (m_be) return m_be;
    endEncoders();
    m_be = [commandBuffer() blitCommandEncoder];
    return m_be;
}

void MetalPipeline::commit(bool wait) {
    endEncoders();
    if (!m_cb) return;
    id<MTLCommandBuffer> cb = m_cb;
    m_cb = nil;
    [cb commit];
    if (!wait) return;
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError)
        std::fprintf(stderr, "Metal command buffer failed: %s\n",
                     cb.error ? [[cb.error localizedDescription] UTF8String] : "unknown");
    m_gpuMs = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
}

void MetalPipeline::dispatch(const Kernel& k, uint32_t gx, uint32_t gy, const Bind& b,
                             const void* loose, size_t looseSize) {
    id<MTLComputeCommandEncoder> ce = computeEncoder();
    [ce setComputePipelineState:k.pso];
    [ce setBytes:&m_params length:sizeof(TuningParams) atIndex:PARAMS_BUFFER];
    if (loose) [ce setBytes:loose length:looseSize atIndex:LOOSE_BUFFER];
    for (const ArgInfo& a : k.args) {
        if (a.type == MTLArgumentTypeBuffer) {
            if (a.index == PARAMS_BUFFER || a.index == LOOSE_BUFFER) continue;
            id<MTLBuffer> buf = (a.index >= 0 && a.index < 16) ? b.buf[a.index] : nil;
            if (!buf) {
                std::fprintf(stderr, "paintify: no buffer bound at index %d\n", a.index);
                buf = m_dummyBuf;
            }
            [ce setBuffer:buf offset:0 atIndex:NSUInteger(a.index)];
        } else if (a.type == MTLArgumentTypeTexture) {
            id<MTLTexture> t = (a.index >= 0 && a.index < 16) ? b.tex[a.index] : nil;
            [ce setTexture:(t ? t : m_dummy) atIndex:NSUInteger(a.index)];
        }
    }
    [ce dispatchThreadgroups:MTLSizeMake(gx, gy, 1) threadsPerThreadgroup:k.threads];
}

void MetalPipeline::copyTex(id<MTLTexture> src, id<MTLTexture> dst) {
    Bind b;
    b.t(0, src).img(0, dst);
    dispatch(m_copyTex, divUp(uint32_t(dst.width), 16), divUp(uint32_t(dst.height), 16), b);
}

void MetalPipeline::clearTex(id<MTLTexture> t) {
    Bind b;
    b.img(0, t);
    dispatch(m_clearTex, divUp(uint32_t(t.width), 16), divUp(uint32_t(t.height), 16), b);
}

void MetalPipeline::clearU32(id<MTLBuffer> buf, uint32_t count) {
    Bind b;
    b.b(0, buf);
    dispatch(m_clearU32, divUp(count, 64), 1, b, &count, sizeof(count));
}

void MetalPipeline::generateMips(id<MTLTexture> t) {
    [blitEncoder() generateMipmapsForTexture:t];
}

void MetalPipeline::readBuffer(id<MTLBuffer> src, size_t bytes, void* dst) {
    id<MTLBuffer> rb = [m_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    [blitEncoder() copyFromBuffer:src sourceOffset:0 toBuffer:rb destinationOffset:0 size:bytes];
    commit(true);
    std::memcpy(dst, [rb contents], bytes);
}

void MetalPipeline::drawStrokes(id<MTLBuffer> vertexBuf, id<MTLBuffer> headerBuf) {
    endEncoders();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = m_canvasTex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].texture = m_heightTex;
    rp.colorAttachments[1].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [commandBuffer() renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:m_stroke];
    [re setVertexBuffer:vertexBuf offset:0 atIndex:BIND_VERTS];
    [re setVertexBuffer:headerBuf offset:0 atIndex:BIND_HEADERS];
    [re setVertexBytes:&m_params length:sizeof(TuningParams) atIndex:PARAMS_BUFFER];
    [re setFragmentBytes:&m_params length:sizeof(TuningParams) atIndex:PARAMS_BUFFER];
    [re setFragmentTexture:m_brushTex atIndex:UNIT_BRUSH];
    [re setFragmentSamplerState:m_sampRepeat atIndex:UNIT_BRUSH];
    // One triangle-strip instance per stroke; trace.comp / pool.comp wrote the
    // vertex count and instance count into m_indirectBuf.
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip
        indirectBuffer:m_indirectBuf
  indirectBufferOffset:0];
    [re endEncoding];
}

// ── sources ─────────────────────────────────────────────────────────────────

bool MetalPipeline::setSource(const unsigned char* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (w == m_w && h == m_h && m_srcTex) copyTex(m_srcTex, m_prevSrcTex);
    resize(w, h);
    const NSUInteger bytes = NSUInteger(w) * NSUInteger(h) * 4;
    id<MTLBuffer> staging = [m_dev newBufferWithBytes:rgba length:bytes
                                              options:MTLResourceStorageModeShared];
    [blitEncoder() copyFromBuffer:staging
                     sourceOffset:0
                sourceBytesPerRow:NSUInteger(w) * 4
              sourceBytesPerImage:bytes
                       sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                        toTexture:m_srcTex
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
    generateMips(m_srcTex);
    return true;
}

bool MetalPipeline::setSourceTexture(id<MTLTexture> tex, int w, int h, bool flipY) {
    if (!tex || w <= 0 || h <= 0) return false;
    if (w == m_w && h == m_h && m_srcTex) copyTex(m_srcTex, m_prevSrcTex);
    resize(w, h);
    Bind b;
    b.t(0, tex).img(0, m_srcTex);
    const uint32_t flip = flipY ? 1u : 0u;
    dispatch(m_ingest, divUp(uint32_t(w), 16), divUp(uint32_t(h), 16), b, &flip, sizeof(flip));
    generateMips(m_srcTex);
    return true;
}

void MetalPipeline::buildBrushTiles(const std::vector<float>& radii, float bristleDensity) {
    if (radii == m_brushRadii && bristleDensity == m_brushDensity && m_brushTex) return;

    brush::TileArray t = brush::makeTiles(radii, bristleDensity);
    std::vector<uint16_t> half(t.data.size());
    for (size_t i = 0; i < half.size(); ++i) half[i] = floatToHalf(t.data[i]);

    MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatR16Float;
    d.width = NSUInteger(brush::TILE_W);
    d.height = NSUInteger(brush::TILE_ROWS_MAX);
    d.arrayLength = NSUInteger(t.layers);
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [m_dev newTextureWithDescriptor:d];

    const NSUInteger rowBytes = NSUInteger(brush::TILE_W) * 2;
    const NSUInteger sliceBytes = rowBytes * NSUInteger(brush::TILE_ROWS_MAX);
    id<MTLBuffer> staging = [m_dev newBufferWithBytes:half.data()
                                               length:half.size() * sizeof(uint16_t)
                                              options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> be = blitEncoder();
    for (int layer = 0; layer < t.layers; ++layer) {
        [be copyFromBuffer:staging
              sourceOffset:NSUInteger(layer) * sliceBytes
         sourceBytesPerRow:rowBytes
       sourceBytesPerImage:sliceBytes
                sourceSize:MTLSizeMake(NSUInteger(brush::TILE_W), NSUInteger(brush::TILE_ROWS_MAX), 1)
                 toTexture:tex
          destinationSlice:NSUInteger(layer)
          destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
    }

    m_brushTex = tex;
    m_brushRows = t.rows;
    m_brushRadii = radii;
    m_brushDensity = bristleDensity;
}

void MetalPipeline::ensureCellCapacity(uint32_t cells) {
    if (cells <= m_cellCapacity && m_cellBuf) return;
    m_cellCapacity = cells;
    m_cellBuf = [m_dev newBufferWithLength:NSUInteger(cells) * 16
                                   options:MTLResourceStorageModePrivate];
}

// ── stages ──────────────────────────────────────────────────────────────────

// Separable Gaussian: src -> m_tmpTex (horizontal) -> dst (vertical).
void MetalPipeline::gaussian(id<MTLTexture> src, id<MTLTexture> dst) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    LooseBlur l{0};
    Bind h;
    h.t(UNIT_A, src).img(0, m_tmpTex);
    dispatch(m_blur, gx, gy, h, &l, sizeof(l));

    l.uPass = 1;
    Bind v;
    v.t(UNIT_A, m_tmpTex).img(0, dst);
    dispatch(m_blur, gx, gy, v, &l, sizeof(l));
}

void MetalPipeline::buildReference(TuningParams& p, float radius) {
    p.blurSigma = std::max(0.1f, radius * 0.5f);   // worker.js: max(0.1, radius * 0.5)
    uploadParams(p);
    gaussian(m_srcTex, m_refTex);
}

void MetalPipeline::buildGradients(const TuningParams& p) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    if (p.tensorSigma <= 0.f) {
        LooseFeatures l{0};
        Bind b;
        b.t(UNIT_A, m_refTex).img(0, m_gradTex);
        dispatch(m_features, gx, gy, b, &l, sizeof(l));
        return;
    }

    // Structure tensor: raw components -> Gaussian smooth -> eigenvector.
    LooseFeatures l{1};
    Bind b1;
    b1.t(UNIT_A, m_refTex).img(0, m_gradTex);
    dispatch(m_features, gx, gy, b1, &l, sizeof(l));

    TuningParams q = p;
    q.blurSigma = p.tensorSigma;
    uploadParams(q);
    gaussian(m_gradTex, m_tensorTex);
    uploadParams(p);

    l.uPass = 2;
    Bind b2;
    b2.t(UNIT_A, m_refTex).t(UNIT_B, m_tensorTex).img(0, m_gradTex);
    dispatch(m_features, gx, gy, b2, &l, sizeof(l));
}

// Edge Tangent Flow (Kang, Lee & Chui 2007), in place on m_gradTex.
void MetalPipeline::applyEtf(const TuningParams& p, const RenderConfig& cfg) {
    if (!(cfg.etfIterations > 0 && p.etfRadius >= 1.f)) return;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    clearU32(m_etfMaxBuf, 1);

    LooseEtf l{0};
    Bind b0;
    b0.t(UNIT_A, m_gradTex).b(BIND_ENERGY, m_energyBuf).b(BIND_ETF_MAX, m_etfMaxBuf);
    dispatch(m_etf, gx, gy, b0, &l, sizeof(l));

    l.uPass = 1;
    id<MTLTexture> src = m_gradTex, dst = m_etfTex;
    for (int i = 0; i < cfg.etfIterations; ++i) {
        Bind b;
        b.t(UNIT_A, src).img(0, dst).b(BIND_ENERGY, m_energyBuf).b(BIND_ETF_MAX, m_etfMaxBuf);
        dispatch(m_etf, gx, gy, b, &l, sizeof(l));
        std::swap(src, dst);
    }
    // An odd iteration count leaves the result in m_etfTex; downstream stages
    // read m_gradTex by name.
    if (src != m_gradTex) copyTex(m_etfTex, m_gradTex);
}

// Dense backward flow from the previous source frame to this one, by
// pyramidal Lucas-Kanade. See shaders/flow.comp.
void MetalPipeline::computeFlow(const TuningParams& p, const RenderConfig& cfg) {
    (void)p;
    m_flowValid = false;
    if (cfg.flowLevels <= 0 || !m_flowTex || !m_havePrev) return;

    constexpr int kFlowLevel = 2;
    const int coarsest = kFlowLevel + cfg.flowLevels - 1;
    const uint32_t fx = divUp(uint32_t(m_flowW), 16), fy = divUp(uint32_t(m_flowH), 16);
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    LooseFlow l{};
    l.uFlowSize[0] = m_flowW;
    l.uFlowSize[1] = m_flowH;
    l.uWindow = 3;                         // 7x7, in level pixels

    // Luma pyramids for both frames (pass 0), then box-filtered mips.
    l.uPass = 0;
    Bind prev;
    prev.t(UNIT_D, m_prevSrcTex).img(1, m_lumaPrevTex);
    dispatch(m_flow, gx, gy, prev, &l, sizeof(l));
    Bind cur;
    cur.t(UNIT_D, m_srcTex).img(1, m_lumaCurTex);
    dispatch(m_flow, gx, gy, cur, &l, sizeof(l));
    generateMips(m_lumaPrevTex);
    generateMips(m_lumaCurTex);

    // Start from zero displacement.
    clearTex(m_flowTex);

    id<MTLTexture> src = m_flowTex, dst = m_flowTmpTex;
    auto pass = [&](int which, int level) {
        l.uPass = which;
        l.uLevel = level;
        Bind b;
        b.t(UNIT_A, m_lumaPrevTex).t(UNIT_B, m_lumaCurTex).t(UNIT_C, src).img(0, dst);
        dispatch(m_flow, fx, fy, b, &l, sizeof(l));
        std::swap(src, dst);
    };

    for (int level = coarsest; level >= kFlowLevel; --level) {
        for (int it = 0; it < std::max(1, cfg.flowIterations); ++it) pass(1, level);
        pass(2, level);
    }

    if (src != m_flowTex) copyTex(m_flowTmpTex, m_flowTex);
    m_flowValid = true;
}

// Lays down the starting canvas: worker.js `applyUnderpaint`, or the previous
// frame's paint in temporal mode.
void MetalPipeline::layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                                  const std::vector<float>& radii, bool temporal,
                                  bool allowCache) {
    const uint32_t gxi = divUp(uint32_t(m_w), 16), gyi = divUp(uint32_t(m_h), 16);

    if (allowCache && m_underValid) {
        copyTex(m_underTex, m_canvasTex);
        clearTex(m_heightTex);
        return;
    }

    if (temporal) {
        if (m_flowValid) {
            // Advect the previous paint along the flow first.
            LooseCanvas l{};
            l.uPass = 3;
            Bind b;
            b.t(UNIT_E, m_flowTex).t(UNIT_F, m_prevCanvasTex).img(0, m_canvasTex);
            dispatch(m_canvasProg, gxi, gyi, b, &l, sizeof(l));
        } else {
            copyTex(m_prevCanvasTex, m_canvasTex);
        }

        LooseCanvas l{};
        l.uPass = 2;
        l.uUseFlow = m_flowValid ? 1 : 0;
        Bind b;
        b.t(UNIT_B, m_srcTex).t(UNIT_C, m_prevSrcTex).t(UNIT_E, m_flowTex).img(1, m_diffTex);
        dispatch(m_canvasProg, gxi, gyi, b, &l, sizeof(l));
    } else if (cfg.underpaint == Underpaint::Blur) {
        // The coarsest radius' Gaussian, per applyUnderpaint('blur').
        buildReference(p, radii[0]);
        LooseCanvas l{};
        l.uPass = 0;
        Bind b;
        b.t(UNIT_A, m_refTex).img(0, m_canvasTex);
        dispatch(m_canvasProg, gxi, gyi, b, &l, sizeof(l));
    } else {
        float fill[4] = {1.f, 1.f, 1.f, 1.f};
        if (cfg.underpaint == Underpaint::Average) {
            // The 1x1 mip is the image mean. It has to be read back, which
            // means running everything encoded so far.
            const int levels = 1 + int(std::floor(std::log2(float(std::max(m_w, m_h)))));
            id<MTLBuffer> rb = [m_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            [blitEncoder() copyFromTexture:m_srcTex
                               sourceSlice:0
                               sourceLevel:NSUInteger(levels - 1)
                              sourceOrigin:MTLOriginMake(0, 0, 0)
                                sourceSize:MTLSizeMake(1, 1, 1)
                                  toBuffer:rb
                         destinationOffset:0
                    destinationBytesPerRow:4
                  destinationBytesPerImage:4];
            commit(true);
            const unsigned char* px = static_cast<const unsigned char*>([rb contents]);
            for (int i = 0; i < 4; ++i) fill[i] = float(px[i]) / 255.f;
        }
        LooseCanvas l{};
        l.uPass = 1;
        l.uFill[0] = fill[0]; l.uFill[1] = fill[1]; l.uFill[2] = fill[2];
        Bind b;
        b.img(0, m_canvasTex);
        dispatch(m_canvasProg, gxi, gyi, b, &l, sizeof(l));
    }

    // Keep the ground so a relaxation repaint can copy it back.
    copyTex(m_canvasTex, m_underTex);
    m_underValid = true;
    clearTex(m_heightTex);
}

// Repaints the canvas from the stroke pool: underpaint, then every pooled
// stroke in one instanced draw.
void MetalPipeline::repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                                    const std::vector<float>& radii) {
    TuningParams q = p;
    layUnderpaint(q, cfg, radii, /*temporal=*/false, /*allowCache=*/true);
    uploadParams(p);

    // The draw command comes from the pool count, written GPU-side.
    LoosePool l{1, SBR_POOL_MAX};
    Bind b;
    b.b(BIND_VERTS, m_vertexBuf).b(BIND_COUNTERS, m_counterBuf).b(BIND_HEADERS, m_headerBuf)
     .b(BIND_INDIRECT, m_indirectBuf).b(BIND_POOL_VERTS, m_poolVertexBuf)
     .b(BIND_POOL_HEADERS, m_poolHeaderBuf).b(BIND_POOL_COUNTER, m_poolCounterBuf);
    dispatch(m_pool, 1, 1, b, &l, sizeof(l));

    drawStrokes(m_poolVertexBuf, m_poolHeaderBuf);
}

// Hertzmann 2001. The greedy phase has placed the strokes; now improve them.
void MetalPipeline::relax(TuningParams& p, const RenderConfig& cfg,
                          const std::vector<float>& radii) {
    uint32_t counters[4] = {0, 0, 0, 0};
    readBuffer(m_poolCounterBuf, sizeof(counters), counters);
    m_poolCount = counters[0];

    if (m_poolCount == 0) return;
    if (m_poolCount > SBR_POOL_MAX) {
        // pool.comp dropped the overflow, so a repaint from the pool would lose
        // strokes the greedy phase did paint; leave the greedy canvas alone.
        std::fprintf(stderr,
                     "paintify: %u strokes exceeds the relaxation pool (%u); skipping"
                     " relaxation for this frame.\n", m_poolCount, SBR_POOL_MAX);
        m_poolCount = SBR_POOL_MAX;
        return;
    }

    const uint32_t subs = uint32_t(std::max(1, cfg.relaxSubPasses));
    for (int iter = 0; iter < cfg.relaxIterations; ++iter) {
        p.relaxIter = float(iter);
        uploadParams(p);

        for (uint32_t sub = 0; sub < subs; ++sub) {
            LooseRelax l{SBR_POOL_MAX, sub, subs};
            Bind b;
            b.t(UNIT_A, m_srcTex).t(UNIT_B, m_underTex)
             .b(BIND_POOL_VERTS, m_poolVertexBuf).b(BIND_POOL_HEADERS, m_poolHeaderBuf)
             .b(BIND_POOL_COUNTER, m_poolCounterBuf);
            dispatch(m_relax, divUp(m_poolCount, 64), 1, b, &l, sizeof(l));
        }
        repaintFromPool(p, cfg, radii);
    }
}

// ── the frame ───────────────────────────────────────────────────────────────

void MetalPipeline::render(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
    if (!m_w || !m_h) return;

    // paintify(): drop radii below 1 and paint coarse to fine.
    std::vector<float> radii = cfg.radii;
    radii.erase(std::remove_if(radii.begin(), radii.end(),
                               [](float r) { return !(r >= 1.f); }),
                radii.end());
    if (radii.empty()) radii.push_back(4.f);
    std::sort(radii.begin(), radii.end(), std::greater<float>());
    if (radii.size() > SBR_MAX_LAYERS) radii.resize(SBR_MAX_LAYERS);

    buildBrushTiles(radii, cfg.bristleDensity);

    TuningParams p = base;
    p.imgW = float(m_w);  p.imgH = float(m_h);
    p.texelW = 1.f / float(m_w); p.texelH = 1.f / float(m_h);
    p.layerCount = float(radii.size());
    p.seedBudget = float(SBR_MAX_STROKES);

    const uint32_t gxi = divUp(uint32_t(m_w), 16), gyi = divUp(uint32_t(m_h), 16);
    const bool useTemporal = temporal && m_havePrev && p.frameDiffThreshold > 0.f;

    m_poolCount = 0;
    m_underValid = false;              // this frame's ground is not built yet
    uploadParams(p);

    // --- canvas setup ----------------------------------------------------
    // The flow has to exist before the underpaint, which is what consumes it.
    if (useTemporal) computeFlow(p, cfg);
    else m_flowValid = false;

    layUnderpaint(p, cfg, radii, useTemporal);

    // The frame-wide pool. trace.comp reserves a slot range per pass and
    // pool.comp copies into it, so relaxation has every stroke of the frame.
    const bool wantRelax = cfg.relaxIterations > 0;
    clearU32(m_poolCounterBuf, 4);

    // --- per layer, coarse to fine ---------------------------------------
    for (size_t l = 0; l < radii.size(); ++l) {
        const float radius = radii[l];
        p.currentLayer = float(l);
        p.layerRadius = radius;
        p.firstLayer = (l == 0) ? 1.f : 0.f;
        const int grid = std::max(1, int(std::lround(radius * p.gridFactor)));
        p.gridSpacing = float(grid);
        p.texRows = float(l < m_brushRows.size() ? m_brushRows[l] : 16);
        p.texLayerBase = float(l * size_t(brush::VARIANTS));
        p.texUScale = float(brush::TILE_W) / std::max(16.f, radius * 4.f);

        const uint32_t gridW = divUp(uint32_t(m_w), uint32_t(grid));
        const uint32_t gridH = divUp(uint32_t(m_h), uint32_t(grid));
        const uint32_t cells = gridW * gridH;
        ensureCellCapacity(cells);

        // Passes over this layer's cells: enough to fit the stroke buffers, and
        // enough for the intra-layer feedback that regulates stroke length.
        const uint32_t needed = std::max(1u, divUp(cells, SBR_MAX_STROKES));
        const uint32_t wanted = uint32_t(std::max(1, cfg.passesPerLayer));
        const uint32_t chunks = std::min(MAX_CHUNKS, std::max(needed, wanted));
        p.chunkCount = float(chunks);

        // Reference blur and gradient field at this layer's scale.
        buildReference(p, radius);
        buildGradients(p);
        applyEtf(p, cfg);

        // --- error map + per-cell reduction ------------------------------
        {
            LooseError le{0, useTemporal ? 1 : 0, m_cellCapacity};
            Bind b0;
            b0.t(UNIT_A, m_refTex).t(UNIT_B, m_canvasTex).img(0, m_errTex).b(BIND_CELLS, m_cellBuf);
            dispatch(m_error, gxi, gyi, b0, &le, sizeof(le));

            le.uPass = 1;
            Bind b1;
            b1.t(UNIT_C, m_errTex).t(UNIT_D, m_diffTex).b(BIND_CELLS, m_cellBuf);
            dispatch(m_error, divUp(gridW, 16), divUp(gridH, 16), b1, &le, sizeof(le));
        }

        const uint32_t traceBound =
            std::min(SBR_MAX_STROKES, divUp(cells, chunks) + 1024u);

        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            p.chunk = float(chunk);
            uploadParams(p);

            // --- seeds ---------------------------------------------------
            clearU32(m_counterBuf, 4);
            {
                LooseSeeds ls{useTemporal ? 1 : 0};
                Bind b;
                b.t(UNIT_A, m_refTex).b(BIND_CELLS, m_cellBuf).b(BIND_SEEDS, m_seedBuf)
                 .b(BIND_COUNTERS, m_counterBuf);
                dispatch(m_seeds, divUp(gridW, 8), divUp(gridH, 8), b, &ls, sizeof(ls));
            }

            // --- trace ---------------------------------------------------
            {
                Bind b;
                b.t(UNIT_A, m_refTex).t(UNIT_B, m_canvasTex).t(UNIT_C, m_gradTex)
                 .b(BIND_SEEDS, m_seedBuf).b(BIND_VERTS, m_vertexBuf)
                 .b(BIND_COUNTERS, m_counterBuf).b(BIND_HEADERS, m_headerBuf)
                 .b(BIND_INDIRECT, m_indirectBuf).b(BIND_POOL_COUNTER, m_poolCounterBuf);
                dispatch(m_trace, divUp(traceBound, 64), 1, b);
            }

            // --- rasterise -----------------------------------------------
            drawStrokes(m_vertexBuf, m_headerBuf);

            // --- append this pass to the frame pool ----------------------
            if (wantRelax) {
                LoosePool lp{0, SBR_POOL_MAX};
                Bind b;
                b.b(BIND_VERTS, m_vertexBuf).b(BIND_COUNTERS, m_counterBuf)
                 .b(BIND_HEADERS, m_headerBuf).b(BIND_INDIRECT, m_indirectBuf)
                 .b(BIND_POOL_VERTS, m_poolVertexBuf).b(BIND_POOL_HEADERS, m_poolHeaderBuf)
                 .b(BIND_POOL_COUNTER, m_poolCounterBuf);
                dispatch(m_pool, divUp(traceBound, 64), 1, b, &lp, sizeof(lp));
            }
        }
    }

    // --- relaxation -------------------------------------------------------
    if (wantRelax) relax(p, cfg, radii);

    // --- impasto lighting -------------------------------------------------
    // Metal read-write textures are not universal: light a copy, write back.
    if (p.impastoLight > 0.f) {
        copyTex(m_canvasTex, m_tmpTex);
        Bind b;
        b.t(UNIT_A, m_heightTex).t(UNIT_B, m_tmpTex).img(0, m_canvasTex);
        dispatch(m_impasto, gxi, gyi, b);
    }

    // Keep this frame's canvas for the next temporal frame.
    copyTex(m_canvasTex, m_prevCanvasTex);
    m_havePrev = true;
}

// ── output ──────────────────────────────────────────────────────────────────

id<MTLTexture> MetalPipeline::encodeOutput() {
    Bind b;
    b.t(0, m_canvasTex).img(0, m_outTex);
    dispatch(m_egress, divUp(uint32_t(m_w), 16), divUp(uint32_t(m_h), 16), b);
    return m_outTex;
}

std::vector<unsigned char> MetalPipeline::readCanvas() {
    std::vector<unsigned char> px(size_t(m_w) * size_t(m_h) * 4);
    if (!m_w || !m_h) return px;
    encodeOutput();
    const NSUInteger bytes = px.size();
    id<MTLBuffer> rb = [m_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    [blitEncoder() copyFromTexture:m_outTex
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(NSUInteger(m_w), NSUInteger(m_h), 1)
                          toBuffer:rb
                 destinationOffset:0
            destinationBytesPerRow:NSUInteger(m_w) * 4
          destinationBytesPerImage:bytes];
    commit(true);
    std::memcpy(px.data(), [rb contents], bytes);
    for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // BGRA -> RGBA
    return px;
}
