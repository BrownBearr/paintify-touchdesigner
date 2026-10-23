#include "mtl_pipeline.h"
#include "brush_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>

using mtl::Tex;
using mtl::Kernel;

namespace {

// SSBO binding points, mirrored in the shaders (and in pipeline.cpp).
enum : int {
    BIND_CELLS = 0,
    BIND_SEEDS = 1,
    BIND_VERTS = 2,
    BIND_COUNTERS = 3,
    BIND_HEADERS = 4,
    BIND_INDIRECT = 5,
    BIND_POOL_VERTS = 6,
    BIND_POOL_HEADERS = 7,
    BIND_POOL_COUNTER = 8,
    BIND_ENERGY = 9,
    BIND_ETF_MAX = 10,
};

// Texture units, mirrored in the shaders.
enum : int {
    UNIT_A = 0, UNIT_B = 1, UNIT_C = 2, UNIT_D = 3, UNIT_BRUSH = 4, UNIT_E = 5, UNIT_F = 6,
};

// Metal slots the translated shaders use for GL's other binding spaces.
constexpr int SLOT_PARAMS = 0;
constexpr int SLOT_SSBO_BASE = 1;
constexpr int SLOT_IMAGE_BASE = 8;
constexpr int SLOT_LOCALS = 12;
constexpr int SLOT_BUFFER_SIZES = 25;

constexpr uint32_t MAX_CHUNKS = 32u;

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Format conversion and fills that GL did with glClearTexImage,
// glGetTextureImage and the Spout copy. Hand-written, not translated.
const char* kUtilSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void fill(texture2d<float, access::write> dst [[texture(0)]],
                 constant float4& v [[buffer(0)]],
                 uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(v, gid);
}

// Copies src into dst texel for texel, optionally flipping rows. Any float
// format on either side: reading BGRA returns rgba order and writing BGRA
// swizzles back, so this is also the BGRA <-> RGBA conversion.
kernel void convert(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant uint& flip [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]]) {
    uint w = dst.get_width(), h = dst.get_height();
    if (gid.x >= w || gid.y >= h) return;
    uint y = flip != 0u ? h - 1u - gid.y : gid.y;
    dst.write(clamp(src.read(uint2(gid.x, y)), 0.0, 1.0), gid);
}
)MSL";

std::string readText(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *ok = false; return {}; }
    std::stringstream ss;
    ss << f.rdbuf();
    *ok = true;
    return ss.str();
}

uint32_t dataTypeSize(MTLDataType t) {
    switch (t) {
        case MTLDataTypeInt2: case MTLDataTypeUInt2: case MTLDataTypeFloat2: return 8;
        case MTLDataTypeInt3: case MTLDataTypeUInt3: case MTLDataTypeFloat3: return 12;
        case MTLDataTypeInt4: case MTLDataTypeUInt4: case MTLDataTypeFloat4: return 16;
        default: return 4;
    }
}

} // namespace

