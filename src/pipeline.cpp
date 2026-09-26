#include "pipeline.h"
#include "brush_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>

namespace {

// SSBO binding points, mirrored in the shaders.
enum : unsigned {
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
enum : unsigned {
    UNIT_A = 0,        // primary input: reference blur, or the blur's source
    UNIT_B = 1,        // canvas, or the tensor field
    UNIT_C = 2,        // gradient field, or the error map
    UNIT_D = 3,        // temporal source-difference map
    UNIT_BRUSH = 4,
    UNIT_E = 5,        // optical flow field
    UNIT_F = 6,        // previous canvas, for flow advection
};

// Upper bound on passes per layer, so a pathological grid cannot spin forever.
constexpr uint32_t MAX_CHUNKS = 32u;

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

} // namespace

bool Pipeline::init() {
    std::string err;
    if (!reloadShaders(&err)) {
        fprintf(stderr, "shader build failed:\n%s\n", err.c_str());
        return false;
    }

    // Stroke buffers hold one chunk's worth of strokes and are never resized,
    // so changing radii or thresholds costs no allocation.
    const size_t seedBytes   = size_t(SBR_MAX_STROKES) * 32;
    const size_t vertexBytes = size_t(SBR_MAX_STROKES) * SBR_MAX_VERTS * 8;
    const size_t headerBytes = size_t(SBR_MAX_STROKES) * 16;

    m_seedBuf     = gpu::createBuffer(seedBytes);
    m_vertexBuf   = gpu::createBuffer(vertexBytes);
    m_headerBuf   = gpu::createBuffer(headerBytes);
    m_counterBuf  = gpu::createBuffer(16);
    m_indirectBuf = gpu::createBuffer(16);

    // Four counters per (layer, pass): seeds, traced, drawn, total points.
    m_statsBuf = gpu::createBuffer(size_t(SBR_MAX_LAYERS) * MAX_CHUNKS * 16,
                                   gpu::BufferUse::Readback);

    // Frame-wide pool for relaxation: every stroke of the frame, not just one
    // pass'. The greedy phase appends to it as it paints.
    m_poolVertexBuf  = gpu::createBuffer(size_t(SBR_POOL_MAX) * SBR_MAX_VERTS * 8);
    m_poolHeaderBuf  = gpu::createBuffer(size_t(SBR_POOL_MAX) * 16);
    m_poolCounterBuf = gpu::createBuffer(16);

    m_etfMaxBuf = gpu::createBuffer(4);
    return true;
}

void Pipeline::shutdown() {
    gpu::Buffer* bufs[] = {&m_cellBuf, &m_seedBuf, &m_vertexBuf, &m_headerBuf,
                           &m_counterBuf, &m_indirectBuf, &m_statsBuf,
                           &m_poolVertexBuf, &m_poolHeaderBuf, &m_poolCounterBuf,
                           &m_energyBuf, &m_etfMaxBuf};
    for (gpu::Buffer* b : bufs) gpu::destroy(*b);
    gpu::Texture* texs[] = {&m_srcTex, &m_refTex, &m_tmpTex, &m_tensorTex, &m_gradTex,
                            &m_errTex, &m_diffTex, &m_canvasTex, &m_heightTex,
                            &m_prevSrcTex, &m_prevCanvasTex, &m_underTex, &m_brushTex,
                            &m_etfTex, &m_lumaPrevTex, &m_lumaCurTex, &m_flowTex,
                            &m_flowTmpTex};
    for (gpu::Texture* t : texs) gpu::destroy(*t);
    gpu::destroy(m_canvasFb);
    m_blur = m_features = m_error = m_seeds = m_trace = m_stroke = m_impasto =
        m_canvasProg = m_pool = m_relax = m_energy = m_etf = m_flow = gpu::Program();
    m_w = m_h = 0;
    m_cellCapacity = 0;
}

bool Pipeline::reloadShaders(std::string* err) {
    gpu::Program blur, features, error, seeds, trace, stroke, impasto, canvasP;
    gpu::Program poolP, relaxP, energyP, etfP, flowP;

    auto comp = [&](gpu::Program& pr, const char* f) {
        if (!pr.loadCompute(f)) { if (err) *err = pr.lastError; return false; }
        return true;
    };
    if (!comp(blur, "blur.comp"))         return false;
    if (!comp(features, "features.comp")) return false;
    if (!comp(error, "error.comp"))       return false;
    if (!comp(seeds, "seeds.comp"))       return false;
    if (!comp(trace, "trace.comp"))       return false;
    if (!comp(impasto, "impasto.comp"))   return false;
    if (!comp(canvasP, "canvas.comp"))    return false;
    if (!comp(poolP, "pool.comp"))        return false;
    if (!comp(relaxP, "relax.comp"))      return false;
    if (!comp(energyP, "energy.comp"))    return false;
    if (!comp(etfP, "etf.comp"))          return false;
    if (!comp(flowP, "flow.comp"))        return false;
    if (!stroke.loadRaster("stroke.vert", "stroke.frag")) {
        if (err) *err = stroke.lastError;
        return false;
    }

    // Only swap once every stage compiled, so a typo during hot-reload leaves
    // the previous working pipeline running. Assigning releases the old ones.
    m_blur = blur; m_features = features; m_error = error; m_seeds = seeds;
    m_trace = trace; m_stroke = stroke; m_impasto = impasto; m_canvasProg = canvasP;
    m_pool = poolP; m_relax = relaxP; m_energy = energyP; m_etf = etfP; m_flow = flowP;
    return true;
}

void Pipeline::resize(int w, int h) {
    if (w == m_w && h == m_h) return;
    gpu::Texture* texs[] = {&m_srcTex, &m_refTex, &m_tmpTex, &m_tensorTex, &m_gradTex,
                            &m_errTex, &m_diffTex, &m_canvasTex, &m_heightTex,
                            &m_prevSrcTex, &m_prevCanvasTex, &m_underTex, &m_etfTex,
                            &m_lumaPrevTex, &m_lumaCurTex, &m_flowTex, &m_flowTmpTex};
    for (gpu::Texture* t : texs) gpu::destroy(*t);
    gpu::destroy(m_canvasFb);

    m_w = w; m_h = h;
    // Mips on the source only so the 'average' underpaint can read the 1x1
    // level. No stage samples an intermediate level any more -- the reference
    // image is a real Gaussian now, not a mip.
    m_srcTex        = gpu::createTexture2D(w, h, gpu::Format::RGBA8, /*mips=*/true);
    m_refTex        = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_tmpTex        = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_tensorTex     = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_gradTex       = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_etfTex        = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    // Mipped, because the LK pyramid *is* the mip chain: a box-filtered
    // level is a coarser approximation than the Gaussian a textbook
    // pyramid uses, but it costs one mipmap generation instead of a
    // per-level blur, and the flow is only steering a warp.
    m_lumaPrevTex   = gpu::createTexture2D(w, h, gpu::Format::R16F, /*mips=*/true);
    m_lumaCurTex    = gpu::createTexture2D(w, h, gpu::Format::R16F, /*mips=*/true);
    m_flowW = std::max(1, w / 4);
    m_flowH = std::max(1, h / 4);
    m_flowTex       = gpu::createTexture2D(m_flowW, m_flowH, gpu::Format::RG16F);
    m_flowTmpTex    = gpu::createTexture2D(m_flowW, m_flowH, gpu::Format::RG16F);
    m_errTex        = gpu::createTexture2D(w, h, gpu::Format::R16F);
    m_diffTex       = gpu::createTexture2D(w, h, gpu::Format::R16F);
    // Float canvas, matching the web version's Float32 buffer: 178K strokes
    // composite through this in one frame, and the error map and the stroke
    // termination test both read it back. Measured against RGBA8 the
    // difference is under 0.2/255, so this is for correctness rather than to
    // fix any observed artefact -- it costs nothing measurable either way.
    m_canvasTex     = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_heightTex     = gpu::createTexture2D(w, h, gpu::Format::R16F);
    m_prevSrcTex    = gpu::createTexture2D(w, h, gpu::Format::RGBA8);
    m_prevCanvasTex = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);
    m_underTex      = gpu::createTexture2D(w, h, gpu::Format::RGBA16F);

