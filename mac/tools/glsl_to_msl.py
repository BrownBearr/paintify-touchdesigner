#!/usr/bin/env python3
"""Translate shaders/*.comp|vert|frag to Metal Shading Language.

GLSL 4.60 (OpenGL) -> SPIR-V (glslang, relaxed Vulkan rules so the loose
`uniform int uPass;` style declarations survive) -> MSL (spirv-cross).

The shaders stay the single source of truth: the Windows renderer compiles
them as-is and the Mac renderer loads the generated .metal files at runtime.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'shaders'
STAGES = {'.comp': 'comp', '.vert': 'vert', '.frag': 'frag'}


def splice(path):
    # Mirrors gl_util.cpp's single-level include splice.
    out = []
    for line in path.read_text().splitlines():
        s = line.strip()
        if s.startswith('#include'):
            name = s.split('"')[1]
            out.append((SHADERS / name).read_text())
        else:
            out.append(line)
    return '\n'.join(out) + '\n'


def run(cmd):
    # glslang names every file it compiles; only show its output on failure.
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)


def main(dest):
    dest = Path(dest)
    work = dest / 'spv'
    work.mkdir(parents=True, exist_ok=True)
    for src in sorted(SHADERS.iterdir()):
        stage = STAGES.get(src.suffix)
        if not stage:
            continue
        base = src.stem + '_' + stage
        glsl = work / (base + '.' + stage)
        glsl.write_text(splice(src))
        spv = work / (base + '.spv')
        # Metal has one index space per resource kind, so GL's separate
        # SSBO/UBO and sampler/image binding spaces are shifted apart:
        # images -> texture(8+n), SSBOs -> buffer(1+n), loose uniforms ->
        # buffer(12). mtl_pipeline.h documents the full mapping.
        run(['glslangValidator', '-V', '-R', '--auto-map-locations',
             '--sdub', 'Locals', '0', '12', '--sib', '8', '--sbb', '1',
             '--target-env', 'vulkan1.1', '-S', stage, '-o', str(spv), str(glsl)])
        msl = dest / (base + '.metal')
        # GL's NDC y points at texel row 0 of the framebuffer, Metal's at the
        # last row; flipping here keeps stroke.vert's "image y is NDC y" rule.
        flip = ['--flip-vert-y'] if stage == 'vert' else []
        # --msl-decoration-binding keeps the shifted GL bindings as the Metal
        # slots. Each stage is its own library with entry point "main0".
        run(['spirv-cross', str(spv), '--msl', '--msl-version', '20300', *flip,
             '--msl-decoration-binding', '--rename-entry-point', 'main', 'main0', stage,
             '--output', str(msl)])
        print('ok', msl.name)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'mac' / 'build' / 'msl')