void Kernel::set(const char* name, const void* v, size_t n) {
    auto it = fields.find(name);
    if (it == fields.end()) {
        fprintf(stderr, "paintify: shader has no uniform '%s'\n", name);
        return;
    }
    std::memcpy(locals.data() + it->second.offset, v, std::min<size_t>(n, it->second.size));
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static MTLCompileOptions* compileOptions() {
    MTLCompileOptions* o = [MTLCompileOptions new];
    // GL does not reassociate or flush denormals; the Lab error metric and the
    // threshold calibrated against it are sensitive to that, so match GL.
    if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return o;
}

id<MTLLibrary> MetalPipeline::loadLibrary(const std::string& file, std::string* err) {
    bool ok = false;
    const std::string src = readText(m_shaderDir + "/" + file, &ok);
    if (!ok) { if (err) *err = "cannot open " + m_shaderDir + "/" + file; return nil; }
    NSError* e = nil;
    id<MTLLibrary> lib = [m_dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                             options:compileOptions() error:&e];
    if (!lib && err) *err = file + ":\n" + std::string(e.localizedDescription.UTF8String);
    return lib;
}

bool MetalPipeline::buildKernel(Kernel& k, const std::string& file, MTLSize group, std::string* err) {
    id<MTLLibrary> lib = loadLibrary(file, err);
    if (!lib) return false;
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    NSError* e = nil;
    MTLComputePipelineReflection* refl = nil;
    k.pso = [m_dev newComputePipelineStateWithFunction:fn
                                               options:MTLPipelineOptionBindingInfo |
                                                       MTLPipelineOptionBufferTypeInfo
                                            reflection:&refl error:&e];
    if (!k.pso) {
        if (err) *err = file + ": " + std::string(e.localizedDescription.UTF8String);
        return false;
    }
    k.group = group;
    // The loose uniforms' layout comes from reflection rather than a
    // hand-written struct per shader: spirv-cross lays them out by MSL rules
    // (a float3 is 16 bytes), and reading the offsets back is the only way
    // that cannot drift from what the compiler actually produced.
    for (id<MTLBinding> b in refl.bindings) {
        if (b.type != MTLBindingTypeBuffer || b.index != SLOT_LOCALS) continue;
        id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
        k.locals.assign(bb.bufferDataSize, 0);
        for (MTLStructMember* m in bb.bufferStructType.members)
            k.fields[m.name.UTF8String] = {uint32_t(m.offset), dataTypeSize(m.dataType)};
    }
    return true;
}

bool MetalPipeline::init(id<MTLDevice> device, const std::string& shaderDir, std::string* err) {
    m_dev = device;
    m_queue = [m_dev newCommandQueue];
    m_shaderDir = shaderDir;

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m_linear = [m_dev newSamplerStateWithDescriptor:sd];
    sd.mipFilter = MTLSamplerMipFilterLinear;
    m_linearMip = [m_dev newSamplerStateWithDescriptor:sd];
    // u wraps -- the tile is seamless along the stroke. v must clamp, or the
    // bristle profile folds over at the stroke's edges.
    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    m_brushSampler = [m_dev newSamplerStateWithDescriptor:sd];

    const MTLSize g16 = MTLSizeMake(16, 16, 1), g8 = MTLSizeMake(8, 8, 1), g64 = MTLSizeMake(64, 1, 1);
    struct { Kernel* k; const char* file; MTLSize g; } stages[] = {
        {&m_blur, "blur_comp.metal", g16},       {&m_features, "features_comp.metal", g16},
        {&m_error, "error_comp.metal", g16},     {&m_seeds, "seeds_comp.metal", g8},
        {&m_trace, "trace_comp.metal", g64},     {&m_impasto, "impasto_comp.metal", g16},
        {&m_canvasProg, "canvas_comp.metal", g16}, {&m_pool, "pool_comp.metal", g64},
        {&m_relax, "relax_comp.metal", g64},     {&m_etf, "etf_comp.metal", g16},
        {&m_flow, "flow_comp.metal", g16},
    };
    for (auto& s : stages)
        if (!buildKernel(*s.k, s.file, s.g, err)) return false;

    // Utility kernels.
    {
        NSError* e = nil;
        id<MTLLibrary> lib = [m_dev newLibraryWithSource:@(kUtilSource) options:compileOptions() error:&e];
        if (!lib) { if (err) *err = e.localizedDescription.UTF8String; return false; }
        Kernel* ks[] = {&m_fill, &m_convert};
        NSString* names[] = {@"fill", @"convert"};
        for (int i = 0; i < 2; ++i) {
            ks[i]->pso = [m_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:names[i]] error:&e];
            ks[i]->group = g16;
            if (!ks[i]->pso) { if (err) *err = e.localizedDescription.UTF8String; return false; }
        }
    }

    // Stroke rasteriser: colour blends premultiplied source-over, height
    // accumulates -- glBlendFunci(0, ONE, ONE_MINUS_SRC_ALPHA), (1, ONE, ONE).
    {
        id<MTLLibrary> vl = loadLibrary("stroke_vert.metal", err);
        id<MTLLibrary> fl = loadLibrary("stroke_frag.metal", err);
        if (!vl || !fl) return false;
        MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [vl newFunctionWithName:@"main0"];
        rd.fragmentFunction = [fl newFunctionWithName:@"main0"];
        rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        MTLRenderPipelineColorAttachmentDescriptor* c0 = rd.colorAttachments[0];
        c0.pixelFormat = MTLPixelFormatRGBA16Float;
        c0.blendingEnabled = YES;
        c0.sourceRGBBlendFactor = c0.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c0.destinationRGBBlendFactor = c0.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        MTLRenderPipelineColorAttachmentDescriptor* c1 = rd.colorAttachments[1];
        c1.pixelFormat = MTLPixelFormatR16Float;
        c1.blendingEnabled = YES;
        c1.sourceRGBBlendFactor = c1.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c1.destinationRGBBlendFactor = c1.destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError* e = nil;
        m_stroke = [m_dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!m_stroke) { if (err) *err = std::string("stroke: ") + e.localizedDescription.UTF8String; return false; }
    }

    const size_t seedBytes   = size_t(SBR_MAX_STROKES) * 32;
    const size_t vertexBytes = size_t(SBR_MAX_STROKES) * SBR_MAX_VERTS * 8;
    const size_t headerBytes = size_t(SBR_MAX_STROKES) * 16;
    m_seedBuf     = makeBuf(seedBytes);
    m_vertexBuf   = makeBuf(vertexBytes);
    m_headerBuf   = makeBuf(headerBytes);
    m_counterBuf  = makeBuf(16);
    m_indirectBuf = makeBuf(16);
    m_statsBuf    = makeBuf(size_t(SBR_MAX_LAYERS) * MAX_CHUNKS * 16);
    m_poolVertexBuf  = makeBuf(size_t(SBR_POOL_MAX) * SBR_MAX_VERTS * 8);
    m_poolHeaderBuf  = makeBuf(size_t(SBR_POOL_MAX) * 16);
    m_poolCounterBuf = makeBuf(16);
    m_etfMaxBuf = makeBuf(16);

    // Placeholders for declared-but-unused bindings. GL tolerates a shader
    // naming a unit nothing is bound to; Metal's validation does not.
    m_dummyTex = makeTex(1, 1, MTLPixelFormatRGBA16Float);
    m_dummyBuf = makeBuf(256);
    return true;
}