    m_havePrev = false;
    m_underValid = false;
    m_flowValid = false;

    m_canvasFb = gpu::createFramebuffer({m_canvasTex, m_heightTex});

    // Two floats per 16x16 workgroup. energy.comp needs one (its partial
    // sum); etf.comp's coherence pass needs two (a sum and a count), and the
    // two reductions are never in flight together, so they share the buffer
    // rather than duplicating an allocation that scales with the image.
    gpu::destroy(m_energyBuf);
    const uint32_t groups = divUp(uint32_t(w), 16) * divUp(uint32_t(h), 16);
    m_energyBuf = gpu::createBuffer(size_t(groups) * 8, gpu::BufferUse::Readback);
}

bool Pipeline::setSource(const unsigned char* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    // Keep the outgoing frame's source for the temporal difference test before
    // it is overwritten.
    if (w == m_w && h == m_h && m_srcTex) gpu::copyTexture(m_srcTex, m_prevSrcTex);
    resize(w, h);
    gpu::uploadTexture(m_srcTex, rgba);
    gpu::generateMipmaps(m_srcTex);
    return true;
}

bool Pipeline::setSourceTexture(gpu::Texture texture) {
    const int w = gpu::width(texture), h = gpu::height(texture);
    if (!texture || w <= 0 || h <= 0) return false;
    if (w == m_w && h == m_h && m_srcTex) gpu::copyTexture(m_srcTex, m_prevSrcTex);
    resize(w, h);
    gpu::copyTexture(texture, m_srcTex);
    gpu::generateMipmaps(m_srcTex);
    return true;
}

void Pipeline::buildBrushTiles(const std::vector<float>& radii, float bristleDensity) {
    if (radii == m_brushRadii && bristleDensity == m_brushDensity && m_brushTex) return;

    brush::TileArray t = brush::makeTiles(radii, bristleDensity);
    gpu::destroy(m_brushTex);
    // u wraps -- the tile is seamless along the stroke. v must clamp, or the
    // bristle profile folds over at the stroke's edges.
    m_brushTex = gpu::createTextureArray(brush::TILE_W, brush::TILE_ROWS_MAX, t.layers,
                                         gpu::Format::R16F, t.data.data(),
                                         gpu::Wrap::Repeat, gpu::Wrap::Clamp);

    m_brushRows = t.rows;
    m_brushRadii = radii;
    m_brushDensity = bristleDensity;
}

void Pipeline::ensureCellCapacity(uint32_t cells) {
    if (cells <= m_cellCapacity && m_cellBuf) return;
    gpu::destroy(m_cellBuf);
    m_cellCapacity = cells;
    m_cellBuf = gpu::createBuffer(size_t(cells) * 16);
}

// Sums the per-(layer, pass) counters into the reported totals. Called at the
// top of render() for last frame's numbers, and by refreshStats() once a
// caller has waited for the current frame.
void Pipeline::readStats() {
    uint32_t counts[SBR_MAX_LAYERS * MAX_CHUNKS * 4] = {0};
    gpu::readBuffer(m_statsBuf, 0, sizeof(counts), counts);

    m_lastStrokes = 0; m_lastDrawn = 0; m_lastPoints = 0;
    for (size_t l = 0; l < m_layers.size(); ++l) {
        m_layers[l].strokes = 0;
        m_layers[l].drawn = 0;
        for (uint32_t c = 0; c < MAX_CHUNKS; ++c) {
            const uint32_t* rec = counts + (l * MAX_CHUNKS + c) * 4;
            const uint32_t seeds = std::min(rec[0], SBR_MAX_STROKES);
            m_layers[l].strokes += seeds;
            m_layers[l].drawn += rec[2];
            m_lastStrokes += seeds;
            m_lastDrawn += rec[2];
            m_lastPoints += rec[3];
        }
    }
}

