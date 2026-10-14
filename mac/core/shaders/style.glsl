// brushkit style block + the procedural toolkit the style stages share.
// Spliced into every shader through common.glsl. Mirrors src/style.h (std140).
//
// Everything here is a port of the Python brushkit: noise.py (value / gradient
// / fBm / Worley), color.py (Lab, palette pull, warm-cool, jitter) and the
// colour logic of the style recipes. Hashes are position-seeded, never
// frame-seeded, so a still renders the same every run and a static region of a
// video keeps its texture.

layout(std140, binding = 1) uniform Style {
    vec4 bk[33];
    vec4 bkPal[16];
    vec4 bkLayerA[8];   // impasto x, dryout +, edge softness x, darkest value painted
    vec4 bkLayerB[8];   // width x, colour value x, field rotation, dry tooth +
};

#define ST_ENABLED        bk[0].x
#define ST_BRUSHMODEL     bk[0].y
#define ST_STROKES_OFF    bk[0].z
#define ST_WIDTH_SCALE    bk[0].w
#define ST_TIP            bk[1].x
#define ST_CAP_FRAC       bk[1].y
#define ST_TAPER_START    bk[1].z
#define ST_TAPER_END      bk[1].w
#define ST_TIP_MIN        bk[2].x
#define ST_SWELL          bk[2].y
#define ST_BRISTLES       bk[2].z
#define ST_CLUMP          bk[2].w
#define ST_BCONTRAST      bk[3].x
#define ST_BALPHA         bk[3].y
#define ST_CJITTER        bk[3].z
#define ST_LOAD           bk[3].w
#define ST_DRYOUT         bk[4].x
#define ST_DRYTOOTH       bk[4].y
#define ST_STREAK         bk[4].z
#define ST_STREAK_LEN     bk[4].w
#define ST_EDGE_ROUGH     bk[5].x
#define ST_EDGE_SOFT      bk[5].y
#define ST_END_JAG        bk[5].z
#define ST_PICKUP         bk[5].w
#define ST_SMEAR          bk[6].x
#define ST_SMEAR_LEN      bk[6].y
#define ST_IMPASTO        bk[6].z
#define ST_RIDGE          bk[6].w
#define ST_START_BLOB     bk[7].x
#define ST_FLATTEN        bk[7].y
#define ST_GROOVE         bk[7].z
#define ST_GROOVE_PX      bk[7].w
#define ST_GROOVE_COLOR   bk[8].x
#define ST_FIELD_MODE     bk[8].y
#define ST_FIELD_MIX      bk[8].z
#define ST_FALLBACK_ANGLE bk[8].w
#define ST_COHERENCE_MIN  bk[9].x
#define ST_MIN_FIELD_MAG  bk[9].y
#define ST_VORTEX         bk[9].zw
#define ST_VORTEX_SPIRAL  bk[10].x
#define ST_VORTEX_RADIUS  bk[10].y
#define ST_PATCH_CELL     bk[10].z
#define ST_PATCH_JITTER   bk[10].w
#define ST_PATCH_QUANT    bk[11].x
#define ST_PERTURB        bk[11].y
#define ST_PERTURB_SCALE  bk[11].z
#define ST_CURL_SCALE     bk[11].w
#define ST_WAVE_LEN       bk[12].x
#define ST_WAVE_AMP       bk[12].y
#define ST_WAVE_ANGLE     bk[12].z
#define ST_FALLBACK_MODE  bk[12].w
#define ST_PAL_COUNT      bk[13].x
#define ST_PAL_PULL       bk[13].y
#define ST_JIT_L          bk[13].z
#define ST_JIT_AB         bk[13].w
#define ST_SATURATION     bk[14].x
#define ST_VALUE          bk[14].y
#define ST_CONTRAST       bk[14].z
#define ST_LIFT           bk[14].w
#define ST_WARMCOOL       bk[15].x
#define ST_TENEBRISM      bk[15].y
#define ST_FAUVE          bk[15].z
#define ST_FLECK          bk[15].w
#define ST_HUE_SHIFT      bk[16].x
#define ST_OPTICAL        bk[16].y
#define ST_VALUE_GATE     bk[16].z
#define ST_INK            bk[16].w
#define ST_SUPPORT        bk[17].x
#define ST_SUPPORT_SCALE  bk[17].y
#define ST_WEAVE          bk[17].z
#define ST_UNDER_OPACITY  bk[17].w
#define ST_GROUND         bk[18].xyz
#define ST_GROUND_MODE    bk[18].w
#define ST_FINISH         bk[19].x
#define ST_RELIEF_SCALE   bk[19].y
#define ST_IMPASTO_LIGHT  bk[19].z
#define ST_SPECULAR       bk[19].w
#define ST_SHININESS      bk[20].x
#define ST_LIGHT          bk[20].yz
#define ST_ELEVATION      bk[20].w
#define ST_VARNISH        bk[21].x
#define ST_VARNISH_TINT   bk[21].yzw
#define ST_CRACKLE        bk[22].x
#define ST_CRACKLE_SIZE   bk[22].y
#define ST_VIGNETTE       bk[22].z
#define ST_GRAIN          bk[22].w
#define ST_FINAL_SAT      bk[23].x
#define ST_FINAL_CONTRAST bk[23].y
#define ST_LIC            bk[23].z
#define ST_LIC_LEN        bk[23].w
#define ST_CONTOUR        bk[24].x
#define ST_CONTOUR_WIDTH  bk[24].y
#define ST_CONTOUR_THR    bk[24].z
#define ST_CONTOUR_WOBBLE bk[24].w
#define ST_CONTOUR_COLOR  bk[25].xyz
#define ST_CONTOUR_SIGMA  bk[25].w
#define ST_KUWAHARA       bk[26].x
#define ST_OUTLINE        bk[26].y
#define ST_OUTLINE_WIDTH  bk[26].z
#define ST_OUTLINE_CONTR  bk[26].w
#define ST_HALFTONE       bk[27].x
#define ST_HALFTONE_SP    bk[27].y
#define ST_CAUSTICS       bk[27].z
#define ST_CAUSTIC_SCALE  bk[27].w
#define ST_WATERCOLOR     bk[28].x
#define ST_EDGE_DARKEN    bk[28].y
#define ST_GRANULATION    bk[28].z
#define ST_BOKASHI        bk[28].w
#define ST_RAIN           bk[29].x
#define ST_RAIN_ANGLE     bk[29].y
#define ST_RAIN_DENSITY   bk[29].z
#define ST_RAIN_LENGTH    bk[29].w
#define ST_RAIN_ALPHA     bk[30].x
#define ST_SEED           bk[30].y
#define ST_TIME           bk[30].z
#define ST_FLAT_MOTTLE    bk[30].w
#define ST_WOODGRAIN      bk[31].x
#define ST_POSTERIZE      bk[31].y
#define ST_FACET          bk[31].z
#define ST_FACET_CELL     bk[31].w
#define ST_AUTO_VORTEX    bk[32].x
#define ST_BORDER         bk[32].y
#define ST_REFRESH        bk[32].z

