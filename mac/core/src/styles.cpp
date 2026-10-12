#include "styles.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

namespace styles {
namespace {

void hexTo(const char* h, float* out) {
    if (*h == '#') ++h;
    const unsigned v = unsigned(std::strtoul(h, nullptr, 16));
    out[0] = float((v >> 16) & 255u) / 255.f;
    out[1] = float((v >> 8) & 255u) / 255.f;
    out[2] = float(v & 255u) / 255.f;
}

void palette(StyleParams& st, std::initializer_list<const char*> hexes) {
    int i = 0;
    for (const char* h : hexes) {
        if (i >= 16) break;
        hexTo(h, st.palette[i]);
        st.palette[i][3] = 1.f;
        ++i;
    }
    st.paletteCount = float(i);
}

void ground(StyleParams& st, const char* hex) {
    float c[3];
    hexTo(hex, c);
    st.groundR = c[0]; st.groundG = c[1]; st.groundB = c[2];
}

void contourColor(StyleParams& st, const char* hex) {
    float c[3];
    hexTo(hex, c);
    st.contourR = c[0]; st.contourG = c[1]; st.contourB = c[2];
}

void radii(RenderConfig& cfg, float s, std::initializer_list<float> r) {
    cfg.radii.clear();
    for (float v : r) cfg.radii.push_back(std::max(1.f, v * s));
}

LayerSpec L(float maxLen, float minLen, float threshold, float curvature = NAN,
            float opacity = NAN, float gridFactor = NAN) {
    LayerSpec ls;
    ls.maxLen = maxLen; ls.minLen = minLen; ls.threshold = threshold;
    ls.curvature = curvature; ls.opacity = opacity; ls.gridFactor = gridFactor;
    return ls;
}

void layerA(StyleParams& st, int l, float impastoX, float dryoutAdd, float softX, float minValue) {
    st.layerA[l][0] = impastoX; st.layerA[l][1] = dryoutAdd;
    st.layerA[l][2] = softX;    st.layerA[l][3] = minValue;
}
void layerB(StyleParams& st, int l, float widthX, float valueX, float rotate, float toothAdd) {
    st.layerB[l][0] = widthX; st.layerB[l][1] = valueX;
    st.layerB[l][2] = rotate; st.layerB[l][3] = toothAdd;
}

// Shared starting point for every brushkit style: the procedural brush, a toned
// ground with a lay-in, the finish pass on, and the web port's own colour
// jitter and brush tiles off (brushkit does its colour in Lab instead).
void base(TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    st = StyleParams{};
    st.enabled = 1.f;
    st.brushModel = 1.f;
    st.groundMode = 1.f;
    st.finish = 1.f;
    st.temporalRefresh = 0.05f;   // video: renew carried paint ~every 20 frames
    p.jitterHue = p.jitterSat = p.jitterVal = 0.f;
    p.sizeJitter = 0.15f;
    p.angleJitter = 0.f;
    p.opacityJitter = 0.f;
    p.texStrength = 0.f;
    p.impastoStrength = 0.f;
    p.impastoLight = 0.f;
    p.dryBrush = 0.f;
    p.tensorSigma = 0.f;
    p.gridFactor = 1.f;
    p.opacity = 0.9f;
    p.curvature = 1.f;
    p.threshold = 30.f;
    p.maxStrokeLength = 8.f;
    p.minStrokeLength = 2.f;
    cfg.underpaint = Underpaint::Blur;
    cfg.passesPerLayer = 8;
    cfg.etfIterations = 0;
    cfg.relaxIterations = 0;
    cfg.layerSpecs.clear();
}

// Flat / print styles: no strokes, a Kuwahara + palette fill and overlays.
void flatBase(TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    st.strokesOff = 1.f;
    st.brushModel = 0.f;
    st.groundMode = 2.f;
    st.impastoLight = 0.f;
    st.specular = 0.f;
}

// ---------------------------------------------------------------------------
// The recipes. Each comment names the brushkit function it restates.
// ---------------------------------------------------------------------------

// post_impressionism.cezanne
void cezanne(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {7.5f, 5.f, 3.f, 2.f});
    p.curvature = 0.f;                       // straight dabs (infinite inertia)
    p.gridFactor = 1.2f;
    cfg.layerSpecs = {L(3, 1, 0), L(3, 1, 22), L(3, 1, 28), L(3, 1, 36)};
    st.tip = 1.f; st.taperStart = 0.04f; st.taperEnd = 0.12f; st.tipMin = 0.85f;
    st.bristles = 22.f; st.bristleContrast = 0.2f; st.dryout = 0.3f; st.streakNoise = 0.25f;
    st.edgeRough = 0.1f; st.endJag = 0.6f; st.impasto = 0.32f; st.ridge = 0.45f;
    st.groove = 0.25f; st.pickup = 0.08f;
    st.fieldMode = 2.f; st.patchCell = 30.f * s; st.patchJitter = 0.22f; st.patchQuant = 0.26f;
    st.fallbackAngle = -55.f; st.coherenceMin = 0.06f; st.minFieldMag = 0.05f;
    palette(st, {"#f1e8d0", "#e4c98a", "#d49a4a", "#b86a3a", "#8a4a2a", "#5a7a4a", "#3d6a52",
                 "#7a9a6a", "#6a7ab0", "#8a86b8", "#3d4a7a", "#a8b8c0"});
    st.palettePull = 0.28f; st.labJitterL = 6.f; st.labJitterAB = 6.f;
    st.warmCool = 0.18f; st.saturation = 1.1f;
    ground(st, "#ede3cc"); st.underOpacity = 0.55f; st.support = 1.f; st.weave = 0.3f;
    st.impastoLight = 0.5f; st.reliefScale = 1.8f; st.specular = 0.1f; st.varnish = 0.06f;
    st.contour = 0.75f; contourColor(st, "#3a4880"); st.contourWidth = 1.7f * s;
    st.contourThreshold = 0.3f; st.contourSigma = 2.5f * s; st.contourWobble = 1.2f * s;
}

// romanticism.turner
void turner(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    // layers: translucent scumble veils / dragged dry brush / lead-white impasto / incident
    radii(cfg, s, {22.f, 9.f, 8.f, 2.f});
    p.curvature = 0.8f;
    p.tensorSigma = 4.f * s;
    cfg.layerSpecs = {L(6, 3, 0, 0.8f, 0.45f, 1.0f), L(14, 5, 12, 0.8f, 0.8f, 1.1f),
                      L(18, 6, 0, 0.75f, 0.8f, 1.1f), L(5, 2, 30, 1.0f, 0.9f, 1.5f)};
    st.tip = 0.f; st.taperStart = 0.1f; st.taperEnd = 0.3f; st.tipMin = 0.45f;
    st.bristles = 30.f; st.bristleClump = 0.5f; st.bristleContrast = 0.3f; st.bristleAlpha = 0.5f;
    st.streakNoise = 0.35f; st.edgeRough = 0.3f; st.edgeSoft = 2.f; st.endJag = 0.8f;
    st.impasto = 0.5f; st.ridge = 0.2f; st.groove = 1.1f; st.groovePx = 1.5f; st.startBlob = 0.25f;
    st.pickup = 0.25f; st.load = 0.9f;
    layerA(st, 0, 0.15f, 0.3f, 3.f, 0.f);   // scumble: soft, thin
    layerA(st, 1, 0.5f, 0.65f, 1.f, 0.f);   // dry brush runs out
    layerB(st, 1, 1.f, 1.f, 0.f, 0.75f);    //   and skips the weave
    layerA(st, 2, 1.1f, 0.45f, 1.f, 0.55f); // lead white: thick, lights only
    layerB(st, 2, 1.f, 1.06f, 0.f, 0.2f);
    layerA(st, 3, 0.6f, 0.f, 0.8f, 0.f);
    st.fieldMode = 1.f; st.vortexX = 0.47f; st.vortexY = 0.43f; st.vortexSpiral = 0.62f;
    st.vortexRadius = 0.38f; st.autoVortex = 1.f; st.perturb = 0.45f; st.perturbScale = 130.f * s;
    st.fallbackAngle = 0.f; st.coherenceMin = 0.1f; st.minFieldMag = 0.05f;
    palette(st, {"#f6efdc", "#ebd9a6", "#d9a954", "#b8732f", "#7a4a2a", "#3d3a36", "#5f6b66",
                 "#2f4a4a", "#1f2a2a", "#8a8f86", "#c7c2b0"});
    st.palettePull = 0.12f; st.labJitterL = 3.f; st.labJitterAB = 2.f;
    ground(st, "#dcc9a2"); st.underOpacity = 0.95f; st.support = 1.f; st.weave = 0.2f;
    st.licStrength = 0.5f; st.licLength = 26.f * s;
    st.impastoLight = 1.f; st.reliefScale = 1.6f; st.specular = 0.2f; st.varnish = 0.18f;
    st.vignette = 0.25f;
    st.rain = 1.f; st.rainAngle = 70.f; st.rainLength = 180.f * s; st.rainDensity = 1.f / s;
    st.rainAlpha = 0.18f;
}

// post_impressionism.vangogh
void vangogh(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {5.5f, 4.f, 2.5f});
    p.curvature = 0.7f;
    p.tensorSigma = 3.f * s;
    p.gridFactor = 1.25f;
    cfg.layerSpecs = {L(6, 3, 0), L(6, 3, 24), L(5, 2, 32)};
    st.tip = 0.f; st.taperStart = 0.08f; st.taperEnd = 0.3f; st.tipMin = 0.45f;
    st.bristles = 18.f; st.bristleClump = 0.25f; st.bristleContrast = 0.35f;
    st.impasto = 1.f; st.ridge = 0.6f; st.startBlob = 0.9f; st.pickup = 0.15f; st.groove = 0.6f;
    st.fieldMode = 3.f; st.curlScale = 110.f * s; st.fieldMix = 1.f;
    st.fallbackMode = 1.f; st.coherenceMin = 0.12f; st.minFieldMag = 0.05f;
    palette(st, {"#fbf1c9", "#f7d53a", "#eab22a", "#d9772a", "#b8452a", "#1f3a8a", "#2f5cb8",
                 "#5a8ad0", "#8fc0e0", "#2e7a4a", "#6aa84a", "#a8c860", "#15203a"});
    st.palettePull = 0.4f; st.labJitterL = 8.f; st.labJitterAB = 7.f;
    st.saturation = 1.35f; st.contrast = 1.08f;
    ground(st, "#ede3cc"); st.underOpacity = 0.7f; st.support = 3.f; st.weave = 0.12f;
    st.contour = 0.85f; contourColor(st, "#1c2a5a"); st.contourWidth = 2.4f * s;
    st.contourThreshold = 0.5f; st.contourSigma = 1.5f * s; st.contourWobble = 1.5f * s;
    st.impastoLight = 1.0f; st.reliefScale = 1.9f; st.specular = 0.22f; st.shininess = 18.f;
}