void MetalPipeline::shutdown() {
    flush(true);
}

Tex MetalPipeline::makeTex(int w, int h, MTLPixelFormat f, bool mips) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                 width:NSUInteger(w)
                                                                                height:NSUInteger(h)
                                                                             mipmapped:mips ? YES : NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    Tex t;
    t.t = [m_dev newTextureWithDescriptor:d];
    t.s = mips ? m_linearMip : m_linear;
    return t;
}

id<MTLBuffer> MetalPipeline::makeBuf(size_t bytes) {
    // Shared storage: Apple GPUs have unified memory, and it makes the few
    // host readbacks (stats, pool count) a plain pointer read.
    return [m_dev newBufferWithLength:std::max<size_t>(bytes, 16) options:MTLResourceStorageModeShared];
}

// ---------------------------------------------------------------------------
// Command encoding
// ---------------------------------------------------------------------------

id<MTLComputeCommandEncoder> MetalPipeline::computeEncoder() {
    if (m_ce) return m_ce;
    endEncoder();
    if (!m_cb) m_cb = [m_queue commandBuffer];
    // Serial dispatch with tracked resources: each dispatch sees every write
    // of the one before it, which is what the glMemoryBarrier calls bought.
    m_ce = [m_cb computeCommandEncoder];
    return m_ce;
}

id<MTLBlitCommandEncoder> MetalPipeline::blitEncoder() {
    if (m_be) return m_be;
    endEncoder();
    if (!m_cb) m_cb = [m_queue commandBuffer];
    m_be = [m_cb blitCommandEncoder];
    return m_be;
}

void MetalPipeline::endEncoder() {
    if (m_ce) { [m_ce endEncoding]; m_ce = nil; }
    if (m_be) { [m_be endEncoding]; m_be = nil; }
}

void MetalPipeline::flush(bool wait) {
    endEncoder();
    if (!m_cb) return;
    id<MTLCommandBuffer> cb = m_cb;
    m_cb = nil;
    [cb commit];
    if (!wait) return;
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError)
        fprintf(stderr, "paintify: GPU error: %s\n", cb.error.localizedDescription.UTF8String);
    m_gpuMs += (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
}

void MetalPipeline::dispatch(Kernel& k, uint32_t gx, uint32_t gy) {
    id<MTLComputeCommandEncoder> e = computeEncoder();
    [e setComputePipelineState:k.pso];
    [e setBytes:&m_p length:sizeof(TuningParams) atIndex:SLOT_PARAMS];
    if (!k.locals.empty())
        [e setBytes:k.locals.data() length:k.locals.size() atIndex:SLOT_LOCALS];
    for (int u = 0; u < 8; ++u) {
        if (u == UNIT_BRUSH) continue;   // a texture array; only the rasteriser reads it
        const Tex& t = m_unit[u] ? m_unit[u] : m_dummyTex;
        [e setTexture:t.t atIndex:NSUInteger(u)];
        [e setSamplerState:t.s atIndex:NSUInteger(u)];
    }
    for (int i = 0; i < 4; ++i)
        [e setTexture:(m_image[i] ? m_image[i] : m_dummyTex.t) atIndex:NSUInteger(SLOT_IMAGE_BASE + i)];
    // spirv-cross implements `buffer.length()` from this table, indexed by
    // the buffer's Metal slot.
    uint32_t sizes[SLOT_BUFFER_SIZES + 1] = {0};
    for (int i = 0; i < 11; ++i) {
        id<MTLBuffer> b = m_ssbo[i] ? m_ssbo[i] : m_dummyBuf;
        [e setBuffer:b offset:0 atIndex:NSUInteger(SLOT_SSBO_BASE + i)];
        sizes[SLOT_SSBO_BASE + i] = uint32_t(b.length);
    }
    [e setBytes:sizes length:sizeof(sizes) atIndex:SLOT_BUFFER_SIZES];
    [e dispatchThreadgroups:MTLSizeMake(gx, gy, 1) threadsPerThreadgroup:k.group];
}