void Pipeline::refreshStats() { readStats(); }

void Pipeline::uploadParams(const TuningParams& p) {
    gpu::setParams(&p, sizeof(TuningParams));
}

// Separable Gaussian: src -> m_tmpTex (horizontal) -> dst (vertical). Sigma
// comes from the uniform block, which the caller has already uploaded.
void Pipeline::gaussian(gpu::Texture srcTex, gpu::Texture dstTex) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    m_blur.setInt("uPass", 0);
    gpu::bindTexture(UNIT_A, srcTex);
    gpu::bindImage(0, m_tmpTex, gpu::Access::Write);
    gpu::dispatch(m_blur, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);

    m_blur.setInt("uPass", 1);
    gpu::bindTexture(UNIT_A, m_tmpTex);
    gpu::bindImage(0, dstTex, gpu::Access::Write);
    gpu::dispatch(m_blur, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);
}

void Pipeline::buildReference(TuningParams& p, float radius) {
    // worker.js: `const sigma = Math.max(0.1, radius * 0.5)`.
    p.blurSigma = std::max(0.1f, radius * 0.5f);
    uploadParams(p);
    gaussian(m_srcTex, m_refTex);
}

void Pipeline::buildGradients(const TuningParams& p) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    gpu::bindTexture(UNIT_A, m_refTex);

    if (p.tensorSigma <= 0.f) {
        m_features.setInt("uPass", 0);
        gpu::bindImage(0, m_gradTex, gpu::Access::Write);
        gpu::dispatch(m_features, gx, gy, 1);
        gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);
        return;
    }

    // Structure-tensor path: raw components -> Gaussian smooth -> eigenvector,
    // which is how computeGradientsST reuses gaussianBlurRGB on a packed RGB
    // buffer. The smoothed field lands back in m_tensorTex.
    m_features.setInt("uPass", 1);
    gpu::bindImage(0, m_gradTex, gpu::Access::Write);
    gpu::dispatch(m_features, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);

    TuningParams q = p;
    q.blurSigma = p.tensorSigma;
    uploadParams(q);
    gaussian(m_gradTex, m_tensorTex);
    uploadParams(p);

    m_features.setInt("uPass", 2);
    gpu::bindTexture(UNIT_B, m_tensorTex);
    gpu::bindImage(0, m_gradTex, gpu::Access::Write);
    gpu::dispatch(m_features, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);
}

// Edge Tangent Flow (Kang, Lee & Chui 2007), applied to the layer's direction
// field in place. See shaders/etf.comp for what the filter is; this is only the
// loop around it.
//
// The iterate ping-pongs between m_gradTex and m_etfTex rather than needing a
// separate copy of the original field, because the filter never touches .z:
// every iterate still carries the untouched gradient magnitude that w_m is
// computed from, so the magnitudes stay the frame's own throughout.
void Pipeline::applyEtf(const TuningParams& p, const RenderConfig& cfg) {
    const bool run = cfg.etfIterations > 0 && p.etfRadius >= 1.f;

    // Measured before the early return as well, so that `--etf 0` still reports
    // the plain field's coherence -- the baseline the ETF figure has to be read
    // against. Without it the number says nothing.
    if (m_logEtf) m_etfLog.push_back({measureFlowCoherence(), -1.0});
    if (!run) return;

    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    m_tEtf.begin();
    gpu::bindStorage(BIND_ETF_MAX, m_etfMaxBuf);

    // w_m needs the field's maximum magnitude, and the field is rebuilt per
    // layer, so the reduction is too. Cleared to 0, which is the identity for
    // a max over non-negative floats and is also 0.0f's bit pattern.
    gpu::clearBuffer(m_etfMaxBuf, 0, 4, 0u);
    gpu::memoryBarrier(gpu::BarrierBufferUpdate | gpu::BarrierStorage);

    m_etf.setInt("uPass", 0);
    gpu::bindTexture(UNIT_A, m_gradTex);
    gpu::dispatch(m_etf, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierStorage);

    m_etf.setInt("uPass", 1);
    gpu::Texture src = m_gradTex, dst = m_etfTex;
    for (int i = 0; i < cfg.etfIterations; ++i) {
        gpu::bindTexture(UNIT_A, src);
        gpu::bindImage(0, dst, gpu::Access::Write);
        gpu::dispatch(m_etf, gx, gy, 1);
        gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);
        std::swap(src, dst);
    }

    // An odd iteration count leaves the result in m_etfTex, and every stage
    // downstream reads m_gradTex by name.
    if (src != m_gradTex) {
        gpu::copyTexture(m_etfTex, m_gradTex);
        gpu::memoryBarrier(gpu::BarrierTextureFetch);
    }
    endStage(m_tEtf, m_ms.etf);

    if (m_logEtf) m_etfLog.back().second = measureFlowCoherence();
}

// Mean |t(x).t(y)| between a pixel's tangent and its eight neighbours', over
// the whole current gradient field. Reads back, so it stalls -- it exists to
// check ETF, not to run beside it.
//
// The floor is 2/pi ~= 0.637, which is what a uniformly random field scores, so
// that is the number to compare against rather than 0.
double Pipeline::measureFlowCoherence() {
    if (!m_w || !m_h || !m_energyBuf || !m_etf.valid()) return 0.0;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    gpu::bindStorage(BIND_ENERGY, m_energyBuf);
    m_etf.setInt("uPass", 2);
    gpu::bindTexture(UNIT_A, m_gradTex);
    gpu::dispatch(m_etf, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierStorage);

    std::vector<float> partial(size_t(gx) * gy * 2);
    gpu::readBuffer(m_energyBuf, 0, partial.size() * 4, partial.data());

    double sum = 0.0, count = 0.0;
    for (size_t i = 0; i < partial.size(); i += 2) {
        sum   += partial[i];
        count += partial[i + 1];
    }
    return count > 0.0 ? sum / count : 0.0;
}