// impressionism.monet
void monet(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {8.5f, 5.f, 2.5f});
    p.curvature = 0.6f;
    p.tensorSigma = 3.f * s;
    p.gridFactor = 1.3f;
    cfg.layerSpecs = {L(4, 2, 0), L(4, 2, 22), L(4, 2, 30)};
    st.tip = 2.f; st.taperStart = 0.05f; st.taperEnd = 0.45f; st.tipMin = 0.25f;
    st.bristles = 16.f; st.bristleContrast = 0.25f; st.impasto = 0.55f; st.ridge = 0.4f;
    st.startBlob = 0.8f; st.pickup = 0.18f;
    st.fieldMode = 0.f; st.fallbackMode = 2.f; st.coherenceMin = 0.1f;
    st.perturb = 0.3f; st.perturbScale = 50.f * s; st.minFieldMag = 0.05f;
    palette(st, {"#fbf6ea", "#f7de5a", "#f2a93a", "#e5552f", "#c8324b", "#9a4fa8", "#5a5fc8",
                 "#2f64b8", "#3aa0c8", "#2f9a6a", "#7ac35a", "#c9e07a"});
    st.palettePull = 0.3f; st.saturation = 1.3f; st.lift = 0.08f; st.warmCool = 0.4f;
    st.labJitterL = 5.f; st.labJitterAB = 11.f; st.fleckProb = 0.04f;
    ground(st, "#ede3cc"); st.underOpacity = 0.45f; st.support = 1.f; st.weave = 0.25f;
    st.impastoLight = 0.75f; st.specular = 0.18f;
}

