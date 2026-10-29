#version 460 core

#include "common.glsl"
#include "style.glsl"

// Brush tiles, one array layer per (radius index, variant). Generated on the
// host by a direct port of brush-texture.js `makeBrushTile`, so the bristle
// profile, the along-stroke value noise and the edge boost are the same
// content the web version modulates coverage with.
layout(binding = 4) uniform sampler2DArray uBrush;
// brushkit brush model only.
layout(binding = 7) uniform sampler2D uTooth;   // support micro-relief, 0..1
layout(binding = 8) uniform sampler2D uSnap;    // canvas as it stood before this layer

in vec3  vColor;
in float vNorm;
in float vArcPx;
flat in float vREff;
flat in float vRadius;
flat in float vTotalLen;
flat in float vOpacity;
flat in uint  vTexHash;
in float vHw;
in vec2  vTang;

layout(location = 0) out vec4 fragColor;
// Height. .r is the paint deposited; .a is how much of the relief underneath
// this fragment levels (brushkit's `flatten`). The host blends attachment 1
// with (ONE, ONE) for the web model, where .a is 0, and with
// (ONE, ONE_MINUS_SRC_ALPHA) for the brushkit model.
layout(location = 1) out vec4 fragHeight;

const float BRUSH_TW = 64.0;
const float BRUSH_TH = 48.0;   // allocated rows; TEX_ROWS of them are in use