// Dense backward flow from the previous source frame to the current one, by
// pyramidal Lucas-Kanade. See shaders/flow.comp for the method and the sign
// convention; this is the coarse-to-fine schedule around it.
void Pipeline::computeFlow(const TuningParams& p, const RenderConfig& cfg) {
    m_flowValid = false;
    if (cfg.flowLevels <= 0 || !m_flowTex || !m_havePrev) return;
    (void)p;

    // The quarter-resolution field is level 2 of the pyramid, and the LK window
    // is spaced one level pixel apart, so refining below level 2 would measure
    // a window narrower than the gap between flow samples. See flow.comp.
    constexpr int kFlowLevel = 2;
    const int coarsest = kFlowLevel + cfg.flowLevels - 1;

    const uint32_t fx = divUp(uint32_t(m_flowW), 16);
    const uint32_t fy = divUp(uint32_t(m_flowH), 16);
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    m_tFlow.begin();
    m_flow.setIVec2("uFlowSize", m_flowW, m_flowH);

    // Luma pyramids for both frames. Built here rather than cached from the
    // previous frame's work because setSource has already overwritten the
    // source texture by the time the renderer runs, and a second cached
    // pyramid would be one more thing to keep in step with it.
    m_flow.setInt("uPass", 0);
    gpu::bindTexture(UNIT_D, m_prevSrcTex);
    gpu::bindImage(1, m_lumaPrevTex, gpu::Access::Write);
    gpu::dispatch(m_flow, gx, gy, 1);
    gpu::bindTexture(UNIT_D, m_srcTex);
    gpu::bindImage(1, m_lumaCurTex, gpu::Access::Write);
    gpu::dispatch(m_flow, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierImageAccess | gpu::BarrierTextureFetch);
    gpu::generateMipmaps(m_lumaPrevTex);
    gpu::generateMipmaps(m_lumaCurTex);

    // Start from zero displacement. Warm-starting from the previous frame's
    // field would track constant motion in fewer iterations, but it also
    // carries a bad estimate forward indefinitely, and a wrong flow smears the
    // canvas rather than merely failing to help.
    gpu::clearTexture(m_flowTex, 0.f, 0.f, 0.f, 0.f);
    gpu::memoryBarrier(gpu::BarrierTextureUpdate);

    gpu::bindTexture(UNIT_A, m_lumaPrevTex);
    gpu::bindTexture(UNIT_B, m_lumaCurTex);
    m_flow.setInt("uWindow", 3);   // 7x7, in level pixels

    gpu::Texture src = m_flowTex, dst = m_flowTmpTex;
    auto pass = [&](int which, int level) {
        m_flow.setInt("uPass", which);
        m_flow.setInt("uLevel", level);
        gpu::bindTexture(UNIT_C, src);
        gpu::bindImage(0, dst, gpu::Access::Write);
        gpu::dispatch(m_flow, fx, fy, 1);
        gpu::memoryBarrier(gpu::BarrierImageAccess | gpu::BarrierTextureFetch);
        std::swap(src, dst);
    };

    for (int level = coarsest; level >= kFlowLevel; --level) {
        for (int it = 0; it < std::max(1, cfg.flowIterations); ++it) pass(1, level);
        // One smoothing pass per level, not one at the end: the next, finer
        // level refines whatever this one produced, so a speckled field here
        // is a bad starting point there rather than a cosmetic problem.
        pass(2, level);
    }

    // Every consumer reads m_flowTex by name.
    if (src != m_flowTex) {
        gpu::copyTexture(m_flowTmpTex, m_flowTex);
        gpu::memoryBarrier(gpu::BarrierTextureFetch);
    }
    endStage(m_tFlow, m_ms.flow);
    m_flowValid = true;

    if (m_logFlow) {
        // Reads the field back, so it stalls. The mean and max magnitude are
        // the first thing to look at when advection is not helping: a field of
        // zeros means the stage never ran or the windows were all rejected,
        // and a huge max means a bad window escaped the clamp.
        std::vector<float> f(size_t(m_flowW) * m_flowH * 2);
        gpu::readTexture(m_flowTex, 0, gpu::ReadAs::RG32F, f.data());
        double sum = 0.0, mx = 0.0, sx = 0.0, sy = 0.0;
        const size_t n = f.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            const double m = std::sqrt(double(f[i * 2]) * f[i * 2] +
                                       double(f[i * 2 + 1]) * f[i * 2 + 1]);
            sum += m;
            mx = std::max(mx, m);
            sx += f[i * 2];
            sy += f[i * 2 + 1];
        }
        printf("  flow: mean |d| %.3f px, max %.3f, mean d = (%.3f, %.3f)\n",
               sum / double(n), mx, sx / double(n), sy / double(n));
    }
}