// impressionism.seurat
void seurat(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {2.2f, 1.8f});
    p.curvature = 0.f;
    p.gridFactor = 1.7f;
    p.sizeJitter = 0.2f;
    p.opacity = 0.95f;
    cfg.layerSpecs = {L(1, 1, 0), L(1, 1, 0, NAN, NAN, 2.0f)};
    st.tip = 0.f; st.taperStart = 0.f; st.taperEnd = 0.f; st.tipMin = 1.f; st.bristles = 0.f;
    st.edgeRough = 0.18f; st.edgeSoft = 0.7f; st.impasto = 0.35f; st.ridge = 0.1f;
    st.startBlob = 0.3f; st.streakNoise = 0.f; st.groove = 0.f; st.grooveColor = 0.f;
    st.widthScale = 1.1f;
    palette(st, {"#fdf8ec", "#fbe34a", "#f7a531", "#ea5a2c", "#d63a5a", "#9b4fb0", "#4f5ed0",
                 "#2e82d0", "#3fb0c0", "#3a9e5a", "#86c653", "#e7d9b8"});
    st.optical = 1.f; st.saturation = 1.15f;
    ground(st, "#f4f0e6"); st.underOpacity = 0.35f; st.support = 1.f; st.weave = 0.2f;
    st.impastoLight = 0.55f; st.specular = 0.12f;
}

// baroque.chiaroscuro
void chiaroscuro(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    // layers: thin filbert modelling / loaded lights / thin detail / accents
    radii(cfg, s, {7.f, 5.f, 3.5f, 1.75f});
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(4, 2, 0), L(6, 2, 0, 0.8f, 0.9f, 1.4f), L(4, 2, 16), L(4, 1, 30)};
    st.tip = 2.f; st.taperStart = 0.08f; st.taperEnd = 0.25f; st.tipMin = 0.6f;
    st.bristles = 20.f; st.bristleContrast = 0.22f; st.pickup = 0.2f;
    st.impasto = 0.08f; st.ridge = 0.1f; st.groove = 0.1f; st.startBlob = 0.f;
    layerA(st, 1, 11.f, 0.35f, 1.f, 0.42f);  // Rembrandt: thick paint only in the light
    layerB(st, 1, 1.1f, 1.06f, 0.f, 0.f);
    st.fieldMode = 0.f; st.fallbackMode = 1.f; st.curlScale = 120.f * s; st.coherenceMin = 0.08f;
    palette(st, {"#f5ecd6", "#e8c98e", "#c79a55", "#9c5a2c", "#6b3a1f", "#3f2414", "#231510",
                 "#0f0a08", "#a3281c", "#7a1f1a", "#4a4a3a"});
    st.palettePull = 0.25f; st.saturation = 0.9f; st.tenebrism = 1.8f;
    st.labJitterL = 3.f; st.labJitterAB = 2.f;
    ground(st, "#4a3526"); st.underOpacity = 0.75f; st.support = 1.f; st.weave = 0.2f;
    st.impastoLight = 1.f; st.reliefScale = 1.35f; st.specular = 0.16f; st.varnish = 0.4f;
    st.crackle = 0.4f; st.crackleSize = 26.f * s; st.vignette = 0.5f;
}

// baroque.dutch (Vermeer)
void dutch(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {5.f, 2.f, 1.3f});
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(5, 2, 0, NAN, 0.5f), L(5, 2, 14, NAN, 0.8f), L(1, 1, 0, 0.f, 0.9f, 5.f)};
    st.tip = 2.f; st.bristles = 20.f; st.bristleContrast = 0.08f; st.groove = 0.12f;
    st.impasto = 0.25f; st.edgeSoft = 1.5f;
    layerA(st, 2, 2.8f, 0.f, 0.6f, 0.75f);   // pointille: raised dots of light
    layerB(st, 2, 1.f, 1.7f, 0.f, 0.f);
    st.fallbackAngle = 17.f; st.coherenceMin = 0.08f;
    palette(st, {"#f4efe2", "#e9d9a7", "#d8b55a", "#2f4f8c", "#6a86b8", "#8a6a45", "#4a3a2a",
                 "#1a1712", "#b23a2a", "#7c8a73", "#c9c2b0"});
    st.palettePull = 0.25f; st.saturation = 0.95f; st.warmCool = 0.12f;
    ground(st, "#9c978b"); st.underOpacity = 0.95f; st.support = 2.f; st.weave = 0.12f;
    st.impastoLight = 0.5f; st.specular = 0.15f; st.varnish = 0.2f; st.crackle = 0.25f;
    st.crackleSize = 22.f * s; st.vignette = 0.22f;
}

// baroque.rococo
void rococo(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {12.f, 4.f, 2.5f});
    p.curvature = 0.9f;
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(4, 2, 0, NAN, 0.4f), L(8, 3, 18, NAN, 0.75f), L(6, 2, 26, NAN, 0.75f)};
    st.tip = 2.f; st.taperStart = 0.05f; st.taperEnd = 0.75f; st.tipMin = 0.08f;
    st.bristles = 16.f; st.bristleContrast = 0.25f; st.impasto = 0.35f; st.pickup = 0.15f;
    st.startBlob = 0.8f;
    layerA(st, 0, 0.3f, 0.3f, 3.f, 0.65f);   // scumbled clouds in the lights
    st.fallbackMode = 1.f; st.curlScale = 80.f * s; st.perturb = 0.3f; st.perturbScale = 60.f * s;
    palette(st, {"#fbf3ea", "#f6d8d0", "#eab4b4", "#d98a94", "#bcd4e6", "#8fb3d1", "#e9e0b8",
                 "#b8c9a0", "#8aa07a", "#c8a57a", "#6d6a7a"});
    st.palettePull = 0.4f; st.saturation = 0.85f; st.lift = 0.12f; st.warmCool = 0.1f;
    st.labJitterL = 5.f; st.labJitterAB = 4.f;
    ground(st, "#e9d6cc"); st.underOpacity = 0.8f; st.support = 1.f; st.weave = 0.2f;
    st.impastoLight = 0.45f; st.varnish = 0.12f; st.crackle = 0.15f; st.crackleSize = 30.f * s;
}

