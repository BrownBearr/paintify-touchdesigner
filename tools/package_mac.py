"""Package a self-contained, ad-hoc signed Apple Silicon Metal worker."""
from pathlib import Path
import argparse
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, default=ROOT / "build-metal")
args = parser.parse_args()
bundle = ROOT / "build-package-mac/PaintifyMetal.app"
macos = bundle / "Contents/MacOS"
resources = bundle / "Contents/Resources"
macos.mkdir(parents=True, exist_ok=True)
resources.mkdir(parents=True, exist_ok=True)
shutil.copy2(args.build / "paintify-metal", macos / "paintify-metal")
compiler = resources / "tools/glslang"
compiler.parent.mkdir(exist_ok=True)
shutil.copy2(args.build / "_deps/glslang-build/StandAlone/glslang", compiler)
shutil.copytree(ROOT / "mac/core/shaders", resources / "shaders", dirs_exist_ok=True)
cache = {}
for line in (args.build / "CMakeCache.txt").read_text().splitlines():
    if line.startswith("FETCHCONTENT_SOURCE_DIR_") and "=" in line:
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
licenses = resources / "licenses"
licenses.mkdir(exist_ok=True)
for library, filename in (("glfw", "LICENSE.md"), ("glm", "copying.txt"),
                          ("imgui", "LICENSE.txt"), ("glslang", "LICENSE.txt"),
                          ("spirv_cross", "LICENSE")):
    source = cache.get("FETCHCONTENT_SOURCE_DIR_" + library.upper())
    source = Path(source) if source else args.build / "_deps" / (library + "-src")
    shutil.copy2(source / filename, licenses / (library + ".txt"))
(bundle / "Contents/Info.plist").write_bytes(plistlib.dumps({
    "CFBundleExecutable": "paintify-metal", "CFBundleIdentifier": "com.brownbearr.paintify.touchdesigner",
    "CFBundleName": "Paintify Metal", "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": "0.4.0", "LSUIElement": True,
    "LSMinimumSystemVersion": "13.0",
}))
subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True)
print(bundle)