void MetalPipeline::copyTex(const Tex& src, const Tex& dst) {
    const MTLSize size = MTLSizeMake(dst.t.width, dst.t.height, 1);
    [blitEncoder() copyFromTexture:src.t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:size toTexture:dst.t destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
}

void MetalPipeline::clearBuf(id<MTLBuffer> b, size_t bytes) {
    [blitEncoder() fillBuffer:b range:NSMakeRange(0, std::min<size_t>(bytes, b.length)) value:0];
}

void MetalPipeline::fillTex(const Tex& t, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    id<MTLComputeCommandEncoder> e = computeEncoder();
    [e setComputePipelineState:m_fill.pso];
    [e setTexture:t.t atIndex:0];
    [e setBytes:v length:sizeof(v) atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(divUp(uint32_t(t.t.width), 16), divUp(uint32_t(t.t.height), 16), 1)
      threadsPerThreadgroup:m_fill.group];
}

void MetalPipeline::convert(id<MTLTexture> src, id<MTLTexture> dst, bool flipY) {
    const uint32_t flip = flipY ? 1u : 0u;
    id<MTLComputeCommandEncoder> e = computeEncoder();
    [e setComputePipelineState:m_convert.pso];
    [e setTexture:src atIndex:0];
    [e setTexture:dst atIndex:1];
    [e setBytes:&flip length:4 atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(divUp(uint32_t(dst.width), 16), divUp(uint32_t(dst.height), 16), 1)
      threadsPerThreadgroup:m_convert.group];
}

void MetalPipeline::genMips(const Tex& t) {
    [blitEncoder() generateMipmapsForTexture:t.t];
}

// ---------------------------------------------------------------------------
// Ported from Pipeline
// ---------------------------------------------------------------------------

void MetalPipeline::resize(int w, int h) {
    if (w == m_w && h == m_h) return;
    flush(true);
    m_w = w; m_h = h;
    m_srcTex        = makeTex(w, h, MTLPixelFormatRGBA8Unorm, /*mips=*/true);
    m_refTex        = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_tmpTex        = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_tensorTex     = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_gradTex       = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_etfTex        = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_lumaPrevTex   = makeTex(w, h, MTLPixelFormatR16Float, /*mips=*/true);
    m_lumaCurTex    = makeTex(w, h, MTLPixelFormatR16Float, /*mips=*/true);
    m_flowW = std::max(1, w / 4);
    m_flowH = std::max(1, h / 4);
    m_flowTex       = makeTex(m_flowW, m_flowH, MTLPixelFormatRG16Float);
    m_flowTmpTex    = makeTex(m_flowW, m_flowH, MTLPixelFormatRG16Float);
    m_errTex        = makeTex(w, h, MTLPixelFormatR16Float);
    m_diffTex       = makeTex(w, h, MTLPixelFormatR16Float);
    m_canvasTex     = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_heightTex     = makeTex(w, h, MTLPixelFormatR16Float);
    m_prevSrcTex    = makeTex(w, h, MTLPixelFormatRGBA8Unorm);
    m_prevCanvasTex = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_underTex      = makeTex(w, h, MTLPixelFormatRGBA16Float);
    m_outTex        = makeTex(w, h, MTLPixelFormatBGRA8Unorm);
    m_readTex       = makeTex(w, h, MTLPixelFormatRGBA8Unorm);

    m_havePrev = false;
    m_underValid = false;
    m_flowValid = false;

    // Two floats per 16x16 workgroup, shared by energy.comp and etf.comp.
    const uint32_t groups = divUp(uint32_t(w), 16) * divUp(uint32_t(h), 16);
    m_energyBuf = makeBuf(size_t(groups) * 8);
    bindBuf(BIND_ENERGY, m_energyBuf);
}

bool MetalPipeline::setSource(const unsigned char* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (w == m_w && h == m_h && m_srcTex) copyTex(m_srcTex, m_prevSrcTex);
    resize(w, h);
    const size_t rowBytes = size_t(w) * 4;
    id<MTLBuffer> staging = [m_dev newBufferWithBytes:rgba length:rowBytes * size_t(h)
                                              options:MTLResourceStorageModeShared];
    [blitEncoder() copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:rowBytes
                sourceBytesPerImage:rowBytes * size_t(h) sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                          toTexture:m_srcTex.t destinationSlice:0 destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
    genMips(m_srcTex);
    return true;
}

bool MetalPipeline::setSourceTexture(id<MTLTexture> texture, bool flipY) {
    if (!texture) return false;
    const int w = int(texture.width), h = int(texture.height);
    if (w <= 0 || h <= 0) return false;
    if (w == m_w && h == m_h && m_srcTex) copyTex(m_srcTex, m_prevSrcTex);
    resize(w, h);
    convert(texture, m_srcTex.t, flipY);
    genMips(m_srcTex);
    return true;
}

void MetalPipeline::buildBrushTiles(const std::vector<float>& radii, float bristleDensity) {
    if (radii == m_brushRadii && bristleDensity == m_brushDensity && m_brushTex) return;

    brush::TileArray t = brush::makeTiles(radii, bristleDensity);
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DArray;
    d.pixelFormat = MTLPixelFormatR16Float;
    d.width = brush::TILE_W;
    d.height = brush::TILE_ROWS_MAX;
    d.arrayLength = NSUInteger(t.layers);
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    m_brushTex.t = [m_dev newTextureWithDescriptor:d];
    m_brushTex.s = m_brushSampler;

    const size_t slice = size_t(brush::TILE_W) * brush::TILE_ROWS_MAX;
    std::vector<__fp16> half(slice);
    for (int l = 0; l < t.layers; ++l) {
        for (size_t i = 0; i < slice; ++i) half[i] = __fp16(t.data[size_t(l) * slice + i]);
        [m_brushTex.t replaceRegion:MTLRegionMake2D(0, 0, brush::TILE_W, brush::TILE_ROWS_MAX)
                        mipmapLevel:0 slice:NSUInteger(l) withBytes:half.data()
                        bytesPerRow:brush::TILE_W * sizeof(__fp16) bytesPerImage:slice * sizeof(__fp16)];
    }
    m_brushRows = t.rows;
    m_brushRadii = radii;
    m_brushDensity = bristleDensity;
}

void MetalPipeline::ensureCellCapacity(uint32_t cells) {
    if (cells <= m_cellCapacity && m_cellBuf) return;
    m_cellCapacity = cells;
    m_cellBuf = makeBuf(size_t(cells) * 16);
}

void MetalPipeline::readStats() {
    const uint32_t* counts = static_cast<const uint32_t*>(m_statsBuf.contents);
    m_lastStrokes = 0; m_lastDrawn = 0; m_lastPoints = 0;
    for (size_t l = 0; l < m_layerCount; ++l) {
        for (uint32_t c = 0; c < MAX_CHUNKS; ++c) {
            const uint32_t* rec = counts + (l * MAX_CHUNKS + c) * 4;
            m_lastStrokes += std::min(rec[0], SBR_MAX_STROKES);
            m_lastDrawn += rec[2];
            m_lastPoints += rec[3];
        }
    }
}

void MetalPipeline::gaussian(const Tex& src, const Tex& dst) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    m_blur.seti("uPass", 0);
    bindTex(UNIT_A, src);
    bindImage(0, m_tmpTex);
    dispatch(m_blur, gx, gy);

    m_blur.seti("uPass", 1);
    bindTex(UNIT_A, m_tmpTex);
    bindImage(0, dst);
    dispatch(m_blur, gx, gy);
}

void MetalPipeline::buildReference(TuningParams& p, float radius) {
    p.blurSigma = std::max(0.1f, radius * 0.5f);
    uploadParams(p);
    gaussian(m_srcTex, m_refTex);
}

void MetalPipeline::buildGradients(const TuningParams& p) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    bindTex(UNIT_A, m_refTex);

    if (p.tensorSigma <= 0.f) {
        m_features.seti("uPass", 0);
        bindImage(0, m_gradTex);
        dispatch(m_features, gx, gy);
        return;
    }

    m_features.seti("uPass", 1);
    bindImage(0, m_gradTex);
    dispatch(m_features, gx, gy);

    TuningParams q = p;
    q.blurSigma = p.tensorSigma;
    uploadParams(q);
    gaussian(m_gradTex, m_tensorTex);
    uploadParams(p);

    m_features.seti("uPass", 2);
    bindTex(UNIT_A, m_refTex);
    bindTex(UNIT_B, m_tensorTex);
    bindImage(0, m_gradTex);
    dispatch(m_features, gx, gy);
}