// renaissance.sfumato
void sfumato(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {13.f, 3.5f});
    p.tensorSigma = 4.f * s;
    cfg.layerSpecs = {L(4, 2, 0, NAN, 0.14f), L(4, 2, 9, NAN, 0.5f)};
    st.tip = 0.f; st.bristles = 0.f; st.edgeSoft = 6.f * s; st.edgeRough = 0.2f;
    st.impasto = 0.f; st.streakNoise = 0.1f; st.flatten = 0.1f; st.groove = 0.f; st.grooveColor = 0.f;
    layerA(st, 1, 1.f, 0.f, 0.4f, 0.f);
    st.fallbackMode = 1.f; st.curlScale = 150.f * s;
    palette(st, {"#f3ead7", "#e2c28a", "#c9955a", "#a4633a", "#6e3b22", "#3a241a", "#9a2e22",
                 "#c44a32", "#2f4d7a", "#5a7a52", "#7f8a5a", "#1d1712"});
    st.palettePull = 0.3f; st.saturation = 0.72f; st.contrast = 1.08f; st.tenebrism = 1.4f;
    ground(st, "#dcc9a2"); st.underOpacity = 0.97f; st.support = 4.f; st.weave = 0.04f;
    st.impastoLight = 0.05f; st.varnish = 0.45f; st.varnishR = 1.f; st.varnishG = 0.9f;
    st.varnishB = 0.68f; st.crackle = 0.3f; st.crackleSize = 16.f * s; st.vignette = 0.38f;
    st.grain = 0.008f;
}

// medieval.tempera
void tempera(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    st.groundMode = 2.f;                     // flat local colour first
    radii(cfg, s, {1.2f, 1.f});
    p.curvature = 0.5f;
    p.opacity = 0.7f;
    p.gridFactor = 2.6f;
    cfg.layerSpecs = {L(10, 4, 0), L(10, 4, 0)};
    st.tip = 3.f; st.taperStart = 0.2f; st.taperEnd = 0.3f; st.tipMin = 0.2f; st.bristles = 0.f;
    st.edgeSoft = 0.6f; st.edgeRough = 0.05f; st.impasto = 0.02f; st.ridge = 0.f;
    st.startBlob = 0.f; st.flatten = 0.f; st.groove = 0.f; st.grooveColor = 0.f;
    st.widthScale = 0.8f;
    layerB(st, 0, 1.f, 0.8f, 0.f, 0.f);      // dark hatches model the shadows
    layerB(st, 1, 1.f, 1.18f, 0.f, 0.f);     // light hatches build the lights
    st.fallbackAngle = 45.f; st.perturb = 0.15f; st.perturbScale = 60.f * s;
    palette(st, {"#f3ead7", "#e2c28a", "#c9955a", "#a4633a", "#6e3b22", "#3a241a", "#9a2e22",
                 "#c44a32", "#2f4d7a", "#5a7a52", "#7f8a5a", "#1d1712"});
    st.palettePull = 0.3f; st.posterize = 7.f; st.kuwahara = 3.f * s; st.value = 1.05f;
    st.saturation = 0.9f; st.labJitterL = 2.f;
    ground(st, "#f4f0e6"); st.support = 4.f; st.weave = 0.1f;
    st.contour = 0.8f; contourColor(st, "#5a3a26"); st.contourWidth = 1.1f * s;
    st.contourThreshold = 0.3f; st.contourWobble = 0.5f * s;
    st.impastoLight = 0.08f; st.crackle = 0.35f; st.crackleSize = 20.f * s; st.varnish = 0.2f;
    st.vignette = 0.15f;
}

// asian.ukiyoe
void ukiyoe(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    flatBase(p, cfg, st);
    palette(st, {"#f3e9d2", "#e6d3a8", "#1d3f6e", "#3f6fa0", "#7fa4c4", "#c83a2e", "#e8876a",
                 "#e5b93c", "#5f7f4a", "#8aa06a", "#1a1a1a", "#5a4a3a"});
    st.saturation = 1.6f; st.warmCool = 0.35f;  // shadows toward indigo, lights toward paper
    st.kuwahara = 5.f * s; st.woodgrain = 0.05f; st.flatMottle = 0.03f;
    st.outline = 0.92f; st.outlineWidth = 1.6f * s; st.outlineContrast = 0.12f;
    st.bokashi = 1.f;
    ground(st, "#efe6d2"); st.support = 6.f; st.weave = 0.18f; st.reliefScale = 1.2f;
    st.varnish = 0.1f; st.varnishR = 1.f; st.varnishG = 0.95f; st.varnishB = 0.85f; st.grain = 0.008f;
}

// asian.sumie
void sumie(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {13.f, 6.f, 3.f});
    p.curvature = 0.7f;
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(8, 3, 0, NAN, 0.85f), L(10, 3, 20), L(2, 1, 30)};
    st.tip = 3.f; st.taperStart = 0.06f; st.taperEnd = 0.5f; st.tipMin = 0.08f; st.swell = 0.4f;
    st.bristles = 40.f; st.bristleContrast = 0.15f; st.bristleClump = 0.55f; st.bristleAlpha = 0.4f;
    st.load = 0.95f; st.dryout = 0.6f; st.dryTooth = 0.55f; st.streakNoise = 0.3f;
    st.edgeRough = 0.18f; st.edgeSoft = 1.2f; st.endJag = 0.9f;
    st.impasto = 0.f; st.ridge = 0.f; st.startBlob = 0.f; st.flatten = 0.f; st.groove = 0.f;
    layerB(st, 1, 1.f, 0.6f, 0.f, 0.f);      // darker ink as the painting firms up
    layerB(st, 2, 1.f, 0.25f, 0.f, 0.f);     // accent dots in near-black ink
    st.fallbackAngle = -78.f; st.coherenceMin = 0.08f;
    st.ink = 0.45f; st.valueGate = 0.55f;    // the light half stays empty paper
    ground(st, "#f3eee2"); st.underOpacity = 0.f; st.support = 6.f; st.weave = 0.15f;
    st.contour = 0.9f; contourColor(st, "#141312"); st.contourWidth = 3.f * s;
    st.contourThreshold = 0.4f; st.contourWobble = 0.8f * s;
    st.impastoLight = 0.f; st.specular = 0.f; st.reliefScale = 1.f; st.varnish = 0.08f;
    st.varnishR = 1.f; st.varnishG = 0.95f; st.varnishB = 0.85f; st.grain = 0.008f;
}