// Lays down the starting canvas: worker.js `applyUnderpaint`, or the previous
// frame's paint in temporal mode. Factored out because relaxation repaints the
// canvas from the stroke pool and needs the same ground underneath.
void Pipeline::layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                             const std::vector<float>& radii, bool temporal,
                             bool allowCache) {
    const uint32_t gxi = divUp(uint32_t(m_w), 16), gyi = divUp(uint32_t(m_h), 16);

    if (allowCache && m_underValid) {
        gpu::copyTexture(m_underTex, m_canvasTex);
        gpu::clearTexture(m_heightTex, 0.f, 0.f, 0.f, 0.f);
        return;
    }

    if (temporal) {
        // Start from the previous frame's paint and repaint only what moved.
        gpu::bindTexture(UNIT_E, m_flowTex);

        if (m_flowValid) {
            // Advect the paint along the flow first, so "what moved" is asked
            // of the subject rather than of the pixel grid.
            m_canvasProg.setInt("uPass", 3);
            gpu::bindTexture(UNIT_F, m_prevCanvasTex);
            gpu::bindImage(0, m_canvasTex, gpu::Access::Write);
            gpu::dispatch(m_canvasProg, gxi, gyi, 1);
            gpu::memoryBarrier(gpu::BarrierImageAccess | gpu::BarrierTextureFetch);
        } else {
            gpu::copyTexture(m_prevCanvasTex, m_canvasTex);
        }

        m_canvasProg.setInt("uPass", 2);
        m_canvasProg.setInt("uUseFlow", m_flowValid ? 1 : 0);
        gpu::bindTexture(UNIT_B, m_srcTex);
        gpu::bindTexture(UNIT_C, m_prevSrcTex);
        gpu::bindImage(1, m_diffTex, gpu::Access::Write);
        gpu::dispatch(m_canvasProg, gxi, gyi, 1);

        if (m_logFlow) {
            // What the cells are actually gated on. seeds.comp skips a cell
            // when the *maximum* difference inside it is below the threshold,
            // so the mean alone does not say whether anything will be skipped
            // -- hence the percentiles.
            gpu::memoryBarrier(gpu::BarrierImageAccess | gpu::BarrierTextureUpdate);
            std::vector<float> d(size_t(m_w) * m_h);
            gpu::readTexture(m_diffTex, 0, gpu::ReadAs::R32F, d.data());
            double sum = 0.0;
            for (float v : d) sum += v;
            std::vector<float> s = d;
            auto pct = [&](double q) {
                size_t k = size_t(q * double(s.size() - 1));
                std::nth_element(s.begin(), s.begin() + k, s.end());
                return s[k];
            };
            const float p99 = pct(0.99), p999 = pct(0.999);
            printf("  diff: mean %.2f  p99 %.2f  p99.9 %.2f  (threshold %.0f)\n",
                   sum / double(d.size()), p99, p999, p.frameDiffThreshold);
        }
    } else if (cfg.underpaint == Underpaint::Blur) {
        // The coarsest radius' Gaussian, per applyUnderpaint('blur'). The old
        // renderer blitted the source 1:1 here, which is why the sharp
        // photograph showed through every gap between strokes.
        buildReference(p, radii[0]);
        m_canvasProg.setInt("uPass", 0);
        gpu::bindTexture(UNIT_A, m_refTex);
        gpu::bindImage(0, m_canvasTex, gpu::Access::Write);
        gpu::dispatch(m_canvasProg, gxi, gyi, 1);
    } else {
        float fill[4] = {1.f, 1.f, 1.f, 1.f};
        if (cfg.underpaint == Underpaint::Average) {
            // The 1x1 mip is the image mean -- the quantity the web version
            // accumulates in a loop.
            const int levels = 1 + int(std::floor(std::log2(float(std::max(m_w, m_h)))));
            gpu::readTexture(m_srcTex, levels - 1, gpu::ReadAs::RGBA32F, fill);
        }
        m_canvasProg.setInt("uPass", 1);
        m_canvasProg.setVec3("uFill", fill[0], fill[1], fill[2]);
        gpu::bindImage(0, m_canvasTex, gpu::Access::Write);
        gpu::dispatch(m_canvasProg, gxi, gyi, 1);
    }
    gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);

    // Keep the ground so a relaxation repaint can copy it back instead of
    // recomputing the blur.
    gpu::copyTexture(m_canvasTex, m_underTex);
    m_underValid = true;

    gpu::clearTexture(m_heightTex, 0.f, 0.f, 0.f, 0.f);
}

// Repaints the canvas from the stroke pool: underpaint, then every pooled
// stroke in one instanced draw. Relaxation moves strokes that were already
// composited, so the only way to see the result is to paint it again from
// scratch -- there is no incremental undo of a blended stroke.
void Pipeline::repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                               const std::vector<float>& radii) {
    TuningParams q = p;
    layUnderpaint(q, cfg, radii, /*temporal=*/false, /*allowCache=*/true);
    uploadParams(p);

    // The draw command comes from the pool count, written GPU-side.
    m_pool.setInt("uPass", 1);
    m_pool.setUInt("uPoolCapacity", SBR_POOL_MAX);
    gpu::dispatch(m_pool, 1, 1, 1);
    gpu::memoryBarrier(gpu::BarrierStorage | gpu::BarrierCommand);

    // stroke.vert reads whatever is bound at BIND_VERTS / BIND_HEADERS, so
    // pointing those at the pool is the whole change.
    gpu::bindStorage(BIND_VERTS, m_poolVertexBuf);
    gpu::bindStorage(BIND_HEADERS, m_poolHeaderBuf);

    gpu::bindTexture(UNIT_BRUSH, m_brushTex);
    gpu::drawIndirect(m_stroke, m_canvasFb,
                      {gpu::Blend::PremultipliedOver, gpu::Blend::Additive},
                      m_indirectBuf);
    gpu::textureBarrier();

    gpu::bindStorage(BIND_VERTS, m_vertexBuf);
    gpu::bindStorage(BIND_HEADERS, m_headerBuf);
}

double Pipeline::measureEnergy() {
    if (!m_w || !m_h || !m_energyBuf) return 0.0;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    gpu::bindStorage(BIND_ENERGY, m_energyBuf);
    gpu::bindTexture(UNIT_A, m_srcTex);
    gpu::bindTexture(UNIT_B, m_canvasTex);
    gpu::dispatch(m_energy, gx, gy, 1);
    gpu::memoryBarrier(gpu::BarrierStorage);

    std::vector<float> partial(size_t(gx) * gy);
    gpu::readBuffer(m_energyBuf, 0, partial.size() * 4, partial.data());

    double sum = 0.0;
    for (float v : partial) sum += v;
    return sum / (double(m_w) * double(m_h));   // mean Lab error per pixel
}