void MetalPipeline::applyEtf(const TuningParams& p, const RenderConfig& cfg) {
    if (cfg.etfIterations <= 0 || p.etfRadius < 1.f) return;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    bindBuf(BIND_ETF_MAX, m_etfMaxBuf);
    clearBuf(m_etfMaxBuf, 4);

    m_etf.seti("uPass", 0);
    bindTex(UNIT_A, m_gradTex);
    dispatch(m_etf, gx, gy);

    m_etf.seti("uPass", 1);
    Tex src = m_gradTex, dst = m_etfTex;
    for (int i = 0; i < cfg.etfIterations; ++i) {
        bindTex(UNIT_A, src);
        bindImage(0, dst);
        dispatch(m_etf, gx, gy);
        std::swap(src, dst);
    }
    if (src.t != m_gradTex.t) copyTex(m_etfTex, m_gradTex);
}

void MetalPipeline::computeFlow(const TuningParams& p, const RenderConfig& cfg) {
    (void)p;
    m_flowValid = false;
    if (cfg.flowLevels <= 0 || !m_flowTex || !m_havePrev) return;

    constexpr int kFlowLevel = 2;
    const int coarsest = kFlowLevel + cfg.flowLevels - 1;
    const uint32_t fx = divUp(uint32_t(m_flowW), 16), fy = divUp(uint32_t(m_flowH), 16);
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    m_flow.set2i("uFlowSize", m_flowW, m_flowH);

    m_flow.seti("uPass", 0);
    bindTex(UNIT_D, m_prevSrcTex);
    bindImage(1, m_lumaPrevTex);
    dispatch(m_flow, gx, gy);
    bindTex(UNIT_D, m_srcTex);
    bindImage(1, m_lumaCurTex);
    dispatch(m_flow, gx, gy);
    genMips(m_lumaPrevTex);
    genMips(m_lumaCurTex);

    fillTex(m_flowTex, 0.f, 0.f);

    bindTex(UNIT_A, m_lumaPrevTex);
    bindTex(UNIT_B, m_lumaCurTex);
    m_flow.seti("uWindow", 3);

    Tex src = m_flowTex, dst = m_flowTmpTex;
    auto pass = [&](int which, int level) {
        m_flow.seti("uPass", which);
        m_flow.seti("uLevel", level);
        bindTex(UNIT_C, src);
        bindImage(0, dst);
        dispatch(m_flow, fx, fy);
        std::swap(src, dst);
    };
    for (int level = coarsest; level >= kFlowLevel; --level) {
        for (int it = 0; it < std::max(1, cfg.flowIterations); ++it) pass(1, level);
        pass(2, level);
    }
    if (src.t != m_flowTex.t) copyTex(m_flowTmpTex, m_flowTex);
    m_flowValid = true;
}