// techniques.watercolor
void watercolor(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    st.strokesOff = 1.f; st.brushModel = 0.f; st.groundMode = 0.f;
    st.watercolor = 1.f; st.kuwahara = 3.f * s; st.edgeDarken = 0.9f; st.granulation = 0.4f;
    ground(st, "#f7f3ea"); st.underOpacity = 0.f; st.support = 5.f; st.supportScale = s;
    st.weave = 0.16f; st.reliefScale = 1.2f; st.impastoLight = 0.f; st.specular = 0.f;
    st.contour = 0.22f; contourColor(st, "#5c5c66"); st.contourWidth = 0.9f * s;
    st.contourThreshold = 0.35f; st.contourWobble = 0.6f * s; st.grain = 0.006f;
}

// modern.fauve
void fauve(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {10.f, 5.5f});
    p.curvature = 0.5f;
    p.gridFactor = 1.5f;                      // gaps on purpose: the white ground breathes
    p.opacity = 0.95f;
    cfg.layerSpecs = {L(4, 2, 0), L(4, 2, 33)};
    st.tip = 1.f; st.taperStart = 0.04f; st.taperEnd = 0.12f; st.tipMin = 0.85f;
    st.bristles = 22.f; st.bristleContrast = 0.25f; st.impasto = 0.4f; st.endJag = 0.55f;
    st.fallbackMode = 1.f; st.curlScale = 120.f * s;
    palette(st, {"#ffe14a", "#ff9a1f", "#ff4a2a", "#e0205a", "#b02ab0", "#4a3ae0", "#1f8ae0",
                 "#1fc0a0", "#2aa82a", "#9ae02a", "#fff4e0", "#1a1a3a"});
    st.fauve = 1.f; st.palettePull = 0.5f; st.labJitterL = 7.f; st.labJitterAB = 10.f;
    ground(st, "#f4f0e6"); st.underOpacity = 0.3f; st.support = 1.f; st.weave = 0.35f;
    st.contour = 0.9f; contourColor(st, "#2440b0"); st.contourWidth = 3.f * s;
    st.contourThreshold = 0.45f; st.contourSigma = 2.f * s; st.contourWobble = 1.5f * s;
    st.impastoLight = 0.45f; st.specular = 0.12f;
}

// modern.expressionist
void expressionist(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {6.5f, 3.5f});
    p.curvature = 0.9f;
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(20, 6, 0), L(16, 4, 24)};
    st.tip = 0.f; st.taperStart = 0.08f; st.taperEnd = 0.2f; st.tipMin = 0.5f;
    st.bristles = 14.f; st.bristleContrast = 0.4f; st.dryout = 0.35f; st.dryTooth = 0.35f;
    st.impasto = 0.35f;
    st.fieldMode = 4.f; st.waveLen = 140.f * s; st.waveAmp = 0.9f; st.waveAngle = -0.15f;
    st.perturb = 0.4f; st.perturbScale = 100.f * s; st.coherenceMin = 0.15f; st.minFieldMag = 0.05f;
    palette(st, {"#f4e04a", "#f08a1f", "#d8321f", "#8a1f3a", "#3a2a6a", "#1f5ab0", "#2a8a6a",
                 "#8ab03a", "#f0e8d0", "#141418"});
    st.palettePull = 0.45f; st.saturation = 1.45f; st.contrast = 1.15f;
    st.labJitterL = 9.f; st.labJitterAB = 11.f;
    ground(st, "#ede3cc"); st.underOpacity = 0.5f; st.support = 3.f; st.weave = 0.35f;
    st.licStrength = 0.2f; st.licLength = 10.f * s;
    st.contour = 0.9f; contourColor(st, "#141418"); st.contourWidth = 3.4f * s;
    st.contourThreshold = 0.4f; st.contourWobble = 2.f * s;
    st.impastoLight = 0.45f; st.specular = 0.1f;
}

// modern.cubism
void cubism(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {3.5f, 2.f});
    p.curvature = 0.f;
    p.gridFactor = 1.4f;
    cfg.layerSpecs = {L(3, 1, 0), L(3, 1, 24)};
    st.tip = 1.f; st.taperStart = 0.04f; st.taperEnd = 0.12f; st.tipMin = 0.85f;
    st.bristles = 20.f; st.bristleContrast = 0.25f; st.impasto = 0.3f;
    st.fieldMode = 2.f; st.patchCell = 45.f * s; st.patchJitter = 1.57f; st.patchQuant = 0.f;
    palette(st, {"#e9dcc0", "#cbb68a", "#a88c5a", "#7a6444", "#4f4232", "#2b241c", "#8a8a7a",
                 "#5f6658", "#b0a890", "#3a3a36"});
    st.palettePull = 0.6f; st.saturation = 0.5f; st.labJitterL = 5.f; st.labJitterAB = 2.f;
    ground(st, "#dcc9a2"); st.underOpacity = 0.9f; st.support = 1.f; st.weave = 0.3f;
    st.facet = 1.f; st.facetCell = 45.f * s;
    st.contour = 0.6f; contourColor(st, "#2b241c"); st.contourWidth = 1.6f * s; st.contourThreshold = 0.35f;
    st.impastoLight = 0.35f; st.varnish = 0.15f; st.specular = 0.1f;
}

// contemporary.pop
void pop(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    flatBase(p, cfg, st);
    palette(st, {"#ffffff", "#ffe600", "#e8231e", "#1f4fd0", "#000000", "#f7b9a8"});
    st.saturation = 1.6f; st.kuwahara = 5.f * s; st.flatMottle = 0.01f;
    st.halftone = 1.f; st.halftoneSpacing = 7.f * s;
    st.outline = 1.f; st.outlineWidth = 3.f * s; st.outlineContrast = 0.08f;
    st.contour = 1.f; contourColor(st, "#101010"); st.contourWidth = 3.5f * s;
    st.contourThreshold = 0.45f; st.contourWobble = 0.4f * s;
    ground(st, "#fbfaf6"); st.support = 5.f; st.weave = 0.06f; st.grain = 0.006f;
}