int  bkLayer()  { return clamp(int(CUR_LAYER + 0.5), 0, 7); }
vec4 layerA()   { return bkLayerA[bkLayer()]; }
vec4 layerB()   { return bkLayerB[bkLayer()]; }

bool styleOn() { return ST_ENABLED > 0.5; }
bool brushkitBrush() { return ST_ENABLED > 0.5 && ST_BRUSHMODEL > 0.5; }

// --- noise (noise.py) -------------------------------------------------------
uint bkHash(uvec2 p, uint s) {
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
float bkHash01(ivec2 p, uint s) { return float(bkHash(uvec2(p), s) & 0x00ffffffu) / 16777216.0; }

float fade5(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in [-1, 1].
float vnoise(vec2 p, uint s) {
    vec2 i = floor(p), f = p - i;
    ivec2 ii = ivec2(i);
    float a = bkHash01(ii, s), b = bkHash01(ii + ivec2(1, 0), s);
    float c = bkHash01(ii + ivec2(0, 1), s), d = bkHash01(ii + ivec2(1, 1), s);
    vec2 u = vec2(fade5(f.x), fade5(f.y));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 2.0 - 1.0;
}

vec2 gradAt(ivec2 p, uint s) {
    float a = bkHash01(p, s) * 6.2831853;
    return vec2(cos(a), sin(a));
}

// Gradient (Perlin-style) noise, roughly in [-1, 1].
float gnoise(vec2 p, uint s) {
    vec2 i = floor(p), f = p - i;
    ivec2 ii = ivec2(i);
    float n00 = dot(gradAt(ii, s), f);
    float n10 = dot(gradAt(ii + ivec2(1, 0), s), f - vec2(1, 0));
    float n01 = dot(gradAt(ii + ivec2(0, 1), s), f - vec2(0, 1));
    float n11 = dot(gradAt(ii + ivec2(1, 1), s), f - vec2(1, 1));
    vec2 u = vec2(fade5(f.x), fade5(f.y));
    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y) * 1.414;
}

float fbm(vec2 p, int oct, uint s) {
    float t = 0.0, amp = 1.0, norm = 0.0, fr = 1.0;
    for (int o = 0; o < oct; ++o) {
        t += amp * gnoise(p * fr + vec2(float(o) * 19.19, float(o) * 7.31), s + uint(o) * 101u);
        norm += amp; amp *= 0.5; fr *= 2.0;
    }
    return t / norm;
}

// Worley: returns (F1, F2) and the id of the nearest feature point.
vec2 worley(vec2 p, uint s, out uint cellId, out vec2 feature) {
    vec2 i = floor(p), f = p - i;
    float f1 = 9.0, f2 = 9.0;
    cellId = 0u; feature = vec2(0.0);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        ivec2 c = ivec2(i) + ivec2(x, y);
        vec2 o = vec2(bkHash01(c, s), bkHash01(c, s ^ 0x5bd1e995u));
        vec2 fp = vec2(x, y) + o;
        float d = length(fp - f);
        if (d < f1) { f2 = f1; f1 = d; cellId = bkHash(uvec2(c), s ^ 0x1234567u); feature = i + fp; }
        else if (d < f2) { f2 = d; }
    }
    return vec2(f1, f2);
}