// Hertzmann 2001. The greedy phase has placed the strokes; now improve them.
void Pipeline::relax(TuningParams& p, const RenderConfig& cfg,
                     const std::vector<float>& radii) {
    uint32_t counters[4] = {0, 0, 0, 0};
    gpu::readBuffer(m_poolCounterBuf, 0, sizeof(counters), counters);
    m_poolCount = counters[0];

    if (m_poolCount == 0) return;
    if (m_poolCount > SBR_POOL_MAX) {
        // pool.comp dropped the overflow, so a repaint from the pool would
        // lose strokes the greedy phase did paint. Leaving the greedy canvas
        // alone is the honest outcome; say so rather than silently degrading.
        fprintf(stderr,
                "gpu-sbr: %u strokes exceeds the relaxation pool (%u); skipping"
                " relaxation for this frame. Raise SBR_POOL_MAX or the"
                " threshold, or use fewer/larger brush radii.\n",
                m_poolCount, SBR_POOL_MAX);
        m_poolCount = SBR_POOL_MAX;
        return;
    }

    m_tRelax.begin();
    const uint32_t subs = uint32_t(std::max(1, cfg.relaxSubPasses));

    if (m_logRelax) m_relaxLog.push_back(measureEnergy());

    for (int iter = 0; iter < cfg.relaxIterations; ++iter) {
        p.relaxIter = float(iter);
        uploadParams(p);

        for (uint32_t sub = 0; sub < subs; ++sub) {
            m_relax.setUInt("uPoolCapacity", SBR_POOL_MAX);
            m_relax.setUInt("uSubPass", sub);
            m_relax.setUInt("uSubPassCount", subs);
            gpu::bindTexture(UNIT_A, m_srcTex);
            // The frame's ground, not the canvas -- see the long comment in
            // relax.comp about why measuring "what is underneath" against the
            // canvas that already contains the stroke is circular.
            gpu::bindTexture(UNIT_B, m_underTex);
            gpu::dispatch(m_relax, divUp(m_poolCount, 64), 1, 1);
            gpu::memoryBarrier(gpu::BarrierStorage);
        }

        // One repaint per iteration, not per sub-pass. The kernel reads only
        // the source and the frame's ground, neither of which relaxation
        // changes, so sub-passes no longer have to see each other's paint --
        // they did when the score was measured against the live canvas.
        repaintFromPool(p, cfg, radii);
        if (m_logRelax) m_relaxLog.push_back(measureEnergy());
    }

    gpu::readBuffer(m_poolCounterBuf, 0, sizeof(counters), counters);
    m_relaxRemoved = counters[2];
    m_relaxAccepted = counters[3];

    if (m_logRelax) {
        // What relaxation actually did to the geometry. Read from the pool,
        // because the trace counters predate it.
        struct Hdr { uint32_t vertexCount, layerTex, color; float totalLen; };
        std::vector<Hdr> hdr(m_poolCount);
        gpu::readBuffer(m_poolHeaderBuf, 0, hdr.size() * sizeof(Hdr), hdr.data());
        std::vector<uint32_t> v0(size_t(m_poolCount) * 2);
        // The first vertex of each stroke carries its radius; read the whole
        // slot stride and stride over it rather than pulling every point.
        std::vector<uint32_t> verts(size_t(m_poolCount) * SBR_MAX_VERTS * 2);
        gpu::readBuffer(m_poolVertexBuf, 0, verts.size() * 4, verts.data());

        auto half = [](uint16_t hb) {
            const uint32_t sg = (hb >> 15) & 1u, e = (hb >> 10) & 0x1fu, m = hb & 0x3ffu;
            float v;
            if (e == 0u)       v = std::ldexp(float(m), -24);
            else if (e == 31u) v = 0.f;
            else               v = std::ldexp(float(m + 1024u), int(e) - 25);
            return sg ? -v : v;
        };

        uint64_t pts = 0;
        double radSum = 0.0, areaSum = 0.0;
        m_relaxLive = 0;
        for (uint32_t i = 0; i < m_poolCount; ++i) {
            if (hdr[i].vertexCount < 2u) continue;
            ++m_relaxLive;
            pts += hdr[i].vertexCount;
            const uint32_t packed = verts[size_t(i) * SBR_MAX_VERTS * 2 + 1];
            const double r = half(uint16_t(packed & 0xffffu));
            radSum += r;
            // Same swept-capsule area relax.comp scores with.
            areaSum += 2.0 * r * hdr[i].totalLen + 3.14159265 * r * r;
        }
        m_relaxMeanPts = m_relaxLive ? double(pts) / m_relaxLive : 0.0;
        m_relaxMeanRadius = m_relaxLive ? radSum / m_relaxLive : 0.0;
        m_relaxTotalArea = areaSum;
    }

    endStage(m_tRelax, m_ms.relax);
}

