"""Portable metadata and shared .sbr serialization for the Mac component."""
import json

RADIUS_SETS = {"balanced": (8, 4, 2), "broad": (12, 6, 3), "fine": (6, 3, 1.5),
               "large_wash": (20, 10), "tiny_marks": (4, 2)}
# key, label, default, low, high, integer
CORE = (
    ("threshold", "Stroke threshold", 50, 0, 150, False),
    ("curvature", "Curvature", 1, 0, 1, False),
    ("opacity", "Stroke opacity", .9, 0, 1, False),
    ("max-length", "Maximum length", 16, 1, 64, False),
    ("min-length", "Minimum length", 4, 0, 32, False),
    ("grid-factor", "Grid spacing", 1, .25, 3, False),
    ("bristle-density", "Bristle density", 1, .1, 3, False),
    ("passes", "Painting passes", 1, 1, 4, True),
    ("tensor-sigma", "Tensor smoothing", 0, 0, 12, False),
    ("etf-iterations", "Edge tangent iterations", 0, 0, 8, True),
    ("etf-radius", "Edge tangent radius", 3, 1, 12, False),
    ("relax-iterations", "Relaxation", 0, 0, 12, True),
    ("relax-area", "Relax area", .05, 0, 1, False),
    ("relax-move", "Relax move", .5, 0, 2, False),
    ("relax-candidates", "Relax candidates", 4, 1, 16, True),
    ("relax-remove", "Relax removal", 1, 0, 2, False),
    ("relax-subpasses", "Relax subpasses", 1, 1, 4, True),
    ("flow-levels", "Optical flow levels", 0, 0, 5, True),
    ("flow-iterations", "Flow iterations", 3, 1, 12, True),
    ("temporal-diff", "Repaint threshold", 0, 0, 255, False),
    ("fresh-paint", "Fresh paint", 0, 0, 1, False),
    ("jitter-hue", "Hue variation", .05, 0, .3, False),
    ("jitter-sat", "Saturation variation", .1, 0, .5, False),
    ("jitter-val", "Value variation", .1, 0, .5, False),
    ("size-jitter", "Size variation", 0, 0, 1, False),
    ("angle-jitter", "Angle variation", 0, 0, 1, False),
    ("opacity-jitter", "Opacity variation", 0, 0, 1, False),
    ("brush-texture", "Classic brush texture", .45, 0, 1, False),
    ("texture-taper", "Classic taper", .4, 0, 1, False),
    ("dry-brush", "Classic dry brush", 0, 0, 1, False),
    ("impasto", "Classic impasto", 0, 0, 2, False),
    ("impasto-light", "Classic lighting", 0, 0, 2, False),
    ("light-angle", "Classic light angle", 45, 0, 360, False),
    ("jitter-per-frame", "Animate variation", 0, 0, 1, True),
)

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