void MetalPipeline::layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                                  const std::vector<float>& radii, bool temporal,
                                  bool allowCache) {
    const uint32_t gxi = divUp(uint32_t(m_w), 16), gyi = divUp(uint32_t(m_h), 16);

    if (allowCache && m_underValid) {
        copyTex(m_underTex, m_canvasTex);
        fillTex(m_heightTex, 0.f);
        return;
    }

    if (temporal) {
        bindTex(UNIT_E, m_flowTex);
        if (m_flowValid) {
            m_canvasProg.seti("uPass", 3);
            bindTex(UNIT_F, m_prevCanvasTex);
            bindImage(0, m_canvasTex);
            dispatch(m_canvasProg, gxi, gyi);
        } else {
            copyTex(m_prevCanvasTex, m_canvasTex);
        }
        m_canvasProg.seti("uPass", 2);
        m_canvasProg.seti("uUseFlow", m_flowValid ? 1 : 0);
        bindTex(UNIT_B, m_srcTex);
        bindTex(UNIT_C, m_prevSrcTex);
        bindImage(1, m_diffTex);
        dispatch(m_canvasProg, gxi, gyi);
    } else if (cfg.underpaint == Underpaint::Blur) {
        buildReference(p, radii[0]);
        m_canvasProg.seti("uPass", 0);
        bindTex(UNIT_A, m_refTex);
        bindImage(0, m_canvasTex);
        dispatch(m_canvasProg, gxi, gyi);
    } else {
        float fill[3] = {1.f, 1.f, 1.f};
        if (cfg.underpaint == Underpaint::Average) {
            // The 1x1 mip is the image mean. Reading it stalls, as the GL
            // glGetTextureImage did.
            const NSUInteger level = m_srcTex.t.mipmapLevelCount - 1;
            id<MTLBuffer> px = makeBuf(16);
            [blitEncoder() copyFromTexture:m_srcTex.t sourceSlice:0 sourceLevel:level
                              sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                                  toBuffer:px destinationOffset:0 destinationBytesPerRow:4
                     destinationBytesPerImage:4];
            flush(true);
            const uint8_t* c = static_cast<const uint8_t*>(px.contents);
            for (int i = 0; i < 3; ++i) fill[i] = float(c[i]) / 255.f;
        }
        m_canvasProg.seti("uPass", 1);
        m_canvasProg.set3f("uFill", fill[0], fill[1], fill[2]);
        bindImage(0, m_canvasTex);
        dispatch(m_canvasProg, gxi, gyi);
    }

    copyTex(m_canvasTex, m_underTex);
    m_underValid = true;
    fillTex(m_heightTex, 0.f);
}