// contemporary.hockney
void hockney(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    flatBase(p, cfg, st);
    palette(st, {"#f6f1e7", "#3aa0e8", "#62c6e8", "#a8e2ee", "#e9c7c0", "#f4d9cf", "#4f9a3a",
                 "#7ab84a", "#e6c46a", "#b8b8b0", "#2a4a6a", "#1a2a3a"});
    st.posterize = 7.f; st.palettePull = 0.35f; st.saturation = 1.15f;
    st.kuwahara = 4.f * s; st.flatMottle = 0.015f;
    st.caustics = 1.f; st.causticScale = 40.f * s;
    st.contour = 0.5f; contourColor(st, "#2a3a4a"); st.contourWidth = 1.4f * s;
    st.contourThreshold = 0.45f; st.contourWobble = 0.3f * s;
    st.border = 0.045f;
    ground(st, "#f1ece2"); st.support = 1.f; st.supportScale = 0.8f; st.weave = 0.14f;
    st.impastoLight = 0.1f; st.specular = 0.05f;
}

// contemporary.folk
void folk(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    st.groundMode = 2.f;
    radii(cfg, s, {1.5f});
    p.curvature = 0.f;
    cfg.layerSpecs = {L(1, 1, 20, NAN, 0.85f, 2.6f)};
    st.tip = 0.f; st.taperStart = 0.f; st.taperEnd = 0.f; st.tipMin = 1.f; st.bristles = 0.f;
    st.edgeRough = 0.18f; st.impasto = 0.2f; st.streakNoise = 0.f; st.groove = 0.f;
    palette(st, {"#f6f4f0", "#dfe4e6", "#b8c0c4", "#6b7a78", "#4a5a4a", "#8a3a2a", "#b85a3a",
                 "#e6c89a", "#3a3a3a", "#1a1a1a", "#7a8a6a"});
    st.posterize = 8.f; st.palettePull = 0.3f; st.saturation = 1.1f; st.kuwahara = 3.f * s;
    st.flatMottle = 0.06f; st.labJitterL = 8.f; st.labJitterAB = 5.f;
    ground(st, "#f4f0e6"); st.support = 4.f; st.weave = 0.05f;
    st.contour = 0.85f; contourColor(st, "#3a3028"); st.contourWidth = 1.3f * s; st.contourThreshold = 0.4f;
    st.impastoLight = 0.15f; st.varnish = 0.12f; st.specular = 0.08f;
}

// techniques.knife
void knife(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {12.f, 7.f, 4.f});
    p.curvature = 0.f;
    p.gridFactor = 1.35f;
    cfg.layerSpecs = {L(3, 2, 0), L(3, 2, 22), L(3, 2, 32)};
    st.tip = 4.f; st.taperStart = 0.f; st.taperEnd = 0.1f; st.tipMin = 0.9f; st.bristles = 0.f;
    st.bristleContrast = 0.f; st.edgeRough = 0.05f; st.edgeSoft = 0.6f; st.impasto = 1.f;
    st.ridge = 0.8f; st.startBlob = 0.2f; st.flatten = 1.f; st.smear = 0.25f; st.smearLen = 0.8f;
    st.streakNoise = 0.25f; st.streakLen = 1.f; st.dryTooth = 0.25f; st.load = 0.9f; st.dryout = 0.4f;
    st.groove = 0.1f;
    st.fieldMode = 2.f; st.patchCell = 40.f * s; st.patchJitter = 0.5f; st.patchQuant = 0.f;
    st.fallbackAngle = 11.f;
    st.saturation = 1.15f; st.labJitterL = 6.f; st.labJitterAB = 4.f;
    ground(st, "#f4f0e6"); st.underOpacity = 0.8f; st.support = 1.f; st.weave = 0.1f;
    st.impastoLight = 1.f; st.reliefScale = 2.2f; st.specular = 0.3f; st.shininess = 26.f;
}

// techniques.alla_prima
void allaPrima(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {11.f, 6.f, 3.f, 1.5f});
    p.curvature = 0.4f;
    p.tensorSigma = 3.f * s;
    cfg.layerSpecs = {L(6, 3, 0), L(6, 2, 20), L(3, 1, 0, 0.5f, NAN, 1.6f), L(4, 1, 32)};
    st.tip = 1.f; st.taperStart = 0.04f; st.taperEnd = 0.12f; st.tipMin = 0.85f;
    st.bristles = 22.f; st.bristleContrast = 0.28f; st.pickup = 0.25f; st.impasto = 0.6f;
    st.dryout = 0.25f; st.endJag = 0.55f;
    layerA(st, 2, 2.f, 0.f, 1.f, 0.75f);     // crisp loaded highlights put down last
    layerB(st, 2, 1.f, 1.05f, 0.f, 0.f);
    st.fallbackAngle = 23.f; st.perturb = 0.15f; st.perturbScale = 80.f * s;
    st.saturation = 1.05f; st.labJitterL = 3.f; st.labJitterAB = 2.f;
    ground(st, "#9c978b"); st.underOpacity = 0.6f; st.support = 2.f; st.weave = 0.2f;
    st.impastoLight = 0.85f; st.reliefScale = 1.8f; st.specular = 0.22f;
}

