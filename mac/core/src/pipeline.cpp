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
enum : GLuint {
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
    BIND_SEED_TILES = 11,
};

// Texture units, mirrored in the shaders.
enum : GLuint {
    UNIT_A = 0,        // primary input: reference blur, or the blur's source
    UNIT_B = 1,        // canvas, or the tensor field
    UNIT_C = 2,        // gradient field, or the error map
    UNIT_D = 3,        // temporal source-difference map
    UNIT_BRUSH = 4,
    UNIT_E = 5,        // optical flow field
    UNIT_F = 6,        // previous canvas, for flow advection
    UNIT_TOOTH = 7,    // brushkit: support tooth
    UNIT_SNAP = 8,     // brushkit: canvas snapshot for pickup / smear
};

// Uniform-buffer binding of the brushkit style block (Params is 0).
constexpr GLuint BIND_STYLE_UBO = 1;

// Upper bound on passes per layer, so a pathological grid cannot spin forever.
constexpr uint32_t MAX_CHUNKS = 32u;

inline uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

} // namespace

bool Pipeline::init() {
    glu::enableDebugOutput();

    std::string err;
    if (!reloadShaders(&err)) {
        fprintf(stderr, "shader build failed:\n%s\n", err.c_str());
        return false;
    }

    // Stroke buffers hold one chunk's worth of strokes and are never resized,
    // so changing radii or thresholds costs no allocation.
    const GLsizeiptr seedBytes   = GLsizeiptr(SBR_MAX_STROKES) * 32;
    const GLsizeiptr vertexBytes = GLsizeiptr(SBR_MAX_STROKES) * SBR_MAX_VERTS * 8;
    const GLsizeiptr headerBytes = GLsizeiptr(SBR_MAX_STROKES) * 16;

    m_seedBuf     = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, seedBytes,   nullptr, GL_DYNAMIC_COPY);
    m_vertexBuf   = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, vertexBytes, nullptr, GL_DYNAMIC_COPY);
    m_headerBuf   = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, headerBytes, nullptr, GL_DYNAMIC_COPY);
    m_counterBuf  = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, 16,          nullptr, GL_DYNAMIC_COPY);
    m_indirectBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, 16,          nullptr, GL_DYNAMIC_COPY);

    // Four counters per (layer, pass): seeds, traced, drawn, total points.
    m_statsBuf = glu::createBuffer(GL_COPY_WRITE_BUFFER,
                                   GLsizeiptr(SBR_MAX_LAYERS) * MAX_CHUNKS * 16,
                                   nullptr, GL_DYNAMIC_READ);

    // Frame-wide pool for relaxation: every stroke of the frame, not just one
    // pass'. The greedy phase appends to it as it paints.
    m_poolVertexBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER,
                                        GLsizeiptr(SBR_POOL_MAX) * SBR_MAX_VERTS * 8,
                                        nullptr, GL_DYNAMIC_COPY);
    m_poolHeaderBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER,
                                        GLsizeiptr(SBR_POOL_MAX) * 16, nullptr, GL_DYNAMIC_COPY);
    m_poolCounterBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_COPY);

    m_paramsUbo = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(TuningParams), nullptr, GL_DYNAMIC_DRAW);
    m_styleUbo  = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(StyleParams), nullptr, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &m_vao);   // core profile needs one bound, even empty

    m_etfMaxBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, 4, nullptr, GL_DYNAMIC_COPY);

    m_tRef.init(); m_tError.init(); m_tSeeds.init();
    m_tTrace.init(); m_tRaster.init(); m_tImpasto.init(); m_tRelax.init();
    m_tEtf.init(); m_tFlow.init(); m_tStyle.init();
    return true;
}