// ---------------------------------------------------------------------------
// The web model: worker.js renderStrokeSolid + the brush tile.
// ---------------------------------------------------------------------------
void webBrush() {
    // Coverage, matching worker.js `cov = rEff - dist + 0.5` clamped to 0..1.
    // Perpendicular distance is |vNorm| * vREff because the ribbon edges are
    // at |vNorm| == 1 by construction. Overshoot past either end turns the
    // flat tip into a round cap, which is what `drawCircle` does on the ends
    // of the web's mask.
    float perp = abs(vNorm) * vREff;
    float over = max(0.0, max(-vArcPx, vArcPx - vTotalLen));
    float dist = sqrt(perp * perp + over * over);
    float cov = clamp(vREff - dist + 0.5, 0.0, 1.0);
    if (cov <= 0.0) discard;

    // Brush texture: u tiles along arc length, v is the signed across-width
    // offset. uScale puts one tile repeat every max(16, radius*4) px, the same
    // rule renderStrokeSolid applies before it walks the segments.
    if (TEX_STRENGTH > 0.0) {
        float uOff  = float((vTexHash >> 3) & 63u);
        float layer = TEX_LAYER_BASE + float(vTexHash & 7u);
        float rows  = max(TEX_ROWS, 2.0);

        float tu = fract((vArcPx * TEX_USCALE + uOff) / BRUSH_TW);
        float row = clamp(vNorm * 0.5 + 0.5, 0.0, 1.0) * (rows - 1.0);
        float tv = (row + 0.5) / BRUSH_TH;

        float tile = texture(uBrush, vec3(tu, tv, layer)).r;
        cov *= (1.0 - TEX_STRENGTH) + TEX_STRENGTH * tile;
    }

    // Dry brush: worker.js fades opacity per segment along the stroke. The
    // ribbon has no segments to fade, so the same ramp is applied continuously
    // over arc length, which is the limit of that loop as segments shrink.
    float op = vOpacity;
    if (DRY_BRUSH > 0.0) {
        op *= max(0.0, 1.0 - DRY_BRUSH * clamp(vArcPx / max(vTotalLen, 1e-6), 0.0, 1.0));
    }

    float a = min(1.0, cov * op);
    if (a < 0.002 && IMPASTO_STR <= 0.0) discard;

    fragColor  = vec4(vColor * a, a);        // premultiplied
    // Height accumulates the *pre-opacity* coverage, additively, exactly as
    // `heightBuf[...] += mv * impastoStrength`.
    fragHeight = vec4(cov * IMPASTO_STR, 0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// The brushkit model: a port of brushkit/stroke.py. Every quality of the mark
// is a function of (u, v): u along the stroke, v across it (|v| = 1 at the
// nominal edge). The ribbon already carries both, which is why this port fits.
// ---------------------------------------------------------------------------
void brushkitBrushModel() {
    // per-layer brush role
    vec4 LA = layerA(), LB = layerB();
    float impasto = ST_IMPASTO * LA.x;
    float dryout = clamp(ST_DRYOUT + LA.y, 0.0, 1.0);
    float edgeSoft = max(ST_EDGE_SOFT * LA.z, 0.3);
    float dryTooth = clamp(ST_DRYTOOTH + LB.w, 0.0, 1.0);

    float hw = max(vHw, 0.3);
    float W = 2.0 * max(vRadius * ST_WIDTH_SCALE * LB.x, 0.3);
    float L = max(vTotalLen, 1e-3);
    float v = vNorm;
    float d = v * hw;                                  // signed px across
    float spc = clamp(vArcPx, 0.0, L);
    float u = spc / L;
    uint s = vTexHash * 2654435761u + 0x9e37u;
    vec2 off = vec2(float(vTexHash & 1023u), float((vTexHash >> 5) & 1023u)) * 0.731;

    // --- bristle comb across the brush ------------------------------------
    float B = ST_BRISTLES;
    float dens = 1.0, shade = 0.0, jag = 0.5;
    vec3 cj = vec3(0.0);
    if (B > 0.5) {
        float x = (clamp(v, -1.3, 1.3) * 0.5 + 0.5) * B;
        float n1 = vnoise(vec2(x, 0.37), s);
        float n2 = vnoise(vec2(x * 0.37 + 11.0, 0.73), s + 7u);
        dens = clamp(1.0 - ST_CLUMP * (0.5 - 0.5 * n2) * 1.3 + 0.2 * n1, 0.0, 1.25);
        shade = vnoise(vec2(x, 5.1), s + 3u);
        jag = 0.5 + 0.5 * vnoise(vec2(x, 21.0), s + 5u);
        cj = vec3(vnoise(vec2(x, 9.0), s + 11u), vnoise(vec2(x, 13.0), s + 13u),
                  vnoise(vec2(x, 17.0), s + 17u));
    }

    // --- shape: body, caps, ragged edges -----------------------------------
    float capFrac = ST_CAP_FRAC;
    bool square = false;
    int tip = int(ST_TIP + 0.5);
    if (capFrac < 0.0) {
        if (tip == 1)      { capFrac = 0.08; square = true; }   // flat
        else if (tip == 2)   capFrac = 0.75;                     // filbert
        else if (tip == 4) { capFrac = 0.03; square = true; }    // knife
        else if (tip == 5) { capFrac = 0.15; square = true; }    // fan
        else                 capFrac = 1.0;                      // round, rigger
    } else {
        square = (tip == 1 || tip == 4 || tip == 5);
    }
    float over = max(-vArcPx, vArcPx - L);
    bool beyond = over > 0.0;
    float jagAmt = ST_END_JAG * (jag - 0.5) * (square ? 1.0 : 0.5);
    float capLen = hw * max(capFrac + jagAmt, 0.04);
    float on = beyond ? over / capLen : 0.0;
    float av = abs(v);
    float q = beyond ? (square ? max(av, on) : sqrt(av * av + on * on)) : av;
    if (ST_EDGE_ROUGH > 0.0) {
        float side = (v >= 0.0) ? 1.0 : -1.0;
        float er = 0.7 * gnoise(vec2(spc / (W * 0.9) + off.x, side * 3.1 + off.y), s + 31u)
                 + 0.3 * gnoise(vec2(spc / (W * 0.22) + off.x * 1.3, side * 5.7 + off.y), s + 37u);
        q -= ST_EDGE_ROUGH * er;
    }
    float shape = clamp((1.0 - q) * hw / edgeSoft + 0.5, 0.0, 1.0);
    if (shape < 0.003) discard;

    // --- paint: bristles, load, dry brush ----------------------------------
    float loadU = clamp(ST_LOAD * (1.0 - dryout * pow(u, 1.3)), 0.0, 1.0);
    float streak = (ST_STREAK > 0.0)
        ? gnoise(vec2(spc / (W * ST_STREAK_LEN) + off.x * 0.7, v * max(B, 4.0) * 0.3 + off.y * 0.7), s + 41u)
        : 0.0;
    float tex = 1.0 - ST_BALPHA * (1.0 - clamp(dens, 0.0, 1.0));
    if (ST_STREAK > 0.0) tex *= clamp(1.0 + ST_STREAK * streak, 0.0, 1.0);
    if (dryTooth > 0.0 || ST_LOAD < 0.999 || dryout > 0.0) {
        float tooth = texelFetch(uTooth, ivec2(gl_FragCoord.xy), 0).r;
        float req = (1.0 - loadU) + dryTooth * (0.55 - tooth);
        float val = 0.6 * clamp(dens, 0.0, 1.2) + 0.2 + 0.2 * streak;
        tex *= smoothstep(req - 0.12, req + 0.12, val);
    }
    float alphaT = shape * tex;
    float a = min(1.0, alphaT * vOpacity);
    if (alphaT < 0.002) discard;

    // --- colour ------------------------------------------------------------
    vec3 col = vColor;
    col *= 1.0 + ST_BCONTRAST * 0.5 * shade;
    col += ST_CJITTER * cj;
    float groove = 0.0;
    if (ST_GROOVE > 0.0 || ST_GROOVE_COLOR > 0.0) {
        // fine combed lines left by individual bristles, stretched along the stroke
        groove = gnoise(vec2(d / max(ST_GROOVE_PX, 0.5) + off.x * 0.37, spc / (W * 1.4) + off.y * 0.41), s + 53u);
        col *= 1.0 + ST_GROOVE_COLOR * groove;
    }
    if (ST_PICKUP > 0.0) {
        vec3 under = texelFetch(uSnap, ivec2(gl_FragCoord.xy), 0).rgb;
        col = mix(col, under, ST_PICKUP * (0.3 + 0.7 * u));
    }
    if (ST_SMEAR > 0.0) {
        vec2 t = normalize(vTang + vec2(1e-6));
        vec3 dragged = texture(uSnap, (gl_FragCoord.xy - t * ST_SMEAR_LEN * W) / IMG_SIZE).rgb;
        col = mix(col, dragged, ST_SMEAR * (1.0 - 0.4 * u));
    }
    col = clamp(col, 0.0, 1.0);
    fragColor = vec4(col * a, a);            // premultiplied

    // --- relief ------------------------------------------------------------
    float dep = 0.0;
    if (impasto > 0.0) {
        float ridge = (1.0 - ST_RIDGE) * (0.55 + 0.45 * clamp(dens, 0.0, 1.2))
                    + ST_RIDGE * exp(-pow((av - 0.8) / 0.22, 2.0));
        ridge += ST_GROOVE * 0.5 * groove;
        float blob = 1.0 + ST_START_BLOB * exp(-u * L / (W * 0.8));
        dep = impasto * max(ridge, 0.0) * blob * (0.35 + 0.65 * loadU) * smoothstep(0.0, 1.0, shape);
    }
    fragHeight = vec4(dep * alphaT, 0.0, 0.0, clamp(ST_FLATTEN * alphaT, 0.0, 1.0));
}

void main() {
    if (brushkitBrush()) brushkitBrushModel();
    else webBrush();
}