// techniques.pastel
void pastel(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {4.5f, 3.f, 1.75f});
    p.curvature = 0.5f;
    p.tensorSigma = 3.f * s;
    p.opacity = 0.95f;
    cfg.layerSpecs = {L(6, 3, 0), L(6, 2, 20), L(6, 2, 28)};
    st.tip = 0.f; st.taperStart = 0.1f; st.taperEnd = 0.2f; st.tipMin = 0.6f;
    st.bristles = 6.f; st.bristleAlpha = 0.2f; st.bristleContrast = 0.1f; st.load = 0.62f;
    st.dryout = 0.12f; st.dryTooth = 1.f; st.impasto = 0.02f; st.ridge = 0.f; st.groove = 0.f;
    st.streakNoise = 0.3f; st.edgeRough = 0.2f; st.startBlob = 0.f;
    layerB(st, 1, 1.f, 1.f, 0.9f, 0.f);      // second layer cross-hatches
    layerB(st, 2, 1.f, 1.f, 0.f, 0.f);
    st.fallbackAngle = -40.f;
    st.saturation = 1.15f; st.labJitterL = 5.f; st.labJitterAB = 5.f;
    ground(st, "#c7b597"); st.underOpacity = 0.f; st.support = 5.f; st.supportScale = 1.2f * s;
    st.weave = 0.5f; st.reliefScale = 1.3f;
    st.contour = 0.85f; contourColor(st, "#292623"); st.contourWidth = 1.8f * s; st.contourThreshold = 0.45f;
    st.impastoLight = 0.f; st.specular = 0.f; st.grain = 0.01f;
}

// abstract.gestural
void gestural(float s, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    base(p, cfg, st);
    radii(cfg, s, {22.f, 13.f});
    p.curvature = 0.95f;
    p.tensorSigma = 5.f * s;
    p.opacity = 0.92f;
    cfg.layerSpecs = {L(8, 4, 0, NAN, NAN, 1.4f), L(9, 4, 30, NAN, NAN, 1.4f)};
    st.tip = 1.f; st.taperStart = 0.04f; st.taperEnd = 0.12f; st.tipMin = 0.85f;
    st.bristles = 30.f; st.bristleContrast = 0.3f; st.pickup = 0.35f; st.smear = 0.35f;
    st.smearLen = 1.f; st.impasto = 0.7f; st.dryout = 0.35f;
    st.fieldMode = 3.f; st.fieldMix = 0.9f; st.curlScale = 200.f * s; st.fallbackMode = 1.f;
    st.saturation = 1.3f; st.labJitterL = 10.f; st.labJitterAB = 8.f;
    ground(st, "#f4f0e6"); st.underOpacity = 0.15f; st.support = 1.f; st.weave = 0.15f;
    st.contour = 0.95f; contourColor(st, "#121212"); st.contourWidth = 5.f * s;
    st.contourThreshold = 0.55f; st.contourSigma = 3.f * s; st.contourWobble = 3.f * s;
    st.impastoLight = 0.9f; st.reliefScale = 2.f; st.specular = 0.25f;
}

using ApplyFn = void (*)(float, TuningParams&, RenderConfig&, StyleParams&);

struct Entry { Info info; ApplyFn fn; };

const std::vector<Entry>& entries() {
    static const std::vector<Entry> e = {
        {{"none", "Classic tile brush", "-", "-", "PainterlyImageCreatorWeb",
          "Hertzmann strokes with web-inspired brush tiles and presets."}, nullptr},
        {{"tempera", "Tempera hatch", "Early Renaissance", "1300-1500",
          "Fra Angelico, Botticelli", "Flat colour, then fine value hatches over a gesso panel."}, tempera},
        {{"sfumato", "Soft glaze", "High Renaissance", "1480-1520", "Leonardo da Vinci",
          "Smoky glazes over a warm panel, deepened shadows, yellowed varnish."}, sfumato},
        {{"chiaroscuro", "Loaded impasto", "Baroque", "1600-1669", "Caravaggio, Rembrandt",
          "Dark umber ground, tenebrist values, loaded impasto only in the light."}, chiaroscuro},
        {{"dutch", "Fine soft brush", "Dutch Golden Age", "1650-1675", "Johannes Vermeer",
          "Fused soft paint, cool light, raised pointille dots on highlights."}, dutch},
        {{"rococo", "Feathered flick", "Rococo", "1730-1780", "Fragonard, Boucher",
          "Pastel palette, feathery flicked commas, scumbled clouds."}, rococo},
        {{"turner", "Dry scumble", "Romanticism", "1830-1845", "J. M. W. Turner",
          "Everything swept around the light: scumbles, dry brush, lead-white impasto, smear, rain."}, turner},
        {{"ukiyoe", "Woodblock edge", "Edo Japan", "1760-1860", "Hokusai, Hiroshige",
          "Flat printed blocks, key lines, bokashi sky, wood grain on washi."}, ukiyoe},
        {{"sumie", "Dry ink", "East Asian ink", "Song - Edo", "Muqi, Sesshu",
          "Graded ink, flying-white dry brush, empty paper for the lights."}, sumie},
        {{"watercolor", "Watercolor edge", "English watercolour", "1790-1910",
          "Turner, Sargent, Homer", "Beer-Lambert washes, dark drying rims, granulation, pencil."}, watercolor},
        {{"monet", "Comma dab", "Impressionism", "1869-1900", "Claude Monet",
          "Comma strokes of unmixed colour, violet shadows, horizontal water."}, monet},
        {{"seurat", "Pointillist dot", "Neo-Impressionism", "1884-1891", "Georges Seurat",
          "Dots of pure pigment chosen by optical mixing."}, seurat},
        {{"cezanne", "Flat hatch", "Post-Impressionism", "1880-1906", "Paul Cezanne",
          "Flat parallel hatches patch by patch, broken colour, blue contours."}, cezanne},
        {{"vangogh", "Rhythmic ridge", "Post-Impressionism", "1886-1890", "Vincent van Gogh",
          "Loaded dashes in flowing rows, turbulent skies, dark outlines."}, vangogh},
        {{"fauve", "Broad flat brush", "Fauvism", "1905-1908", "Matisse, Derain",
          "Arbitrary saturated colour in broad strokes, white ground breathing."}, fauve},
        {{"expressionist", "Jagged drag", "Expressionism", "1893-1925", "Edvard Munch",
          "Undulating strokes, acid colour, smeared bands, heavy contours."}, expressionist},
        {{"cubism", "Angular hatch", "Cubism", "1909-1912", "Picasso, Braque",
          "Faceted planes lit from their own sides, ochre-grey hatching."}, cubism},
        {{"gestural", "Wet sweep", "Abstract Expressionism", "1948-1960", "de Kooning",
          "Huge wet sweeps dragged through each other, black slashes."}, gestural},
        {{"pop", "Smooth print brush", "Pop Art", "1961-1970", "Roy Lichtenstein",
          "Printing primaries, Ben-Day dots, heavy black key lines."}, pop},
        {{"hockney", "Acrylic flat", "Pop / Contemporary", "1964-1972", "David Hockney",
          "Flat acrylic, pool caustic lines, unpainted border."}, hockney},
        {{"folk", "Fine stipple", "American Folk", "1938-1961", "Grandma Moses",
          "Flat local colour on board, stippled detail, fine dark lines."}, folk},
        {{"knife", "Palette knife", "Contemporary", "1950-today", "de Stael",
          "Planar knife slabs dragged through wet paint, raking light."}, knife},
        {{"alla_prima", "Loaded flat", "Belle Epoque", "1880-1925", "Sargent, Sorolla",
          "Long confident flat strokes on a toned ground, loaded highlights last."}, allaPrima},
        {{"pastel", "Dry pastel", "Impressionist pastel", "1870-1900", "Degas, Cassatt",
          "Chalk catching only the paper's tooth, hatched and cross-hatched."}, pastel},
    };
    return e;
}

} // namespace

