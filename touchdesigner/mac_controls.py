"""Portable metadata and shared .sbr serialization for the Mac component."""
import json

RADIUS_SETS = {"balanced": (8, 4, 2), "broad": (12, 6, 3), "fine": (6, 3, 1.5),
               "large_wash": (20, 10), "tiny_marks": (4, 2)}
# key, label, default, low, high, integer
CORE = (
    ("threshold", "Detail", 50, 1, 150, False),
    ("curvature", "Curvature", 1, 0, 1, False),
    ("opacity", "Stroke opacity", .9, .05, 1, False),
    ("max-length", "Maximum length", 16, 1, 32, False),
    ("min-length", "Minimum length", 4, 1, 32, False),
    ("grid-factor", "Spacing", 1, .25, 2.5, False),
    ("bristle-density", "Bristle density", 10, 2, 30, False),
    ("passes", "Painting passes", 1, 1, 16, True),
    ("tensor-sigma", "Direction smoothing", 0, 0, 8, False),
    ("etf-iterations", "Edge tangent iterations", 0, 0, 8, True),
    ("etf-radius", "Edge tangent radius", 5, 1, 12, False),
    ("relax-iterations", "Refinement passes", 0, 0, 16, True),
    ("relax-area", "Economy", .06, 0, .4, False),
    ("relax-move", "Move distance", .8, 0, 2, False),
    ("relax-candidates", "Tries per stroke", 6, 1, 16, True),
    ("relax-remove", "Remove weak strokes", 0, 0, 2, False),
    ("relax-subpasses", "Relax subpasses", 1, 1, 4, True),
    ("flow-levels", "Optical flow levels", 0, 0, 5, True),
    ("flow-iterations", "Flow iterations", 3, 1, 12, True),
    ("temporal-diff", "Repaint threshold", 0, 0, 255, False),
    ("fresh-paint", "Fresh paint", 0, 0, 1, False),
    ("jitter-hue", "Hue variation", .05, 0, .3, False),
    ("jitter-sat", "Saturation variation", .1, 0, .5, False),
    ("jitter-val", "Value variation", .1, 0, .5, False),
    ("size-jitter", "Size variation", 0, 0, .6, False),
    ("angle-jitter", "Angle variation (degrees)", 0, 0, 45, False),
    ("opacity-jitter", "Opacity variation", 0, 0, .6, False),
    ("brush-texture", "Classic brush texture", .45, 0, 1, False),
    ("texture-taper", "Classic taper", .4, 0, 1, False),
    ("dry-brush", "Classic dry brush", 0, 0, 1, False),
    ("impasto", "Classic impasto", 0, 0, .5, False),
    ("impasto-light", "Classic lighting", 0, 0, .5, False),
    ("light-angle", "Classic light angle", 45, 0, 360, False),
    ("jitter-per-frame", "Stable variation", 0, 0, 1, True),
)

# Native slider limits mirror Paintify's inspector; numeric entry remains
# available beyond these soft limits to preserve imported look values.
SLIDER_RANGES = {
    "widthScale": (.3, 2.5), "bristles": (0, 60), "load": (.1, 1),
    "edgeRough": (0, .6), "perturb": (0, 1.5),
    "vortexSpiral": (-1.5, 1.5), "vortexRadius": (.05, 1),
    "labJitterL": (0, 20), "labJitterAB": (0, 20), "specular": (0, .6),
}
QUICK_FIELDS = {"widthScale", "bristles", "load", "impasto", "saturation", "warmCool"}
QUICK_CORE = {"threshold", "curvature", "opacity", "grid-factor"}


def core_page(key):
    if key in QUICK_CORE:
        return "Paintify"
    if key.startswith("flow-") or key in {"temporal-diff", "fresh-paint", "jitter-per-frame"}:
        return "Motion"
    if key in {"bristle-density", "brush-texture", "texture-taper", "dry-brush",
               "impasto", "impasto-light", "light-angle"}:
        return "Classic Brush"
    return "Strokes"

def parname(key, prefix=""):
    return prefix + key.replace("-", "").lower().capitalize() if not prefix else prefix + key.replace("-", "").lower()


def select_brush(data, key, block):
    fields = data["fields"]
    recipe = next(b["values"] for b in data["brushes"] if b["key"] == key)
    values = list(block)
    for name in fields[3:33] + ["brushModel", "seed"]:
        i = fields.index(name)
        values[i] = recipe[i]
    values[0] = float(key != "none" or values[33:] != data["defaults"][33:])
    values[2] = 0
    return values


def serialize(core, radii, underpaint, block, brush, layers=()):
    lines = ["# Paintify TouchDesigner / Brushkit", "version 1", "style " + brush,
             "radii " + ", ".join(format(v, ".9g") for v in radii), "underpaint " + underpaint]
    lines += [key + " " + format(value, ".9g") for key, value in core.items()]
    lines += list(layers)
    lines.append("style-block " + " ".join(format(v, ".9g") for v in block))
    return "\n".join(lines) + "\n"


def parse(text, data):
    result = {"core": {}, "layers": [], "block": list(data["defaults"]), "brush": "none"}
    for line in text.splitlines():
        parts = line.split("#", 1)[0].split()
        if not parts:
            continue
        key, values = parts[0], parts[1:]
        if key == "version" and values != ["1"]:
            raise ValueError("Unsupported look version")
        elif key == "style-block":
            if len(values) != len(data["defaults"]):
                raise ValueError("Invalid Brushkit style block")
            result["block"] = [float(v) for v in values]
        elif key == "style":
            result["brush"] = values[0]
        elif key == "radii":
            result["radii"] = [float(v) for v in " ".join(values).replace(",", " ").split()]
        elif key == "underpaint":
            result["underpaint"] = values[0]
        elif key == "layer":
            result["layers"].append(" ".join(parts))
        elif key in {row[0] for row in CORE}:
            result["core"][key] = float(values[0])
    return result