void MetalPipeline::drawStrokes(id<MTLBuffer> indirect) {
    endEncoder();
    if (!m_cb) m_cb = [m_queue commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = m_canvasTex.t;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].texture = m_heightTex.t;
    rp.colorAttachments[1].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> re = [m_cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:m_stroke];
    [re setViewport:(MTLViewport){0, 0, double(m_w), double(m_h), 0, 1}];
    [re setVertexBytes:&m_p length:sizeof(TuningParams) atIndex:SLOT_PARAMS];
    // stroke.vert reads whatever is bound at BIND_VERTS / BIND_HEADERS, so the
    // pool repaint only has to rebind those, as in pipeline.cpp.
    [re setVertexBuffer:m_ssbo[BIND_VERTS] offset:0 atIndex:SLOT_SSBO_BASE + BIND_VERTS];
    [re setVertexBuffer:m_ssbo[BIND_HEADERS] offset:0 atIndex:SLOT_SSBO_BASE + BIND_HEADERS];
    [re setFragmentBytes:&m_p length:sizeof(TuningParams) atIndex:SLOT_PARAMS];
    [re setFragmentTexture:m_brushTex.t atIndex:UNIT_BRUSH];
    [re setFragmentSamplerState:m_brushTex.s atIndex:UNIT_BRUSH];
    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip indirectBuffer:indirect indirectBufferOffset:0];
    [re endEncoding];
}

void MetalPipeline::repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                                    const std::vector<float>& radii) {
    TuningParams q = p;
    layUnderpaint(q, cfg, radii, /*temporal=*/false, /*allowCache=*/true);
    uploadParams(p);

    m_pool.seti("uPass", 1);
    m_pool.setu("uPoolCapacity", SBR_POOL_MAX);
    dispatch(m_pool, 1, 1);

    bindBuf(BIND_VERTS, m_poolVertexBuf);
    bindBuf(BIND_HEADERS, m_poolHeaderBuf);
    drawStrokes(m_indirectBuf);
    bindBuf(BIND_VERTS, m_vertexBuf);
    bindBuf(BIND_HEADERS, m_headerBuf);
}

void MetalPipeline::relax(TuningParams& p, const RenderConfig& cfg,
                          const std::vector<float>& radii) {
    flush(true);   // the pool count is a GPU-side counter
    const uint32_t* counters = static_cast<const uint32_t*>(m_poolCounterBuf.contents);
    m_poolCount = counters[0];
    if (m_poolCount == 0) return;
    if (m_poolCount > SBR_POOL_MAX) {
        fprintf(stderr,
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
            m_relax.setu("uPoolCapacity", SBR_POOL_MAX);
            m_relax.setu("uSubPass", sub);
            m_relax.setu("uSubPassCount", subs);
            bindTex(UNIT_A, m_srcTex);
            bindTex(UNIT_B, m_underTex);
            dispatch(m_relax, divUp(m_poolCount, 64), 1);
        }
        repaintFromPool(p, cfg, radii);
    }
}