// --- colour (color.py) ------------------------------------------------------
vec3 srgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 rgb2lab(vec3 c) {
    vec3 lin = srgbToLinear(clamp(c, 0.0, 1.0));
    vec3 xyz = vec3(dot(lin, vec3(0.4124564, 0.3575761, 0.1804375)) / 0.95047,
                    dot(lin, vec3(0.2126729, 0.7151522, 0.0721750)),
                    dot(lin, vec3(0.0193339, 0.1191920, 0.9503041)) / 1.08883);
    vec3 f = mix((903.3 * xyz + 16.0) / 116.0, pow(max(xyz, 1e-8), vec3(1.0 / 3.0)), step(vec3(0.008856), xyz));
    return vec3(116.0 * f.y - 16.0, 500.0 * (f.x - f.y), 200.0 * (f.y - f.z));
}

vec3 lab2rgb(vec3 lab) {
    float fy = (lab.x + 16.0) / 116.0;
    float fx = fy + lab.y / 500.0;
    float fz = fy - lab.z / 200.0;
    vec3 f3 = vec3(fx * fx * fx, fy * fy * fy, fz * fz * fz);
    vec3 xyz = mix((116.0 * vec3(fx, fy, fz) - 16.0) / 903.3, f3, step(vec3(0.008856), f3));
    xyz.y = (lab.x > 8.0) ? f3.y : lab.x / 903.3;
    xyz *= vec3(0.95047, 1.0, 1.08883);
    vec3 lin = vec3(dot(xyz, vec3(3.2404542, -1.5371385, -0.4985314)),
                    dot(xyz, vec3(-0.9692660, 1.8760108, 0.0415560)),
                    dot(xyz, vec3(0.0556434, -0.2040259, 1.0572252)));
    return clamp(linearToSrgb(lin), 0.0, 1.0);
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Nearest palette entry in Lab, with lightness down-weighted (a painter picks
// the pigment by hue family, then mixes it to value).
int paletteNearest(vec3 lab, float lWeight) {
    int n = int(clamp(ST_PAL_COUNT, 0.0, 16.0));
    int best = 0; float bd = 1e20;
    for (int i = 0; i < n; ++i) {
        vec3 d = lab - rgb2lab(bkPal[i].rgb);
        d.x *= lWeight;
        float dd = dot(d, d);
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}

// Two uniforms summed: a cheap bell-shaped jitter in [-1, 1].
float tri(uint h, uint k) { return (hash01(h ^ k) + hash01(h ^ (k * 0x9e3779b9u))) - 1.0; }

// The painter's colour logic for one stroke (colorizer in brushkit). `h` is a
// position-seeded hash; `jitter` false gives the deterministic version used
// for the ground / lay-in.
vec3 styleColor(vec3 c, uint h, bool jitter) {
    c = clamp(c, 0.0, 1.0);
    float lum = luma(c);

    if (ST_TENEBRISM > 0.0) {   // Caravaggio: sink the darks toward warm umber
        float k = pow(max(lum, 1e-3), ST_TENEBRISM) / max(lum, 1e-3);
        c *= k;
        float t = (1.0 - smoothstep(0.04, 0.45, lum)) * 0.65;
        c = mix(c, vec3(0.16, 0.10, 0.06), t);
    }
    if (ST_SATURATION != 1.0 || ST_VALUE != 1.0 || ST_HUE_SHIFT != 0.0) {
        vec3 hsv = rgb2hsv(c);
        hsv.x = fract(hsv.x + ST_HUE_SHIFT);
        hsv.y = clamp(hsv.y * ST_SATURATION, 0.0, 1.0);
        hsv.z = clamp(hsv.z * ST_VALUE, 0.0, 1.0);
        c = hsv2rgb(hsv);
    }
    if (ST_CONTRAST != 1.0) c = clamp((c - 0.5) * ST_CONTRAST + 0.5, 0.0, 1.0);
    if (ST_LIFT > 0.0) c = c * (1.0 - ST_LIFT) + ST_LIFT;
    if (ST_WARMCOOL != 0.0) {
        float l = luma(c);
        float w = (l - 0.5) * 2.0 * sign(ST_WARMCOOL);
        vec3 tint = (w > 0.0) ? vec3(1.0, 0.55, 0.15) : vec3(0.25, 0.3, 0.9);
        c = clamp(c + abs(ST_WARMCOOL) * abs(w) * (tint - c) * 0.5, 0.0, 1.0);
    }
    if (ST_FAUVE > 0.5) {       // value-coherent but arbitrary, saturated hue
        vec3 hsv = rgb2hsv(c);
        float l = luma(c);
        hsv.y = clamp(hsv.y * 2.2 + 0.35, 0.0, 1.0);
        hsv.z = clamp(0.45 + 0.6 * hsv.z, 0.0, 1.0);
        float tgt = (l < 0.4) ? 0.66 : ((l > 0.68) ? 0.1 : hsv.x);
        float dh = fract(tgt - hsv.x + 0.5) - 0.5;
        hsv.x = fract(hsv.x + 0.55 * dh);
        c = hsv2rgb(hsv);
    }
    if (ST_INK > 0.5) {         // sumi: value becomes an ink tone
        float d = 1.0 - luma(c);
        float g = clamp(1.0 - ST_INK * (0.4 + 0.6 * d) + (jitter ? tri(h, 0x77u) * 0.04 : 0.0), 0.03, 1.0);
        return vec3(g);
    }

    int n = int(ST_PAL_COUNT);
    if (n > 0) {
        vec3 lab = rgb2lab(c);
        if (ST_OPTICAL > 0.5 && jitter) {
            // Divisionism: draw one pure pigment at random, weighted by how close
            // it is, so the *average* of neighbouring dots matches the motif.
            float wsum = 0.0;
            for (int i = 0; i < n; ++i) wsum += exp(-length(lab - rgb2lab(bkPal[i].rgb)) / 18.0);
            float r = hash01(h ^ 0x3c3cu) * wsum;
            int pick = n - 1;
            for (int i = 0; i < n; ++i) {
                r -= exp(-length(lab - rgb2lab(bkPal[i].rgb)) / 18.0);
                if (r <= 0.0) { pick = i; break; }
            }
            vec3 pl = rgb2lab(bkPal[pick].rgb);
            pl.x += (lab.x - pl.x) * 0.55;   // keep the value structure legible
            c = lab2rgb(pl);
        } else if (jitter && ST_FLECK > 0.0 && hash01(h ^ 0x5eedu) < ST_FLECK) {
            vec3 s = rgb2hsv(c); s.y = clamp(s.y * 2.2, 0.0, 1.0);
            c = bkPal[paletteNearest(rgb2lab(hsv2rgb(s)), 0.3)].rgb;
        } else if (ST_PAL_PULL > 0.0) {
            vec3 tgt = rgb2lab(bkPal[paletteNearest(lab, 0.35)].rgb);
            tgt.x = lab.x;
            c = lab2rgb(mix(lab, tgt, ST_PAL_PULL));
        }
    }
    if (jitter && (ST_JIT_L > 0.0 || ST_JIT_AB > 0.0)) {
        vec3 lab = rgb2lab(c);
        lab += vec3(tri(h, 0x101u) * ST_JIT_L, tri(h, 0x202u) * ST_JIT_AB, tri(h, 0x303u) * ST_JIT_AB) * 1.6;
        c = lab2rgb(lab);
    }
    return clamp(c, 0.0, 1.0);
}