void Pipeline::render(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
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

    // Last frame's exact stroke counts, collected before this frame's chunks
    // start overwriting the stats buffer.
    m_ms = StageTimes{};
    m_poolCount = 0; m_relaxAccepted = 0; m_relaxRemoved = 0;
    m_underValid = false;              // this frame's ground is not built yet
    if (m_logRelax) m_relaxLog.clear(); // report the current frame, not a run
    if (m_logEtf) m_etfLog.clear();
    m_layers.assign(radii.size(), LayerStats());
    readStats();
    // A fresh frame starts with the slots it is about to fill zeroed, so a
    // layer that now needs fewer chunks does not report stale counts.
    const uint32_t zeroStats = 0;
    gpu::clearBuffer(m_statsBuf, 0, 0, zeroStats);

    uploadParams(p);

    // --- canvas setup ----------------------------------------------------
    // The flow has to exist before the underpaint, which is what consumes it.
    // It is only ever used by the temporal path, so there is no reason to pay
    // for it on a still.
    if (useTemporal) computeFlow(p, cfg);
    else m_flowValid = false;

    m_tRef.begin();
    layUnderpaint(p, cfg, radii, useTemporal);
    endStage(m_tRef, m_ms.ref);

    gpu::bindStorage(BIND_SEEDS, m_seedBuf);
    gpu::bindStorage(BIND_VERTS, m_vertexBuf);
    gpu::bindStorage(BIND_COUNTERS, m_counterBuf);
    gpu::bindStorage(BIND_HEADERS, m_headerBuf);
    gpu::bindStorage(BIND_INDIRECT, m_indirectBuf);

    // The frame-wide pool. trace.comp reserves a slot range per pass and
    // pool.comp copies into it, so that after the greedy phase relaxation has
    // every stroke of the frame to work with.
    const bool wantRelax = cfg.relaxIterations > 0;
    gpu::bindStorage(BIND_POOL_VERTS, m_poolVertexBuf);
    gpu::bindStorage(BIND_POOL_HEADERS, m_poolHeaderBuf);
    gpu::bindStorage(BIND_POOL_COUNTER, m_poolCounterBuf);
    gpu::clearBuffer(m_poolCounterBuf, 0, 16, 0u);
    gpu::memoryBarrier(gpu::BarrierBufferUpdate | gpu::BarrierStorage);

    // --- per layer, coarse to fine ---------------------------------------
    for (size_t l = 0; l < radii.size(); ++l) {
        const float radius = radii[l];
        p.currentLayer = float(l);
        p.layerRadius = radius;
        p.firstLayer = (l == 0) ? 1.f : 0.f;
        // worker.js: `grid = Math.max(1, Math.round(radius * gridFactor))`.
        const int grid = std::max(1, int(std::lround(radius * p.gridFactor)));
        p.gridSpacing = float(grid);
        p.texRows = float(l < m_brushRows.size() ? m_brushRows[l] : 16);
        p.texLayerBase = float(l * size_t(brush::VARIANTS));
        p.texUScale = float(brush::TILE_W) / std::max(16.f, radius * 4.f);

        const uint32_t gridW = divUp(uint32_t(m_w), uint32_t(grid));
        const uint32_t gridH = divUp(uint32_t(m_h), uint32_t(grid));
        const uint32_t cells = gridW * gridH;
        ensureCellCapacity(cells);
        gpu::bindStorage(BIND_CELLS, m_cellBuf);

        // Passes over this layer's cells. Two things set the count: the
        // stroke buffers must hold one pass' worth, and the intra-layer
        // feedback that regulates stroke length needs more than one pass to
        // exist at all (see RenderConfig::passesPerLayer). Cells are assigned
        // to passes by hash, so each pass is scattered over the whole image
        // rather than covering a band -- worker.js shuffles its cell list for
        // the same reason, to decorrelate overlap order from position.
        const uint32_t needed = std::max(1u, divUp(cells, SBR_MAX_STROKES));
        const uint32_t wanted = uint32_t(std::max(1, cfg.passesPerLayer));
        const uint32_t chunks = std::min(MAX_CHUNKS, std::max(needed, wanted));
        p.chunkCount = float(chunks);

        m_layers[l].radius = radius;
        m_layers[l].grid = float(grid);
        m_layers[l].cells = cells;
        m_layers[l].chunks = chunks;

        // Reference blur and gradient field at this layer's scale. Doing this
        // per layer is what makes a coarse stroke follow coarse structure; the
        // old renderer built one field from the full-resolution source and
        // reused it for every layer.
        m_tRef.begin();
        buildReference(p, radius);
        buildGradients(p);
        endStage(m_tRef, m_ms.ref);

        // ETF refines the field this layer just built, so it runs per layer and
        // at the layer's own scale -- which is the same reason the gradients
        // are rebuilt per layer rather than shared. Its own timer, because it
        // is optional and its cost should not hide inside `reference`.
        applyEtf(p, cfg);

        // --- error map + per-cell reduction ------------------------------
        // Once per layer, against the canvas as it stands before this layer
        // paints. worker.js does the same: it builds `err` before the cell
        // loop and lets the loop mutate the canvas underneath it. Keeping it
        // out of the chunk loop is therefore the faithful choice as well as
        // the cheap one -- each cell is still tested exactly once, because the
        // chunks partition the cell grid.
        m_tError.begin();
        m_error.setInt("uPass", 0);
        m_error.setInt("uTemporal", useTemporal ? 1 : 0);
        gpu::bindTexture(UNIT_A, m_refTex);
        gpu::bindTexture(UNIT_B, m_canvasTex);
        gpu::bindImage(0, m_errTex, gpu::Access::Write);
        gpu::dispatch(m_error, gxi, gyi, 1);
        gpu::memoryBarrier(gpu::BarrierTextureFetch | gpu::BarrierImageAccess);

        m_error.setInt("uPass", 1);
        gpu::bindTexture(UNIT_C, m_errTex);
        gpu::bindTexture(UNIT_D, m_diffTex);
        gpu::dispatch(m_error, divUp(gridW, 16), divUp(gridH, 16), 1);
        gpu::memoryBarrier(gpu::BarrierStorage);
        endStage(m_tError, m_ms.error);

        if (m_debugCells) {
            struct CellRec { float meanErr; uint32_t best; float maxDiff; float pad; };
            std::vector<CellRec> rec(cells);
            gpu::readBuffer(m_cellBuf, 0, cells * sizeof(CellRec), rec.data());
            double sum = 0.0;
            float lo = 1e9f, hi = -1e9f;
            uint32_t pass = 0;
            std::vector<uint32_t> hist(10, 0);
            for (const CellRec& c : rec) {
                sum += c.meanErr;
                lo = std::min(lo, c.meanErr);
                hi = std::max(hi, c.meanErr);
                if (c.meanErr > p.threshold) ++pass;
                hist[std::min<size_t>(9, size_t(c.meanErr / 20.f))]++;
            }
            printf("  layer %zu r %.1f grid %d cells %u: meanErr min %.1f mean %.1f max %.1f"
                   " | > threshold %.0f: %u (%.1f%%)\n",
                   l, radius, grid, cells, lo, sum / std::max<uint32_t>(cells, 1), hi,
                   p.threshold, pass, 100.0 * pass / std::max<uint32_t>(cells, 1));
            printf("    histogram by meanErr/20:");
            for (uint32_t bin : hist) printf(" %u", bin);
            printf("\n");
        }

        // Threads past seedCount return immediately, but there is no reason
        // to launch a full buffer's worth for a pass that can only have
        // produced cells/chunks seeds.
        const uint32_t traceBound =
            std::min(SBR_MAX_STROKES, divUp(cells, chunks) + 1024u);

        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            p.chunk = float(chunk);
            uploadParams(p);

            // --- seeds ---------------------------------------------------
            m_tSeeds.begin();
            gpu::clearBuffer(m_counterBuf, 0, 16, 0u);
            gpu::memoryBarrier(gpu::BarrierBufferUpdate | gpu::BarrierStorage);

            m_seeds.setInt("uTemporal", useTemporal ? 1 : 0);
            gpu::bindTexture(UNIT_A, m_refTex);
            gpu::dispatch(m_seeds, divUp(gridW, 8), divUp(gridH, 8), 1);
            gpu::memoryBarrier(gpu::BarrierStorage);
            endStage(m_tSeeds, m_ms.seeds);

            // --- trace ---------------------------------------------------
            m_tTrace.begin();
            gpu::bindTexture(UNIT_A, m_refTex);
            gpu::bindTexture(UNIT_B, m_canvasTex);
            gpu::bindTexture(UNIT_C, m_gradTex);
            gpu::dispatch(m_trace, divUp(traceBound, 64), 1, 1);
            gpu::memoryBarrier(gpu::BarrierStorage | gpu::BarrierCommand);
            endStage(m_tTrace, m_ms.trace);

            // --- rasterise -----------------------------------------------
            m_tRaster.begin();
            // Colour: premultiplied source-over, once per stroke. Height:
            // plain accumulation, matching `heightBuf[i] += mv * impasto`.
            gpu::bindTexture(UNIT_BRUSH, m_brushTex);
            gpu::drawIndirect(m_stroke, m_canvasFb,
                              {gpu::Blend::PremultipliedOver, gpu::Blend::Additive},
                              m_indirectBuf);
            gpu::textureBarrier();   // the next pass samples what we just drew
            endStage(m_tRaster, m_ms.raster);

            // --- append this pass to the frame pool ----------------------
            if (wantRelax) {
                m_pool.setInt("uPass", 0);
                m_pool.setUInt("uPoolCapacity", SBR_POOL_MAX);
                gpu::dispatch(m_pool, divUp(traceBound, 64), 1, 1);
                gpu::memoryBarrier(gpu::BarrierStorage);
            }

            // Stroke count for the overlay, copied GPU-side. Reading the
            // counter here instead would pull a buffer from video to host
            // memory mid-frame and serialise the whole pipeline.
            gpu::copyBuffer(m_counterBuf, 0, m_statsBuf,
                            size_t(l * MAX_CHUNKS + chunk) * 16, 16);
        }
    }

    // --- relaxation -------------------------------------------------------
    if (wantRelax) relax(p, cfg, radii);

    // --- impasto lighting -------------------------------------------------
    m_tImpasto.begin();
    if (p.impastoLight > 0.f) {
        gpu::bindTexture(UNIT_A, m_heightTex);
        gpu::bindImage(0, m_canvasTex, gpu::Access::ReadWrite);
        gpu::dispatch(m_impasto, gxi, gyi, 1);
        gpu::memoryBarrier(gpu::BarrierImageAccess | gpu::BarrierTextureFetch);
    }
    endStage(m_tImpasto, m_ms.impasto);

    // Keep this frame's canvas for the next temporal frame.
    gpu::copyTexture(m_canvasTex, m_prevCanvasTex);
    m_havePrev = true;
}