void MetalPipeline::render(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
    if (!m_w || !m_h) return;

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

    m_gpuMs = 0.0;
    m_poolCount = 0;
    m_underValid = false;
    m_layerCount = radii.size();
    clearBuf(m_statsBuf, m_statsBuf.length);
    uploadParams(p);

    if (useTemporal) computeFlow(p, cfg);
    else m_flowValid = false;

    layUnderpaint(p, cfg, radii, useTemporal);

    bindBuf(BIND_SEEDS, m_seedBuf);
    bindBuf(BIND_VERTS, m_vertexBuf);
    bindBuf(BIND_COUNTERS, m_counterBuf);
    bindBuf(BIND_HEADERS, m_headerBuf);
    bindBuf(BIND_INDIRECT, m_indirectBuf);

    const bool wantRelax = cfg.relaxIterations > 0;
    bindBuf(BIND_POOL_VERTS, m_poolVertexBuf);
    bindBuf(BIND_POOL_HEADERS, m_poolHeaderBuf);
    bindBuf(BIND_POOL_COUNTER, m_poolCounterBuf);
    clearBuf(m_poolCounterBuf, 16);

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
        bindBuf(BIND_CELLS, m_cellBuf);

        const uint32_t needed = std::max(1u, divUp(cells, SBR_MAX_STROKES));
        const uint32_t wanted = uint32_t(std::max(1, cfg.passesPerLayer));
        const uint32_t chunks = std::min(MAX_CHUNKS, std::max(needed, wanted));
        p.chunkCount = float(chunks);

        buildReference(p, radius);
        buildGradients(p);
        applyEtf(p, cfg);

        // --- error map + per-cell reduction, once per layer -----------------
        m_error.seti("uPass", 0);
        m_error.seti("uTemporal", useTemporal ? 1 : 0);
        bindTex(UNIT_A, m_refTex);
        bindTex(UNIT_B, m_canvasTex);
        bindImage(0, m_errTex);
        dispatch(m_error, gxi, gyi);

        m_error.seti("uPass", 1);
        bindTex(UNIT_C, m_errTex);
        bindTex(UNIT_D, m_diffTex);
        dispatch(m_error, divUp(gridW, 16), divUp(gridH, 16));

        const uint32_t traceBound = std::min(SBR_MAX_STROKES, divUp(cells, chunks) + 1024u);

        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            p.chunk = float(chunk);
            uploadParams(p);

            clearBuf(m_counterBuf, 16);
            m_seeds.seti("uTemporal", useTemporal ? 1 : 0);
            bindTex(UNIT_A, m_refTex);
            dispatch(m_seeds, divUp(gridW, 8), divUp(gridH, 8));

            bindTex(UNIT_A, m_refTex);
            bindTex(UNIT_B, m_canvasTex);
            bindTex(UNIT_C, m_gradTex);
            dispatch(m_trace, divUp(traceBound, 64), 1);

            // The next pass samples what this one drew; a separate render
            // encoder is Metal's glTextureBarrier.
            drawStrokes(m_indirectBuf);

            if (wantRelax) {
                m_pool.seti("uPass", 0);
                m_pool.setu("uPoolCapacity", SBR_POOL_MAX);
                dispatch(m_pool, divUp(traceBound, 64), 1);
            }

            [blitEncoder() copyFromBuffer:m_counterBuf sourceOffset:0 toBuffer:m_statsBuf
                        destinationOffset:(l * MAX_CHUNKS + chunk) * 16 size:16];
        }
    }

    if (wantRelax) relax(p, cfg, radii);

    if (p.impastoLight > 0.f) {
        uploadParams(p);
        bindTex(UNIT_A, m_heightTex);
        bindImage(0, m_canvasTex);
        dispatch(m_impasto, gxi, gyi);
    }

    copyTex(m_canvasTex, m_prevCanvasTex);
    m_havePrev = true;

    flush(true);
    m_msTotal = m_gpuMs;
    readStats();
}

std::vector<unsigned char> MetalPipeline::readCanvas() {
    std::vector<unsigned char> px(size_t(m_w) * size_t(m_h) * 4);
    if (!m_w) return px;
    convert(m_canvasTex.t, m_readTex.t, false);
    id<MTLBuffer> out = makeBuf(px.size());
    [blitEncoder() copyFromTexture:m_readTex.t sourceSlice:0 sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(NSUInteger(m_w), NSUInteger(m_h), 1)
                          toBuffer:out destinationOffset:0
            destinationBytesPerRow:NSUInteger(m_w) * 4
          destinationBytesPerImage:px.size()];
    flush(true);
    std::memcpy(px.data(), out.contents, px.size());
    return px;
}

id<MTLTexture> MetalPipeline::outputTexture(bool flipY) {
    if (!m_w) return nil;
    convert(m_canvasTex.t, m_outTex.t, flipY);
    flush(true);
    return m_outTex.t;
}
