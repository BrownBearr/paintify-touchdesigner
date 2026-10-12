"""Snapshot the tested Brushkit Metal engine and shared control definitions."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("gpu", type=Path)
parser.add_argument("blender", type=Path)
args = parser.parse_args()
files = ["pipeline.cpp", "pipeline.h", "params.h", "style.h", "styles.cpp", "styles.h",
         "gl_util.h", "brush_atlas.cpp", "brush_atlas.h", "paramfile.cpp", "paramfile.h",
         "app_resources.cpp", "app_resources.h", "metal/backend.mm", "metal/api.h"]
for name in files:
    target = ROOT / "mac/core/src" / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.gpu / "src" / name, target)
shutil.copytree(args.gpu / "shaders", ROOT / "mac/core/shaders", dirs_exist_ok=True)
target = ROOT / "mac/core/tools/mac/dependencies.cmake"
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(args.gpu / "tools/mac/dependencies.cmake", target)
for name in ("controls.py", "brushkit_data.json"):
    shutil.copy2(args.blender / "blender_addon/paintify_gpu" / name, ROOT / "touchdesigner" / name)
(ROOT / "mac/core/upstream.json").write_text(json.dumps({
    "repository": "https://github.com/BrownBearr/Paintify-GPU",
    "commit": subprocess.check_output(["git", "-C", str(args.gpu), "rev-parse", "HEAD"], text=True).strip(),
    "headless_metal_init": True,
}, indent=2) + "\n")