const std::vector<Info>& list() {
    static const std::vector<Info> v = [] {
        std::vector<Info> out;
        for (const Entry& e : entries()) out.push_back(e.info);
        return out;
    }();
    return v;
}

int indexOf(const std::string& key) {
    const auto& e = entries();
    for (size_t i = 0; i < e.size(); ++i)
        if (key == e[i].info.key) return int(i);
    return -1;
}

void clear(StyleParams& st, RenderConfig& cfg) {
    st = StyleParams{};
    cfg.layerSpecs.clear();
}

void apply(int index, float scale, TuningParams& p, RenderConfig& cfg, StyleParams& st) {
    const auto& e = entries();
    if (index < 0 || index >= int(e.size()) || !e[size_t(index)].fn) {
        clear(st, cfg);
        return;
    }
    e[size_t(index)].fn(std::max(scale, 0.1f), p, cfg, st);
    st.seed = float(index);
}

void applyBrushTexture(int index, StyleParams& st) {
    const bool classic = index <= 0 || index >= int(entries().size());
    TuningParams p;
    RenderConfig cfg;
    StyleParams recipe;
    if (!classic) apply(index, 1.f, p, cfg, recipe);
    // Flat historical recipes paint strokes here too. A return to the
    // classic tile brush keeps independently chosen colour and finish values.
    if (!classic) st.enabled = 1.f;
    st.brushModel = classic ? 0.f : 1.f;
    st.strokesOff = 0.f;
#define BRUSH_FIELD(name) st.name = recipe.name
    BRUSH_FIELD(widthScale); BRUSH_FIELD(tip); BRUSH_FIELD(capFrac);
    BRUSH_FIELD(taperStart); BRUSH_FIELD(taperEnd); BRUSH_FIELD(tipMin);
    BRUSH_FIELD(swell); BRUSH_FIELD(bristles); BRUSH_FIELD(bristleClump);
    BRUSH_FIELD(bristleContrast); BRUSH_FIELD(bristleAlpha);
    BRUSH_FIELD(colorJitter); BRUSH_FIELD(load); BRUSH_FIELD(dryout);
    BRUSH_FIELD(dryTooth); BRUSH_FIELD(streakNoise); BRUSH_FIELD(streakLen);
    BRUSH_FIELD(edgeRough); BRUSH_FIELD(edgeSoft); BRUSH_FIELD(endJag);
    BRUSH_FIELD(pickup); BRUSH_FIELD(smear); BRUSH_FIELD(smearLen);
    BRUSH_FIELD(impasto); BRUSH_FIELD(ridge); BRUSH_FIELD(startBlob);
    BRUSH_FIELD(flatten); BRUSH_FIELD(groove); BRUSH_FIELD(groovePx);
    BRUSH_FIELD(grooveColor);
#undef BRUSH_FIELD
    // The source recipes for these eras were flat compositing effects, so
    // define a usable mark when borrowing their material as a brush texture.
    const std::string key = classic ? "none" : entries()[size_t(index)].info.key;
    if (key == "ukiyoe") {
        st.tip = 1.f; st.tipMin = 0.95f; st.bristles = 0.f;
        st.edgeSoft = 0.12f; st.edgeRough = 0.02f; st.impasto = 0.02f;
    } else if (key == "watercolor") {
        st.tip = 5.f; st.bristles = 2.f; st.load = 0.35f;
        st.edgeSoft = 1.8f; st.edgeRough = 0.12f; st.impasto = 0.f;
    } else if (key == "pop") {
        st.tip = 0.f; st.bristles = 0.f; st.edgeSoft = 0.15f;
        st.edgeRough = 0.f; st.impasto = 0.f;
    } else if (key == "hockney") {
        st.tip = 1.f; st.bristles = 1.f; st.edgeSoft = 0.25f;
        st.edgeRough = 0.02f; st.impasto = 0.08f;
    } else if (key == "folk") {
        st.tip = 3.f; st.bristles = 5.f; st.dryout = 0.3f;
        st.edgeRough = 0.18f; st.impasto = 0.08f;
    }
    st.seed = classic ? 0.f : float(index);
    if (classic) {
        if (st.finish == 0.5f) st.finish = 0.f;
        // Compare only the independent look portion. Brush mark fields were
        // reset above; the rest should survive a brush-family change.
        const StyleParams defaults;
        const auto* current = reinterpret_cast<const unsigned char*>(&st);
        const auto* baseline = reinterpret_cast<const unsigned char*>(&defaults);
        const size_t begin = offsetof(StyleParams, fieldMode);
        const size_t seed = offsetof(StyleParams, seed);
        const size_t tail = offsetof(StyleParams, flatMottle);
        const bool lookChanged =
            std::memcmp(current + begin, baseline + begin, seed - begin) != 0 ||
            std::memcmp(current + tail, baseline + tail, sizeof(StyleParams) - tail) != 0;
        st.enabled = lookChanged ? 1.f : 0.f;
    } else if (st.finish <= 0.f) {
        st.finish = 0.5f; // brush relief only; no aging or ground effects
    }
}

} // namespace styles