void Pipeline::shutdown() {
    GLuint bufs[] = {m_cellBuf, m_seedBuf, m_vertexBuf, m_headerBuf,
                     m_counterBuf, m_indirectBuf, m_statsBuf, m_paramsUbo,
                     m_poolVertexBuf, m_poolHeaderBuf, m_poolCounterBuf,
                     m_energyBuf, m_etfMaxBuf, m_styleUbo, m_tileBuf};
    glDeleteBuffers(15, bufs);
    GLuint texs[] = {m_srcTex, m_refTex, m_tmpTex, m_tensorTex, m_gradTex, m_errTex,
                     m_diffTex, m_canvasTex, m_heightTex, m_prevSrcTex,
                     m_prevCanvasTex, m_underTex, m_brushTex, m_etfTex,
                     m_lumaPrevTex, m_lumaCurTex, m_flowTex, m_flowTmpTex,
                     m_toothTex, m_finalTex, m_postTmpTex, m_snapTex, m_flatTex};
    glDeleteTextures(23, texs);
    if (m_canvasFbo) glDeleteFramebuffers(1, &m_canvasFbo);
    if (m_srcFbo) glDeleteFramebuffers(1, &m_srcFbo);
    if (m_finalFbo) glDeleteFramebuffers(1, &m_finalFbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool Pipeline::reloadShaders(std::string* err) {
    glu::Program blur, features, error, seeds, trace, stroke, impasto, canvasP;
    glu::Program poolP, relaxP, energyP, etfP, flowP;
    glu::Program supportP, styleFieldP, finishP, postP, flatP;

    auto comp = [&](glu::Program& pr, const char* f) {
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
    if (!comp(supportP, "support.comp"))  return false;
    if (!comp(styleFieldP, "stylefield.comp")) return false;
    if (!comp(finishP, "finish.comp"))    return false;
    if (!comp(postP, "post.comp"))        return false;
    if (!comp(flatP, "flat.comp"))        return false;
    if (!stroke.loadRaster("stroke.vert", "stroke.frag")) {
        if (err) *err = stroke.lastError;
        return false;
    }

    // Only swap once every stage compiled, so a typo during hot-reload leaves
    // the previous working pipeline running.
    GLuint old[] = {m_blur.id, m_features.id, m_error.id, m_seeds.id, m_trace.id,
                    m_stroke.id, m_impasto.id, m_canvasProg.id,
                    m_pool.id, m_relax.id, m_energy.id, m_etf.id, m_flow.id,
                    m_support.id, m_styleField.id, m_finish.id, m_post.id, m_flat.id};
    for (GLuint id : old) if (id) glDeleteProgram(id);

    m_blur = blur; m_features = features; m_error = error; m_seeds = seeds;
    m_trace = trace; m_stroke = stroke; m_impasto = impasto; m_canvasProg = canvasP;
    m_pool = poolP; m_relax = relaxP; m_energy = energyP; m_etf = etfP; m_flow = flowP;
    m_support = supportP; m_styleField = styleFieldP; m_finish = finishP;
    m_post = postP; m_flat = flatP;
    m_toothKey[0] = -1.f;   // a reloaded support.comp must be allowed to rebuild
    m_keyValid = false;     // new shaders: repaint even if no parameter moved
    return true;
}

void Pipeline::resize(int w, int h) {
    if (w == m_w && h == m_h) return;
    if (m_srcTex) {
        GLuint texs[] = {m_srcTex, m_refTex, m_tmpTex, m_tensorTex, m_gradTex,
                         m_errTex, m_diffTex, m_canvasTex, m_heightTex,
                         m_prevSrcTex, m_prevCanvasTex, m_underTex, m_etfTex,
                         m_lumaPrevTex, m_lumaCurTex, m_flowTex, m_flowTmpTex,
                         m_toothTex, m_finalTex, m_postTmpTex, m_snapTex, m_flatTex};
        glDeleteTextures(22, texs);
    }
    if (m_canvasFbo) glDeleteFramebuffers(1, &m_canvasFbo);
    if (m_srcFbo) glDeleteFramebuffers(1, &m_srcFbo);
    if (m_finalFbo) glDeleteFramebuffers(1, &m_finalFbo);

    m_w = w; m_h = h;
    // Mips on the source only so the 'average' underpaint can read the 1x1
    // level. No stage samples an intermediate level any more -- the reference
    // image is a real Gaussian now, not a mip.
    m_srcTex        = glu::createTexture2D(w, h, GL_RGBA8, /*mips=*/true);
    m_refTex        = glu::createTexture2D(w, h, GL_RGBA16F);
    m_tmpTex        = glu::createTexture2D(w, h, GL_RGBA16F);
    m_tensorTex     = glu::createTexture2D(w, h, GL_RGBA16F);
    m_gradTex       = glu::createTexture2D(w, h, GL_RGBA16F);
    m_etfTex        = glu::createTexture2D(w, h, GL_RGBA16F);
    // Mipped, because the LK pyramid *is* the mip chain: a box-filtered
    // level is a coarser approximation than the Gaussian a textbook
    // pyramid uses, but it costs one glGenerateTextureMipmap instead of a
    // per-level blur, and the flow is only steering a warp.
    m_lumaPrevTex   = glu::createTexture2D(w, h, GL_R16F, /*mips=*/true);
    m_lumaCurTex    = glu::createTexture2D(w, h, GL_R16F, /*mips=*/true);
    m_flowW = std::max(1, w / 4);
    m_flowH = std::max(1, h / 4);
    m_flowTex       = glu::createTexture2D(m_flowW, m_flowH, GL_RG16F);
    m_flowTmpTex    = glu::createTexture2D(m_flowW, m_flowH, GL_RG16F);
    m_errTex        = glu::createTexture2D(w, h, GL_R16F);
    m_diffTex       = glu::createTexture2D(w, h, GL_R16F);
    // Float canvas, matching the web version's Float32 buffer: 178K strokes
    // composite through this in one frame, and the error map and the stroke
    // termination test both read it back. Measured against RGBA8 the
    // difference is under 0.2/255, so this is for correctness rather than to
    // fix any observed artefact -- it costs nothing measurable either way.
    m_canvasTex     = glu::createTexture2D(w, h, GL_RGBA16F);
    m_heightTex     = glu::createTexture2D(w, h, GL_R16F);
    m_prevSrcTex    = glu::createTexture2D(w, h, GL_RGBA8);
    m_prevCanvasTex = glu::createTexture2D(w, h, GL_RGBA16F);
    m_underTex      = glu::createTexture2D(w, h, GL_RGBA16F);
    // brushkit style stages. The output image is separate from the canvas so
    // that relighting and aging never feed back into the temporal state.
    m_toothTex      = glu::createTexture2D(w, h, GL_R16F);
    m_finalTex      = glu::createTexture2D(w, h, GL_RGBA16F);
    m_postTmpTex    = glu::createTexture2D(w, h, GL_RGBA16F);
    m_snapTex       = glu::createTexture2D(w, h, GL_RGBA16F);
    m_flatTex       = glu::createTexture2D(w, h, GL_RGBA16F);
    m_toothKey[0] = -1.f;
    m_outputIsFinal = false;
    m_flatValid = false;

    m_havePrev = false;
    m_underValid = false;
    m_flowValid = false;

    glCreateFramebuffers(1, &m_canvasFbo);
    glNamedFramebufferTexture(m_canvasFbo, GL_COLOR_ATTACHMENT0, m_canvasTex, 0);
    glNamedFramebufferTexture(m_canvasFbo, GL_COLOR_ATTACHMENT1, m_heightTex, 0);
    const GLenum targets[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(m_canvasFbo, 2, targets);

    glCreateFramebuffers(1, &m_srcFbo);
    glNamedFramebufferTexture(m_srcFbo, GL_COLOR_ATTACHMENT0, m_srcTex, 0);

    glCreateFramebuffers(1, &m_finalFbo);
    glNamedFramebufferTexture(m_finalFbo, GL_COLOR_ATTACHMENT0, m_finalTex, 0);

    // Two floats per 16x16 workgroup. energy.comp needs one (its partial
    // sum); etf.comp's coherence pass needs two (a sum and a count), and the
    // two reductions are never in flight together, so they share the buffer
    // rather than duplicating an allocation that scales with the image.
    if (m_energyBuf) glDeleteBuffers(1, &m_energyBuf);
    const uint32_t groups = divUp(uint32_t(w), 16) * divUp(uint32_t(h), 16);
    m_energyBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER,
                                    GLsizeiptr(groups) * 8, nullptr, GL_DYNAMIC_READ);
}

bool Pipeline::setSource(const unsigned char* rgba, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    m_keyValid = false;   // new pixels: whatever is on the canvas is out of date
    // Keep the outgoing frame's source for the temporal difference test before
    // it is overwritten.
    if (w == m_w && h == m_h && m_srcTex) {
        glCopyImageSubData(m_srcTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_prevSrcTex, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
    }
    resize(w, h);
    glTextureSubImage2D(m_srcTex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateTextureMipmap(m_srcTex);
    return true;
}

// ── brushkit style stages ───────────────────────────────────────────────────

void Pipeline::uploadStyle(float time) {
    m_style.time = time;
    glNamedBufferSubData(m_styleUbo, 0, sizeof(StyleParams), &m_style);
    glBindBufferBase(GL_UNIFORM_BUFFER, BIND_STYLE_UBO, m_styleUbo);
}

// Turner's eye of the storm is wherever the light is. On arbitrary footage that
// has to be found per frame: the brightest texel of a ~16 px mip of the source
// (a broad region, not a specular point), eased over time on video so the
// vortex drifts with the light instead of jumping.
void Pipeline::trackVortex(bool smooth) {
    const int maxDim = std::max(m_w, m_h);
    const int level = std::max(0, int(std::floor(std::log2(float(maxDim) / 16.f))));
    const int lw = std::max(1, m_w >> level), lh = std::max(1, m_h >> level);
    std::vector<float> px(size_t(lw) * lh * 4);
    glGetTextureImage(m_srcTex, level, GL_RGBA, GL_FLOAT, GLsizei(px.size() * 4), px.data());
    float best = -1.f, bx = 0.5f, by = 0.45f;
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            const float* c = &px[(size_t(y) * lw + x) * 4];
            const float dx = (x + 0.5f) / lw - 0.5f, dy = (y + 0.5f) / lh - 0.5f;
            // a mild pull to the centre breaks ties toward a composed vortex
            const float s = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]
                          - 0.15f * (dx * dx + dy * dy);
            if (s > best) { best = s; bx = (x + 0.5f) / lw; by = (y + 0.5f) / lh; }
        }
    if (smooth && m_vortexEma[0] >= 0.f) {
        m_vortexEma[0] += (bx - m_vortexEma[0]) * 0.08f;
        m_vortexEma[1] += (by - m_vortexEma[1]) * 0.08f;
    } else {
        m_vortexEma[0] = bx;
        m_vortexEma[1] = by;
    }
    m_style.vortexX = m_vortexEma[0];
    m_style.vortexY = m_vortexEma[1];
}

void Pipeline::buildSupport() {
    const float key[4] = {m_style.support, m_style.supportScale, float(m_w), float(m_h)};
    if (std::equal(key, key + 4, m_toothKey)) return;
    std::copy(key, key + 4, m_toothKey);
    m_support.use();
    glBindImageTexture(0, m_toothTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glDispatchCompute(divUp(uint32_t(m_w), 16), divUp(uint32_t(m_h), 16), 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

// fields.py on the layer's direction field. Reads m_gradTex, writes m_etfTex,
// copies back -- the patch mode samples the field at a patch's seed, so an
// in-place rewrite would race.
void Pipeline::applyStyleField() {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    m_styleField.use();
    glBindTextureUnit(UNIT_A, m_gradTex);
    glBindImageTexture(0, m_etfTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                    GL_TEXTURE_UPDATE_BARRIER_BIT);
    glCopyImageSubData(m_etfTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_gradTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Pipeline::layStyleGround(TuningParams& p, const std::vector<float>& radii) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_flatValid = false;

    const bool flatPrint = m_style.groundMode > 1.5f;
    const bool wash = m_style.watercolor > 0.f;
    if (flatPrint || wash) {
        // Kuwahara of the source; the print styles then quantize it into the
        // canvas, the watercolour wash reads it straight from m_flatTex.
        m_flat.use();
        glUniform1i(m_flat.uniform("uPass"), 0);
        glBindTextureUnit(UNIT_A, m_srcTex);
        glBindImageTexture(0, m_flatTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
        glBindImageTexture(1, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    if (flatPrint) {
        glUniform1i(m_flat.uniform("uPass"), 1);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        m_flatValid = true;
    } else {
        // Toned ground + a thin lay-in of the coarsest layer's blur. For the
        // watercolour the lay-in opacity is 0: the canvas is the bare paper.
        buildReference(p, radii[0]);
        m_canvasProg.use();
        glUniform1i(m_canvasProg.uniform("uPass"), 4);
        glBindTextureUnit(UNIT_A, m_refTex);
        glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    if (wash) m_flatValid = true;

    glCopyImageSubData(m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_underTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
    m_underValid = true;
    const float zeroF[4] = {0.f, 0.f, 0.f, 0.f};
    glClearTexImage(m_heightTex, 0, GL_RED, GL_FLOAT, zeroF);
}

// Smear or wash -> finish -> overlays, always into m_finalTex. Nothing here
// writes the canvas, which is the painting state the next frame starts from.
void Pipeline::runPost() {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLuint color = m_canvasTex;

    m_post.use();
    glUniform1i(m_post.uniform("uFlatValid"), m_flatValid ? 1 : 0);
    glBindTextureUnit(UNIT_B, m_gradTex);
    glBindTextureUnit(UNIT_C, m_flatTex);
    glBindTextureUnit(UNIT_D, m_toothTex);
    glBindTextureUnit(UNIT_BRUSH, m_srcTex);   // unit 4 = uSrc in post.comp

    if (m_style.watercolor > 0.f && m_flatValid) {
        glUniform1i(m_post.uniform("uPass"), 1);
        glBindImageTexture(0, m_postTmpTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        color = m_postTmpTex;
    } else if (m_style.licStrength > 0.f) {
        glUniform1i(m_post.uniform("uPass"), 0);
        glBindTextureUnit(UNIT_A, m_canvasTex);
        glBindImageTexture(0, m_postTmpTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        color = m_postTmpTex;
    }

    if (m_style.finish > 0.f) {
        m_finish.use();
        glBindTextureUnit(UNIT_A, color);
        glBindTextureUnit(UNIT_B, m_heightTex);
        glBindTextureUnit(UNIT_C, m_toothTex);
        glBindImageTexture(0, m_finalTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    } else {
        glCopyImageSubData(color, GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_finalTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    const bool overlays = m_style.contour > 0.f || m_style.outline > 0.f ||
                          m_style.halftone > 0.f || m_style.caustics > 0.f ||
                          m_style.bokashi > 0.f || m_style.rain > 0.f ||
                          m_style.facet > 0.f || m_style.border > 0.f;
    if (overlays) {
        m_post.use();
        glUniform1i(m_post.uniform("uPass"), 2);
        glBindTextureUnit(UNIT_C, m_flatTex);
        glBindTextureUnit(UNIT_BRUSH, m_srcTex);
        glBindImageTexture(0, m_finalTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                        GL_FRAMEBUFFER_BARRIER_BIT);
    }
    m_outputIsFinal = true;
}

void Pipeline::buildBrushTiles(const std::vector<float>& radii, float bristleDensity) {
    if (radii == m_brushRadii && bristleDensity == m_brushDensity && m_brushTex) return;

    brush::TileArray t = brush::makeTiles(radii, bristleDensity);
    if (m_brushTex) glDeleteTextures(1, &m_brushTex);
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_brushTex);
    glTextureStorage3D(m_brushTex, 1, GL_R16F, brush::TILE_W, brush::TILE_ROWS_MAX, t.layers);
    glTextureSubImage3D(m_brushTex, 0, 0, 0, 0, brush::TILE_W, brush::TILE_ROWS_MAX,
                        t.layers, GL_RED, GL_FLOAT, t.data.data());
    // u wraps -- the tile is seamless along the stroke. v must clamp, or the
    // bristle profile folds over at the stroke's edges.
    glTextureParameteri(m_brushTex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_brushTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_brushTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_brushTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    m_brushRows = t.rows;
    m_brushRadii = radii;
    m_brushDensity = bristleDensity;
}

void Pipeline::ensureCellCapacity(uint32_t cells) {
    if (cells <= m_cellCapacity && m_cellBuf) return;
    if (m_cellBuf) glDeleteBuffers(1, &m_cellBuf);
    m_cellCapacity = cells;
    m_cellBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER,
                                  GLsizeiptr(cells) * 16, nullptr, GL_DYNAMIC_COPY);
}

// A stride near N/phi that is coprime to N, so i -> i*stride mod N visits every
// tile exactly once in a scattered order. 64-bit products keep it exact.
static uint32_t coprimeStride(uint32_t n) {
    if (n < 3u) return 1u;
    uint32_t k = std::max(1u, uint32_t(double(n) * 0.6180339887));
    auto gcd = [](uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; };
    while (gcd(k, n) != 1u) ++k;
    return k;
}

void Pipeline::ensureTileCapacity(uint32_t tiles) {
    if (tiles <= m_tileCapacity && m_tileBuf) return;
    if (m_tileBuf) glDeleteBuffers(1, &m_tileBuf);
    m_tileCapacity = tiles;
    m_tileBuf = glu::createBuffer(GL_SHADER_STORAGE_BUFFER,
                                  GLsizeiptr(tiles) * 4, nullptr, GL_DYNAMIC_COPY);
}

// Sums the per-(layer, pass) counters into the reported totals. Called at the
// top of render() for last frame's numbers, and by refreshStats() once a
// caller has waited for the current frame.
void Pipeline::readStats() {
    uint32_t counts[SBR_MAX_LAYERS * MAX_CHUNKS * 4] = {0};
    glGetNamedBufferSubData(m_statsBuf, 0, GLsizeiptr(sizeof(counts)), counts);

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
    glNamedBufferSubData(m_paramsUbo, 0, sizeof(TuningParams), &p);
}

// Separable Gaussian: src -> m_tmpTex (horizontal) -> dst (vertical). Sigma
// comes from the uniform block, which the caller has already uploaded.
void Pipeline::gaussian(GLuint srcTex, GLuint dstTex) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    m_blur.use();

    glUniform1i(m_blur.uniform("uPass"), 0);
    glBindTextureUnit(UNIT_A, srcTex);
    glBindImageTexture(0, m_tmpTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUniform1i(m_blur.uniform("uPass"), 1);
    glBindTextureUnit(UNIT_A, m_tmpTex);
    glBindImageTexture(0, dstTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

void Pipeline::buildReference(TuningParams& p, float radius) {
    // worker.js: `const sigma = Math.max(0.1, radius * 0.5)`.
    p.blurSigma = std::max(0.1f, radius * 0.5f);
    uploadParams(p);
    gaussian(m_srcTex, m_refTex);
}

void Pipeline::buildGradients(const TuningParams& p) {
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);
    m_features.use();
    glBindTextureUnit(UNIT_A, m_refTex);

    if (p.tensorSigma <= 0.f) {
        glUniform1i(m_features.uniform("uPass"), 0);
        glBindImageTexture(0, m_gradTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        return;
    }

    // Structure-tensor path: raw components -> Gaussian smooth -> eigenvector,
    // which is how computeGradientsST reuses gaussianBlurRGB on a packed RGB
    // buffer. The smoothed field lands back in m_tensorTex.
    glUniform1i(m_features.uniform("uPass"), 1);
    glBindImageTexture(0, m_gradTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    TuningParams q = p;
    q.blurSigma = p.tensorSigma;
    uploadParams(q);
    gaussian(m_gradTex, m_tensorTex);
    uploadParams(p);

    m_features.use();
    glUniform1i(m_features.uniform("uPass"), 2);
    glBindTextureUnit(UNIT_B, m_tensorTex);
    glBindImageTexture(0, m_gradTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
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
    m_etf.use();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_ETF_MAX, m_etfMaxBuf);

    // w_m needs the field's maximum magnitude, and the field is rebuilt per
    // layer, so the reduction is too. Cleared to 0, which is the identity for
    // a max over non-negative floats and is also 0.0f's bit pattern.
    const uint32_t zero = 0;
    glClearNamedBufferSubData(m_etfMaxBuf, GL_R32UI, 0, 4,
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    glUniform1i(m_etf.uniform("uPass"), 0);
    glBindTextureUnit(UNIT_A, m_gradTex);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glUniform1i(m_etf.uniform("uPass"), 1);
    GLuint src = m_gradTex, dst = m_etfTex;
    for (int i = 0; i < cfg.etfIterations; ++i) {
        glBindTextureUnit(UNIT_A, src);
        glBindImageTexture(0, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        std::swap(src, dst);
    }

    // An odd iteration count leaves the result in m_etfTex, and every stage
    // downstream reads m_gradTex by name.
    if (src != m_gradTex) {
        glCopyImageSubData(m_etfTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_gradTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
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
    if (!m_w || !m_h || !m_energyBuf || !m_etf.id) return 0.0;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_ENERGY, m_energyBuf);
    m_etf.use();
    glUniform1i(m_etf.uniform("uPass"), 2);
    glBindTextureUnit(UNIT_A, m_gradTex);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<float> partial(size_t(gx) * gy * 2);
    glGetNamedBufferSubData(m_energyBuf, 0, GLsizeiptr(partial.size() * 4), partial.data());

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

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_tFlow.begin();
    m_flow.use();
    glUniform2i(m_flow.uniform("uFlowSize"), m_flowW, m_flowH);

    // Luma pyramids for both frames. Built here rather than cached from the
    // previous frame's work because setSource has already overwritten the
    // source texture by the time the renderer runs, and a second cached
    // pyramid would be one more thing to keep in step with it.
    glUniform1i(m_flow.uniform("uPass"), 0);
    glBindTextureUnit(UNIT_D, m_prevSrcTex);
    glBindImageTexture(1, m_lumaPrevTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glDispatchCompute(gx, gy, 1);
    glBindTextureUnit(UNIT_D, m_srcTex);
    glBindImageTexture(1, m_lumaCurTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glGenerateTextureMipmap(m_lumaPrevTex);
    glGenerateTextureMipmap(m_lumaCurTex);

    // Start from zero displacement. Warm-starting from the previous frame's
    // field would track constant motion in fewer iterations, but it also
    // carries a bad estimate forward indefinitely, and a wrong flow smears the
    // canvas rather than merely failing to help.
    const float zero[2] = {0.f, 0.f};
    glClearTexImage(m_flowTex, 0, GL_RG, GL_FLOAT, zero);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

    glBindTextureUnit(UNIT_A, m_lumaPrevTex);
    glBindTextureUnit(UNIT_B, m_lumaCurTex);
    glUniform1i(m_flow.uniform("uWindow"), 3);   // 7x7, in level pixels

    GLuint src = m_flowTex, dst = m_flowTmpTex;
    auto pass = [&](int which, int level) {
        glUniform1i(m_flow.uniform("uPass"), which);
        glUniform1i(m_flow.uniform("uLevel"), level);
        glBindTextureUnit(UNIT_C, src);
        glBindImageTexture(0, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
        glDispatchCompute(fx, fy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
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
        glCopyImageSubData(m_flowTmpTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_flowTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_flowW, m_flowH, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    endStage(m_tFlow, m_ms.flow);
    m_flowValid = true;

    if (m_logFlow) {
        // Reads the field back, so it stalls. The mean and max magnitude are
        // the first thing to look at when advection is not helping: a field of
        // zeros means the stage never ran or the windows were all rejected,
        // and a huge max means a bad window escaped the clamp.
        std::vector<float> f(size_t(m_flowW) * m_flowH * 2);
        glGetTextureImage(m_flowTex, 0, GL_RG, GL_FLOAT,
                          GLsizei(f.size() * sizeof(float)), f.data());
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (allowCache && m_underValid) {
        glCopyImageSubData(m_underTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
        const float zeroC[4] = {0.f, 0.f, 0.f, 0.f};
        glClearTexImage(m_heightTex, 0, GL_RED, GL_FLOAT, zeroC);
        return;
    }

    if (temporal) {
        // brushkit: exposed borders take a fresh lay-in of this frame, so the
        // coarsest blur has to exist before the advection pass reads it.
        // Fresh paint needs the same lay-in for the pixels it wipes.
        if ((styleOn() && m_flowValid) || cfg.freshPaint > 0.f) {
            buildReference(p, radii[0]);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        // Start from the previous frame's paint and repaint only what moved.
        m_canvasProg.use();
        glBindTextureUnit(UNIT_A, m_refTex);
        glBindTextureUnit(UNIT_E, m_flowTex);

        if (m_flowValid) {
            // Advect the paint along the flow first, so "what moved" is asked
            // of the subject rather than of the pixel grid.
            glUniform1i(m_canvasProg.uniform("uPass"), 3);
            glBindTextureUnit(UNIT_F, m_prevCanvasTex);
            glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(gxi, gyi, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
        } else {
            glCopyImageSubData(m_prevCanvasTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                               m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
        }

        glUniform1i(m_canvasProg.uniform("uPass"), 2);
        glUniform1i(m_canvasProg.uniform("uUseFlow"), m_flowValid ? 1 : 0);
        glUniform1f(m_canvasProg.uniform("uFreshPaint"), std::clamp(cfg.freshPaint, 0.f, 1.f));
        glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindTextureUnit(UNIT_B, m_srcTex);
        glBindTextureUnit(UNIT_C, m_prevSrcTex);
        glBindImageTexture(1, m_diffTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glDispatchCompute(gxi, gyi, 1);

        if (m_logFlow) {
            // What the cells are actually gated on. seeds.comp skips a cell
            // when the *maximum* difference inside it is below the threshold,
            // so the mean alone does not say whether anything will be skipped
            // -- hence the percentiles.
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
            std::vector<float> d(size_t(m_w) * m_h);
            glGetTextureImage(m_diffTex, 0, GL_RED, GL_FLOAT,
                              GLsizei(d.size() * sizeof(float)), d.data());
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
        m_canvasProg.use();
        glUniform1i(m_canvasProg.uniform("uPass"), 0);
        glBindTextureUnit(UNIT_A, m_refTex);
        glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gxi, gyi, 1);
    } else {
        float fill[4] = {1.f, 1.f, 1.f, 1.f};
        if (cfg.underpaint == Underpaint::Average) {
            // The 1x1 mip is the image mean -- the quantity the web version
            // accumulates in a loop.
            const int levels = 1 + int(std::floor(std::log2(float(std::max(m_w, m_h)))));
            glGetTextureImage(m_srcTex, levels - 1, GL_RGBA, GL_FLOAT,
                              GLsizei(sizeof(fill)), fill);
        }
        m_canvasProg.use();
        glUniform1i(m_canvasProg.uniform("uPass"), 1);
        glUniform3f(m_canvasProg.uniform("uFill"), fill[0], fill[1], fill[2]);
        glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gxi, gyi, 1);
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // Keep the ground so a relaxation repaint can copy it back instead of
    // recomputing the blur.
    glCopyImageSubData(m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_underTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
    m_underValid = true;

    const float zeroF[4] = {0.f, 0.f, 0.f, 0.f};
    glClearTexImage(m_heightTex, 0, GL_RED, GL_FLOAT, zeroF);
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
    m_pool.use();
    glUniform1i(m_pool.uniform("uPass"), 1);
    glUniform1ui(m_pool.uniform("uPoolCapacity"), SBR_POOL_MAX);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    // stroke.vert reads whatever is bound at BIND_VERTS / BIND_HEADERS, so
    // pointing those at the pool is the whole change.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_VERTS, m_poolVertexBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_HEADERS, m_poolHeaderBuf);

    glBindFramebuffer(GL_FRAMEBUFFER, m_canvasFbo);
    glViewport(0, 0, m_w, m_h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (styleOn() && m_style.brushModel > 0.5f) glBlendFunci(1, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    else                                        glBlendFunci(1, GL_ONE, GL_ONE);

    m_stroke.use();
    glBindTextureUnit(UNIT_BRUSH, m_brushTex);
    glBindTextureUnit(UNIT_TOOTH, m_toothTex);
    glBindTextureUnit(UNIT_SNAP, m_snapTex);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuf);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glTextureBarrier();

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_VERTS, m_vertexBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_HEADERS, m_headerBuf);
}

double Pipeline::measureEnergy() {
    if (!m_w || !m_h || !m_energyBuf) return 0.0;
    const uint32_t gx = divUp(uint32_t(m_w), 16), gy = divUp(uint32_t(m_h), 16);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_ENERGY, m_energyBuf);
    m_energy.use();
    glBindTextureUnit(UNIT_A, m_srcTex);
    glBindTextureUnit(UNIT_B, m_canvasTex);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::vector<float> partial(size_t(gx) * gy);
    glGetNamedBufferSubData(m_energyBuf, 0, GLsizeiptr(partial.size() * 4), partial.data());

    double sum = 0.0;
    for (float v : partial) sum += v;
    return sum / (double(m_w) * double(m_h));   // mean Lab error per pixel
}

// Hertzmann 2001. The greedy phase has placed the strokes; now improve them.
void Pipeline::relax(TuningParams& p, const RenderConfig& cfg,
                     const std::vector<float>& radii) {
    uint32_t counters[4] = {0, 0, 0, 0};
    glGetNamedBufferSubData(m_poolCounterBuf, 0, GLsizeiptr(sizeof(counters)), counters);
    m_poolCount = counters[0];

    if (m_poolCount == 0) return;
    if (m_poolCount > SBR_POOL_MAX) {
        // pool.comp dropped the overflow, so a repaint from the pool would
        // lose strokes the greedy phase did paint. Leaving the greedy canvas
        // alone is the honest outcome; say so rather than silently degrading.
        fprintf(stderr,
                "Brushkit: %u strokes exceeds the relaxation pool (%u); skipping"
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
            m_relax.use();
            glUniform1ui(m_relax.uniform("uPoolCapacity"), SBR_POOL_MAX);
            glUniform1ui(m_relax.uniform("uSubPass"), sub);
            glUniform1ui(m_relax.uniform("uSubPassCount"), subs);
            glBindTextureUnit(UNIT_A, m_srcTex);
            // The frame's ground, not the canvas -- see the long comment in
            // relax.comp about why measuring "what is underneath" against the
            // canvas that already contains the stroke is circular.
            glBindTextureUnit(UNIT_B, m_underTex);
            glDispatchCompute(divUp(m_poolCount, 64), 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }

        // One repaint per iteration, not per sub-pass. The kernel reads only
        // the source and the frame's ground, neither of which relaxation
        // changes, so sub-passes no longer have to see each other's paint --
        // they did when the score was measured against the live canvas.
        repaintFromPool(p, cfg, radii);
        if (m_logRelax) m_relaxLog.push_back(measureEnergy());
    }

    glGetNamedBufferSubData(m_poolCounterBuf, 0, GLsizeiptr(sizeof(counters)), counters);
    m_relaxRemoved = counters[2];
    m_relaxAccepted = counters[3];

    if (m_logRelax) {
        // What relaxation actually did to the geometry. Read from the pool,
        // because the trace counters predate it.
        struct Hdr { uint32_t vertexCount, layerTex, color; float totalLen; };
        std::vector<Hdr> hdr(m_poolCount);
        glGetNamedBufferSubData(m_poolHeaderBuf, 0,
                                GLsizeiptr(hdr.size() * sizeof(Hdr)), hdr.data());
        std::vector<uint32_t> v0(size_t(m_poolCount) * 2);
        // The first vertex of each stroke carries its radius; read the whole
        // slot stride and stride over it rather than pulling every point.
        std::vector<uint32_t> verts(size_t(m_poolCount) * SBR_MAX_VERTS * 2);
        glGetNamedBufferSubData(m_poolVertexBuf, 0,
                                GLsizeiptr(verts.size() * 4), verts.data());

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

// FNV-1a over raw bytes. Every struct hashed below is a flat run of 32-bit
// floats/ints with no padding, so equal values hash equal.
static uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

void Pipeline::render(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
    m_keyValid = false;   // a direct paint says nothing about what renderIfChanged last saw
    renderImpl(base, cfg, temporal);
}

bool Pipeline::renderIfChanged(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
    if (!m_w || !m_h) return false;

    TuningParams p = base;
    if (p.jitterPerFrame <= 0.5f) p.frame = 0.f;   // frame only reaches the shaders through the jitter salt
    StyleParams st = m_style;
    if (!(st.rain > 0.f)) st.time = 0.f;           // ...and style.time only animates rain

    uint64_t key = 1469598103934665603ull;
    key = fnv(key, &p, sizeof(p));
    key = fnv(key, &st, sizeof(st));
    key = fnv(key, cfg.radii.data(), cfg.radii.size() * sizeof(float));
    const int ints[] = {int(cfg.underpaint), cfg.passesPerLayer, cfg.relaxIterations,
                        cfg.etfIterations, cfg.flowLevels, cfg.flowIterations,
                        cfg.relaxSubPasses, int(cfg.layerSpecs.size())};
    key = fnv(key, ints, sizeof(ints));
    const float flts[] = {cfg.bristleDensity, cfg.freshPaint};
    key = fnv(key, flts, sizeof(flts));
    key = fnv(key, cfg.layerSpecs.data(), cfg.layerSpecs.size() * sizeof(LayerSpec));

    if (!temporal && m_keyValid && key == m_lastKey) {
        // Nothing changed. The previous paint's stroke counts are still wanted
        // for the overlay; read them once now that the GPU has had time.
        if (m_statsPending) {
            readStats();   // blocks until the paint has finished, so the queries below are ready
#ifndef SBR_METAL
            GLuint64 t0 = 0, t1 = 0;
            glGetQueryObjectui64v(m_tsQ[0], GL_QUERY_RESULT, &t0);
            glGetQueryObjectui64v(m_tsQ[1], GL_QUERY_RESULT, &t1);
            if (t1 > t0) m_settledMs = double(t1 - t0) * 1e-6;
#endif
            m_statsPending = false;
        }
        return false;
    }
    renderImpl(base, cfg, temporal);
    m_lastKey = key;
    m_keyValid = !temporal;
    return true;
}

void Pipeline::renderImpl(const TuningParams& base, const RenderConfig& cfg, bool temporal) {
#ifdef SBR_METAL
    metal::AutoreleaseScope renderPool;
#endif
    if (!m_w || !m_h) return;
    ++m_renderCount;
    m_statsPending = true;
    m_settledMs = -1.0;
#ifndef SBR_METAL
    if (!m_tsQ[0]) glGenQueries(2, m_tsQ);
    glQueryCounter(m_tsQ[0], GL_TIMESTAMP);
#endif

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
    glClearNamedBufferData(m_statsBuf, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zeroStats);

    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_paramsUbo);
    uploadParams(p);

    // The style block is always bound: with enabled == 0 every shader takes
    // the classic rendering path.
    const bool style = styleOn();
    const bool bkBrush = style && m_style.brushModel > 0.5f;
    const bool strokesOff = style && m_style.strokesOff > 0.5f;
    m_outputIsFinal = false;
    if (style && m_style.fieldMode > 0.5f && m_style.fieldMode < 1.5f && m_style.autoVortex > 0.5f)
        trackVortex(temporal && m_havePrev);
    uploadStyle(p.frame);
    if (style) {
        m_tStyle.begin();
        buildSupport();
        endStage(m_tStyle, m_ms.style);
    }

    // --- canvas setup ----------------------------------------------------
    // The flow has to exist before the underpaint, which is what consumes it.
    // It is only ever used by the temporal path, so there is no reason to pay
    // for it on a still.
    if (useTemporal && !strokesOff) computeFlow(p, cfg);
    else m_flowValid = false;

    m_tRef.begin();
    const bool styleGround = style && (m_style.groundMode > 0.5f || m_style.watercolor > 0.f);
    if (styleGround && (!useTemporal || strokesOff)) layStyleGround(p, radii);
    else layUnderpaint(p, cfg, radii, useTemporal && !strokesOff);
    endStage(m_tRef, m_ms.ref);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_SEEDS, m_seedBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_VERTS, m_vertexBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_COUNTERS, m_counterBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_HEADERS, m_headerBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_INDIRECT, m_indirectBuf);

    // The frame-wide pool. trace.comp reserves a slot range per pass and
    // pool.comp copies into it, so that after the greedy phase relaxation has
    // every stroke of the frame to work with.
    const bool wantRelax = cfg.relaxIterations > 0;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_POOL_VERTS, m_poolVertexBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_POOL_HEADERS, m_poolHeaderBuf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_POOL_COUNTER, m_poolCounterBuf);
    {
        const uint32_t zero = 0;
        glClearNamedBufferSubData(m_poolCounterBuf, GL_R32UI, 0, 16,
                                  GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    }

    // --- per layer, coarse to fine ---------------------------------------
    // Flat print styles (ukiyo-e, pop, Hockney, watercolour) place no strokes.
    const size_t layerCount = strokesOff ? 0 : radii.size();
    const TuningParams layerBase = p;
    for (size_t l = 0; l < layerCount; ++l) {
        const float radius = radii[l];
        p.currentLayer = float(l);
        p.layerRadius = radius;
        p.firstLayer = (l == 0) ? 1.f : 0.f;

        // brushkit per-layer roles: restore the global knobs, then apply this
        // layer's overrides. Without a style there are none and nothing moves.
        p.maxStrokeLength = layerBase.maxStrokeLength;
        p.minStrokeLength = layerBase.minStrokeLength;
        p.threshold = layerBase.threshold;
        p.curvature = layerBase.curvature;
        p.opacity = layerBase.opacity;
        p.gridFactor = layerBase.gridFactor;
        if (style && l < cfg.layerSpecs.size()) {
            const LayerSpec& ls = cfg.layerSpecs[l];
            auto ov = [](float& dst, float v) { if (!std::isnan(v)) dst = v; };
            ov(p.maxStrokeLength, ls.maxLen);
            ov(p.minStrokeLength, ls.minLen);
            ov(p.threshold, ls.threshold);
            ov(p.curvature, ls.curvature);
            ov(p.opacity, ls.opacity);
            ov(p.gridFactor, ls.gridFactor);
        }
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
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_CELLS, m_cellBuf);

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

        // brushkit orientation logic on top of the image's own flow, and the
        // snapshot the brush reads for wet pickup / smear.
        if (style) {
            m_tStyle.begin();
            applyStyleField();
            if (bkBrush && (m_style.pickup > 0.f || m_style.smear > 0.f)) {
                glCopyImageSubData(m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                                   m_snapTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            }
            endStage(m_tStyle, m_ms.style);
        }

        // --- error map + per-cell reduction ------------------------------
        // Once per layer, against the canvas as it stands before this layer
        // paints. worker.js does the same: it builds `err` before the cell
        // loop and lets the loop mutate the canvas underneath it. Keeping it
        // out of the chunk loop is therefore the faithful choice as well as
        // the cheap one -- each cell is still tested exactly once, because the
        // chunks partition the cell grid.
        m_tError.begin();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_error.use();
        glUniform1i(m_error.uniform("uPass"), 0);
        glUniform1i(m_error.uniform("uTemporal"), useTemporal ? 1 : 0);
        glBindTextureUnit(UNIT_A, m_refTex);
        glBindTextureUnit(UNIT_B, m_canvasTex);
        glBindImageTexture(0, m_errTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glDispatchCompute(gxi, gyi, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

        glUniform1i(m_error.uniform("uPass"), 1);
        glBindTextureUnit(UNIT_C, m_errTex);
        glBindTextureUnit(UNIT_D, m_diffTex);
        glDispatchCompute(divUp(gridW, 16), divUp(gridH, 16), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        endStage(m_tError, m_ms.error);

        if (m_debugCells) {
            struct CellRec { float meanErr; uint32_t best; float maxDiff; float pad; };
            std::vector<CellRec> rec(cells);
            glGetNamedBufferSubData(m_cellBuf, 0, GLsizeiptr(cells * sizeof(CellRec)), rec.data());
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
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const uint32_t zero = 0;
            glClearNamedBufferSubData(m_counterBuf, GL_R32UI, 0, 16,
                                      GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

            // Deterministic compaction in three dispatches (see seeds.comp).
            const uint32_t tilesX = divUp(gridW, 8), tilesY = divUp(gridH, 8);
            ensureTileCapacity(tilesX * tilesY);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BIND_SEED_TILES, m_tileBuf);
            m_seeds.use();
            glUniform1i(m_seeds.uniform("uTemporal"), useTemporal ? 1 : 0);
            glUniform1ui(m_seeds.uniform("uWgCount"), tilesX * tilesY);
            glUniform1ui(m_seeds.uniform("uTileStride"), coprimeStride(tilesX * tilesY));
            glBindTextureUnit(UNIT_A, m_refTex);
            glUniform1i(m_seeds.uniform("uPhase"), 0);
            glDispatchCompute(tilesX, tilesY, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            glUniform1i(m_seeds.uniform("uPhase"), 1);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            glUniform1i(m_seeds.uniform("uPhase"), 2);
            glDispatchCompute(tilesX, tilesY, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            endStage(m_tSeeds, m_ms.seeds);

            // --- trace ---------------------------------------------------
            m_tTrace.begin();
            m_trace.use();
            glBindTextureUnit(UNIT_A, m_refTex);
            glBindTextureUnit(UNIT_B, m_canvasTex);
            glBindTextureUnit(UNIT_C, m_gradTex);
            glDispatchCompute(divUp(traceBound, 64), 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
            endStage(m_tTrace, m_ms.trace);

            // --- rasterise -----------------------------------------------
            m_tRaster.begin();
            glBindFramebuffer(GL_FRAMEBUFFER, m_canvasFbo);
            glViewport(0, 0, m_w, m_h);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            // Colour: premultiplied source-over, once per stroke. Height:
            // plain accumulation, matching `heightBuf[i] += mv * impasto` --
            // or, for the brushkit brush, deposit plus partial levelling of
            // the relief underneath (its `flatten`), carried in the alpha.
            glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            if (bkBrush) glBlendFunci(1, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            else         glBlendFunci(1, GL_ONE, GL_ONE);

            m_stroke.use();
            glBindTextureUnit(UNIT_BRUSH, m_brushTex);
            glBindTextureUnit(UNIT_TOOTH, m_toothTex);
            glBindTextureUnit(UNIT_SNAP, m_snapTex);
            glBindVertexArray(m_vao);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuf);
            glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

            glDisable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glTextureBarrier();   // the next pass samples what we just drew
            endStage(m_tRaster, m_ms.raster);

            // --- append this pass to the frame pool ----------------------
            if (wantRelax) {
                m_pool.use();
                glUniform1i(m_pool.uniform("uPass"), 0);
                glUniform1ui(m_pool.uniform("uPoolCapacity"), SBR_POOL_MAX);
                glDispatchCompute(divUp(traceBound, 64), 1, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }

            // Stroke count for the overlay, copied GPU-side. Reading the
            // counter here instead would pull a buffer from video to host
            // memory mid-frame and serialise the whole pipeline.
            glCopyNamedBufferSubData(m_counterBuf, m_statsBuf, 0,
                                     GLintptr(l * MAX_CHUNKS + chunk) * 16, 16);
        }
    }

    // --- relaxation -------------------------------------------------------
    if (wantRelax) relax(p, cfg, radii);

    // --- impasto lighting -------------------------------------------------
    // A style with its own finish relights the height field there instead,
    // into the output image, and leaves the canvas unlit.
    m_tImpasto.begin();
    if (p.impastoLight > 0.f && !(style && m_style.finish > 0.f)) {
        m_impasto.use();
        glBindTextureUnit(UNIT_A, m_heightTex);
        glBindImageTexture(0, m_canvasTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
        glDispatchCompute(gxi, gyi, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    endStage(m_tImpasto, m_ms.impasto);

    // --- brushkit finish + effects, into the output image ------------------
    if (style) {
        m_tStyle.begin();
        runPost();
        endStage(m_tStyle, m_ms.style);
    }

    // Keep this frame's canvas for the next temporal frame.
    glCopyImageSubData(m_canvasTex, GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_prevCanvasTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_w, m_h, 1);
    m_havePrev = true;
#ifndef SBR_METAL
    glQueryCounter(m_tsQ[1], GL_TIMESTAMP);
#endif
}

std::vector<unsigned char> Pipeline::readCanvas() const {
    std::vector<unsigned char> px(size_t(m_w) * size_t(m_h) * 4);
    glGetTextureImage(outputTexture(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      GLsizei(px.size()), px.data());
    return px;
}

void Pipeline::blitToScreen(int x, int y, int w, int h, int fbW, int fbH,
                            ViewMode mode, const ViewXform& xf) const {
    (void)fbW; (void)fbH;
    if (m_w <= 0 || m_h <= 0 || w <= 0 || h <= 0) return;

    // Fit preserves aspect. It did not before, which stretched every preview to
    // the shape of the window -- and stroke *shape* is most of what is being
    // judged here, so a horizontal squash reads as a change to the painting.
    const float fit = std::min(float(w) / float(m_w), float(h) / float(m_h));
    const float scale = fit * std::max(xf.zoom, 0.01f);
    const float fx = xf.focusX < 0.f ? float(m_w) * 0.5f : xf.focusX;
    const float fy = xf.focusY < 0.f ? float(m_h) * 0.5f : xf.focusY;

    // Window y grows upwards and image y downwards, hence the sign flip on the
    // y term: windowY(iy) = cy - (iy - fy) * scale.
    const float cx = float(x) + float(w) * 0.5f;
    const float cy = float(y) + float(h) * 0.5f;
    const int dx0   = int(std::lround(cx - fx * scale));
    const int dx1   = int(std::lround(cx + (float(m_w) - fx) * scale));
    const int dyTop = int(std::lround(cy + fy * scale));
    const int dyBot = int(std::lround(cy - (float(m_h) - fy) * scale));

    // Past 1:1 the point of zooming is to inspect brush texture and impasto
    // relief, and a linear filter there invents detail the canvas does not
    // have. Below 1:1 linear is the honest choice instead.
    const GLenum filter = (scale > 1.f) ? GL_NEAREST : GL_LINEAR;

    // The destination rect runs outside the viewport whenever the view is
    // zoomed in, and glBlitFramebuffer clips only against the framebuffer --
    // without a scissor the image would paint straight over the control panel.
    // The scissor is also what makes the split a wipe rather than two blits.
    GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint oldScissor[4];
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glEnable(GL_SCISSOR_TEST);

    // Source rect is flipped in y: canvas texel row 0 is image row 0, while the
    // default framebuffer's origin is its bottom-left. Orientation is applied
    // here, at the edge, never inside the pipeline.
    auto blitRegion = [&](GLuint fbo, int sx, int sw) {
        if (sw <= 0) return;
        glScissor(sx, y, sw, h);
        glBlitNamedFramebuffer(fbo, 0, 0, m_h, m_w, 0, dx0, dyBot, dx1, dyTop,
                               GL_COLOR_BUFFER_BIT, filter);
    };

    // Letterbox. Nothing cleared here before, because the blit always covered
    // the whole viewport; an aspect-correct one does not.
    glScissor(x, y, w, h);
    glClearColor(m_backdrop[0], m_backdrop[1], m_backdrop[2], 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // The painted side shows the output image: the finished, aged painting
    // when a style ran its post chain, the canvas itself otherwise.
    const GLuint paintedFbo = m_outputIsFinal ? m_finalFbo : m_canvasFbo;
    switch (mode) {
        case ViewMode::Source: blitRegion(m_srcFbo, x, w); break;
        case ViewMode::Split: {
            // A wipe, not two shrunken copies side by side: both halves share
            // one framing, so a feature sits at the same place on either side
            // of the divider. Two independently fitted copies cannot be
            // compared at all once the view is zoomed in.
            const int cut = std::clamp(int(float(w) * xf.wipe), 0, w);
            blitRegion(m_srcFbo, x, cut);
            blitRegion(paintedFbo, x + cut, w - cut);
            break;
        }
        default: blitRegion(paintedFbo, x, w); break;
    }

    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (!hadScissor) glDisable(GL_SCISSOR_TEST);
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
    glGetNamedBufferSubData(m_headerBuf, 0, GLsizeiptr(hdr.size() * 4), hdr.data());
    std::vector<uint32_t> vtx(size_t(n) * SBR_MAX_VERTS * 2);
    glGetNamedBufferSubData(m_vertexBuf, 0, GLsizeiptr(vtx.size() * 4), vtx.data());

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
