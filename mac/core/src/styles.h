#pragma once
#include "params.h"
#include "style.h"

#include <string>
#include <vector>

// The brushkit style registry: each entry is one of brushkit's Python recipes
// (brushkit/styles/*.py) re-expressed as settings for this GPU pipeline --
// Hertzmann layers standing in for brushkit's passes, the StyleParams block for
// the brush, field, colour, ground and finish.
namespace styles {

struct Info {
    const char* key;
    const char* name;
    const char* era;
    const char* years;
    const char* artists;
    const char* summary;
};

const std::vector<Info>& list();
int indexOf(const std::string& key);   // -1 when unknown

// Applies style `index` to the three parameter blocks. `scale` multiplies every
// length (radii, patch sizes, noise scales, line widths); 1.0 is tuned for an
// image about 1000 px on its long side, and autoScale() picks it from the size.
void apply(int index, float scale, TuningParams& p, RenderConfig& cfg, StyleParams& st);

// Selects only brush texture; placement, path, colour and finish stay editable.
void applyBrushTexture(int index, StyleParams& st);

// "none": the classic rendering pipeline.
void clear(StyleParams& st, RenderConfig& cfg);

inline float autoScale(int w, int h) {
    const int m = w > h ? w : h;
    return m > 0 ? float(m) / 1000.f : 1.f;
}

} // namespace styles
