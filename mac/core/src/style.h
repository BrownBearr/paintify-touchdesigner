#pragma once
#include <cstdint>

// The brushkit style block: everything a painting *style* adds on top of the
// Hertzmann core in TuningParams. Mirrors `Style` in shaders/style.glsl
// (std140, binding 1). Every member is a float and they come in groups of four,
// so the std140 layout is a flat array of vec4s -- keep it that way.
//
// With `enabled == 0` every stage behaves exactly like the classic renderer,
// which is what keeps the port verifiable against it.
struct StyleParams {
    // --- s0: master switches -------------------------------------------------
    float enabled = 0.f;        // 0 = classic rendering behaviour everywhere
    float brushModel = 0.f;     // 0 = web brush tiles, 1 = brushkit procedural brush
    float strokesOff = 0.f;     // 1 = no stroke layers (flat / print styles)
    float widthScale = 1.f;     // ribbon half-width = radius * widthScale
    // --- s1: tip ------------------------------------------------------------
    float tip = 0.f;            // 0 round, 1 flat, 2 filbert, 3 rigger/point, 4 knife, 5 fan
    float capFrac = -1.f;       // end-cap length / half-width (-1 = by tip)
    float taperStart = 0.12f;   // fraction of length the stroke swells in over
    float taperEnd = 0.25f;     // fraction of length it lifts off over
    // --- s2 ------------------------------------------------------------------
    float tipMin = 0.35f;       // width fraction at the extreme tips
    float swell = 0.f;          // pressure belly mid-stroke
    float bristles = 16.f;      // 0 = smooth
    float bristleClump = 0.35f; // uneven paint between bristles
    // --- s3 ------------------------------------------------------------------
    float bristleContrast = 0.22f;
    float bristleAlpha = 0.25f;
    float colorJitter = 0.02f;  // per-bristle colour variation
    float load = 1.f;
    // --- s4 ------------------------------------------------------------------
    float dryout = 0.f;         // paint runs out along the stroke
    float dryTooth = 0.f;       // paint skips the support's valleys
    float streakNoise = 0.15f;
    float streakLen = 3.f;      // in widths
    // --- s5 ------------------------------------------------------------------
    float edgeRough = 0.12f;
    float edgeSoft = 0.8f;      // px
    float endJag = 0.3f;
    float pickup = 0.f;         // wet-in-wet: mix the paint underneath
    // --- s6 ------------------------------------------------------------------
    float smear = 0.f;          // drag the paint underneath along the stroke
    float smearLen = 1.5f;      // in widths
    float impasto = 0.35f;      // paint thickness deposited
    float ridge = 0.35f;        // paint pushed up at the edges
    // --- s7 ------------------------------------------------------------------
    float startBlob = 0.4f;
    float flatten = 0.5f;       // how much a stroke levels relief beneath it
    float groove = 0.35f;       // fine bristle grooves (height)
    float groovePx = 1.7f;
    // --- s8: orientation field ------------------------------------------------
    float grooveColor = 0.05f;
    float fieldMode = 0.f;      // 0 image, 1 vortex, 2 patches, 3 curl, 4 waves, 5 constant
    float fieldMix = 1.f;       // weight of the style field vs the image flow
    float fallbackAngle = -55.f;// deg, used where the image flow is weak
    // --- s9 ------------------------------------------------------------------
    float coherenceMin = 0.08f; // image-flow magnitude that counts as "confident"
    float minFieldMag = 0.02f;  // floor so strokes keep going across flat areas
    float vortexX = 0.5f, vortexY = 0.45f;
    // --- s10 -----------------------------------------------------------------
    float vortexSpiral = 0.6f;  // radians of inward tilt
    float vortexRadius = 0.38f; // influence radius, fraction of the long side
    float patchCell = 30.f;     // px
    float patchJitter = 0.22f;  // radians
    // --- s11 -----------------------------------------------------------------
    float patchQuant = 0.26f;   // radians (15 deg)
    float perturb = 0.f;        // radians of fBm wobble
    float perturbScale = 60.f;  // px
    float curlScale = 110.f;    // px
    // --- s12 -----------------------------------------------------------------
    float waveLen = 140.f;
    float waveAmp = 0.9f;
    float waveAngle = -0.15f;   // radians
    float fallbackMode = 0.f;   // 0 constant angle, 1 curl, 2 horizontal-in-lower-half
    // --- s13: colour -----------------------------------------------------------
    float paletteCount = 0.f;
    float palettePull = 0.f;
    float labJitterL = 0.f;
    float labJitterAB = 0.f;
    // --- s14 -----------------------------------------------------------------
    float saturation = 1.f;
    float value = 1.f;
    float contrast = 1.f;
    float lift = 0.f;
    // --- s15 -----------------------------------------------------------------
    float warmCool = 0.f;       // + warm lights / cool shadows, - the reverse
    float tenebrism = 0.f;      // value power (0 = off, 1.8 = Caravaggio)
    float fauve = 0.f;          // arbitrary hue by value
    float fleckProb = 0.f;      // chance a stroke is a pure palette colour
    // --- s16 -----------------------------------------------------------------
    float hueShift = 0.f;
    float optical = 0.f;        // Seurat: pick a palette pigment at random by closeness
    float valueGate = 1.f;      // skip strokes where the motif is lighter (sumi-e)
    float ink = 0.f;            // 1 = grey ink tone from value
    // --- s17: support + ground --------------------------------------------------
    float support = 1.f;        // 0 none, 1 canvas, 2 linen, 3 coarse, 4 panel, 5 paper, 6 washi
    float supportScale = 1.f;
    float weave = 0.22f;        // support relief shown by the finish
    float underOpacity = 0.55f; // lay-in opacity over the ground (1 = pure blur)
    // --- s18 -----------------------------------------------------------------
    float groundR = 0.93f, groundG = 0.89f, groundB = 0.8f;
    float groundMode = 0.f;     // 0 = original underpaint, 1 = ground + lay-in, 2 = flat print
    // --- s19: finish -------------------------------------------------------------
    float finish = 0.f;         // 1 = brushkit finish pass (relief, age)
    float reliefScale = 1.8f;
    float impastoLight = 0.6f;  // multiplies the height in the relief
    float specular = 0.18f;
    // --- s20 -----------------------------------------------------------------
    float shininess = 22.f;
    float lightX = -0.55f, lightY = -0.75f, elevation = 0.6f;
    // --- s21 -----------------------------------------------------------------
    float varnish = 0.f;
    float varnishR = 1.f, varnishG = 0.9f, varnishB = 0.7f;
    // --- s22 -----------------------------------------------------------------
    float crackle = 0.f;
    float crackleSize = 38.f;
    float vignette = 0.f;
    float grain = 0.012f;
    // --- s23 -----------------------------------------------------------------
    float finalSat = 1.f;
    float finalContrast = 1.f;
    float licStrength = 0.f;    // wet smear along the field (Turner, Munch)
    float licLength = 24.f;     // px
    // --- s24: drawing -----------------------------------------------------------
    float contour = 0.f;        // painted contour lines along strong edges
    float contourWidth = 1.7f;
    float contourThreshold = 0.35f;
    float contourWobble = 1.2f;
    // --- s25 -----------------------------------------------------------------
    float contourR = 0.23f, contourG = 0.28f, contourB = 0.5f;
    float contourSigma = 1.5f;
    // --- s26: flat / print --------------------------------------------------------
    float kuwahara = 4.f;       // flat mode smoothing radius
    float outline = 0.f;        // keylines on value jumps
    float outlineWidth = 1.6f;
    float outlineContrast = 0.12f;
    // --- s27 -----------------------------------------------------------------
    float halftone = 0.f;       // Ben-Day dots in light mid-tones
    float halftoneSpacing = 7.f;
    float caustics = 0.f;       // pool light network on blue water
    float causticScale = 40.f;
    // --- s28 -----------------------------------------------------------------
    float watercolor = 0.f;     // Beer-Lambert washes with edge darkening
    float edgeDarken = 0.9f;
    float granulation = 0.4f;
    float bokashi = 0.f;        // print-style sky gradient
    // --- s29: weather -------------------------------------------------------------
    float rain = 0.f;
    float rainAngle = 70.f;     // deg
    float rainDensity = 1.f;
    float rainLength = 180.f;   // px
    // --- s30 -----------------------------------------------------------------
    float rainAlpha = 0.18f;
    float seed = 0.f;
    float time = 0.f;           // frame counter for animated effects (rain)
    float flatMottle = 0.03f;
    // --- s31 -----------------------------------------------------------------
    float woodgrain = 0.f;      // woodblock grain in flat areas
    float posterize = 0.f;      // flat mode: Lab lightness levels (0 = snap to palette)
    float facet = 0.f;          // analytic-cubist planes: per-facet shading + edges
    float facetCell = 45.f;     // px
    // --- s32 -----------------------------------------------------------------
    float autoVortex = 0.f;     // host: follow the brightest region of each frame
    float border = 0.f;         // unpainted canvas margin, fraction of the short side
    // Video: fraction of unchanged cells repainted anyway each frame, so paint
    // carried along the flow never goes stale or soft (0 = the web rule only).
    float temporalRefresh = 0.f;
    float pad1 = 0.f;

    // Up to 16 palette pigments, rgb + unused.
    float palette[16][4] = {};

    // Per-layer brush roles (one brushkit "pass" per Hertzmann layer, coarse to
    // fine). layerA = (impasto x, dryout +, edge softness x, darkest value that
    // gets a stroke); layerB = (width x, colour value x, field rotation rad,
    // dry-tooth +).
    float layerA[8][4] = {{1, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 1, 0},
                          {1, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 1, 0}};
    float layerB[8][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0},
                          {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}};
};
static_assert(sizeof(StyleParams) % 16 == 0, "Style must stay vec4-aligned");
static_assert(sizeof(StyleParams) == (33 + 16 + 8 + 8) * 16, "Style layout drifted from style.glsl");