std::vector<unsigned char> Pipeline::readCanvas() const {
    std::vector<unsigned char> px(size_t(m_w) * size_t(m_h) * 4);
    gpu::readTexture(m_canvasTex, 0, gpu::ReadAs::RGBA8, px.data());
    return px;
}

void Pipeline::dumpStrokes(int n) const {
    if (n <= 0) return;

    // Half-float decode: the vertex buffer is packHalf2x16 and there is no
    // host-side equivalent in the standard library before C++23.
    auto h2f = [](uint16_t hb) {
        const uint32_t s = (hb >> 15) & 1u, e = (hb >> 10) & 0x1fu, m = hb & 0x3ffu;
        float v;
        if (e == 0u)       v = std::ldexp(float(m), -24);
        else if (e == 31u) v = float(m ? NAN : INFINITY);
        else               v = std::ldexp(float(m + 1024u), int(e) - 25);
        return s ? -v : v;
    };
    auto unhalf = [&](uint32_t packed) {
        return std::pair<float, float>{h2f(uint16_t(packed & 0xffffu)),
                                       h2f(uint16_t(packed >> 16))};
    };

    std::vector<uint32_t> hdr(size_t(n) * 4);
    gpu::readBuffer(m_headerBuf, 0, hdr.size() * 4, hdr.data());
    std::vector<uint32_t> vtx(size_t(n) * SBR_MAX_VERTS * 2);
    gpu::readBuffer(m_vertexBuf, 0, vtx.size() * 4, vtx.data());

    for (int i = 0; i < n; ++i) {
        const uint32_t count = hdr[size_t(i) * 4 + 0];
        float totalLen;
        std::memcpy(&totalLen, &hdr[size_t(i) * 4 + 3], 4);
        printf("stroke %d: %u pts, %.1f px long\n", i, count, totalLen);
        for (uint32_t k = 0; k < count && k < SBR_MAX_VERTS; ++k) {
            const auto pos = unhalf(vtx[(size_t(i) * SBR_MAX_VERTS + k) * 2 + 0]);
            const auto ra  = unhalf(vtx[(size_t(i) * SBR_MAX_VERTS + k) * 2 + 1]);
            printf("   (%.1f, %.1f) r=%.2f arc=%.1f\n", pos.first, pos.second,
                   ra.first, ra.second);
        }
    }
}
