#!/usr/bin/env python3
"""Translate the renderer's OpenGL 4.6 GLSL shaders to Metal Shading Language.

    glsl2msl.py <shader-dir> <out-dir>

The OpenGL shaders are the single source of truth. For each one this script
  1. splices in common.glsl,
  2. rewrites the OpenGL-only constructs glslang rejects under Vulkan rules
     (per-kind binding namespaces, loose uniforms, unlocated varyings,
     gl_VertexID / gl_InstanceID),
  3. runs glslangValidator (GLSL -> SPIR-V) and spirv-cross (SPIR-V -> MSL).

Resource indices in the generated MSL (spirv-cross --msl-decoration-binding):
  textures + samplers   [[texture(N)]] / [[sampler(N)]]   N = the GLSL sampler binding
  storage images        [[texture(IMAGE_BASE + N)]]       N = the GLSL image binding
  storage buffers       [[buffer(N)]]                     N = the GLSL ssbo binding
  Params uniform block  [[buffer(PARAMS_BUFFER)]]
  loose uniforms        [[buffer(LOOSE_BUFFER)]]          packed into one block
metal_pipeline.mm binds by the same numbers; keep IMAGE_BASE, PARAMS_BUFFER and
LOOSE_BUFFER in step with it.
"""
import os
import re
import subprocess
import sys

IMAGE_BASE = 8
PARAMS_BUFFER = 11
LOOSE_BUFFER = 12

STAGES = {'.comp': 'comp', '.vert': 'vert', '.frag': 'frag'}


def splice(shader_dir, name):
    out = []
    with open(os.path.join(shader_dir, name), encoding='utf-8') as f:
        for line in f:
            m = re.match(r'\s*#include\s+"([^"]+)"', line)
            if m:
                with open(os.path.join(shader_dir, m.group(1)), encoding='utf-8') as inc:
                    out.append(inc.read())
                out.append('\n')
            else:
                out.append(line)
    return ''.join(out)


def to_vulkan_glsl(src, stage):
    lines = src.split('\n')
    out = []
    loose = []            # (type, name)
    loose_slot = None     # index in `out` where the block is emitted
    next_loc = 0

    for line in lines:
        s = line.strip()

        # Per-kind binding namespaces -> distinct (set, binding) pairs.
        m = re.match(r'layout\s*\(([^)]*)\)\s*(.*)$', s)
        if m and 'location' not in m.group(1):
            qual, rest = m.group(1), m.group(2)
            b = re.search(r'binding\s*=\s*(\d+)', qual)
            if b:
                n = int(b.group(1))
                other = re.sub(r',?\s*binding\s*=\s*\d+', '', qual).strip(' ,')
                if re.match(r'(readonly\s+|writeonly\s+|restrict\s+)*(buffer)\b', rest) or \
                        re.search(r'\bbuffer\b', rest.split('{')[0]) and 'uniform' not in rest.split('{')[0]:
                    dec = 'set = 2, binding = %d' % n
                elif re.search(r'\buniform\b\s+(writeonly\s+|readonly\s+)?image', rest):
                    dec = 'set = 1, binding = %d' % (IMAGE_BASE + n)
                elif re.search(r'\buniform\s+sampler', rest):
                    dec = 'set = 0, binding = %d' % n
                elif re.search(r'\buniform\s+\w+\s*\{?', rest):
                    dec = 'set = 3, binding = %d' % PARAMS_BUFFER   # the Params block
                else:
                    raise SystemExit('unrecognised resource: ' + s)
                qual_out = (other + ', ' if other else '') + dec
                line = 'layout(%s) %s' % (qual_out, rest)
                out.append(line)
                continue

        # Loose uniforms (no layout) become one uniform block.
        m = re.match(r'uniform\s+(int|uint|float|vec2|vec3|vec4|ivec2|ivec3|uvec2)\s+(\w+)\s*;', s)
        if m:
            if loose_slot is None:
                loose_slot = len(out)
                out.append('')
            loose.append((m.group(1), m.group(2)))
            out.append('')   # keep line numbers stable
            continue

        # Varyings need explicit locations under Vulkan rules.
        m = re.match(r'((?:flat\s+)?)(in|out)\s+(float|vec2|vec3|vec4|uint|int)\s+(\w+)\s*;', s)
        if m and stage in ('vert', 'frag'):
            line = 'layout(location = %d) %s' % (next_loc, s)
            next_loc += 1

        out.append(line)

    if loose:
        members = ' '.join('%s %s;' % t for t in loose)
        out[loose_slot] = ('layout(std140, set = 3, binding = %d) uniform Loose { %s };'
                           % (LOOSE_BUFFER, members))

    text = '\n'.join(out)
    text = text.replace('gl_VertexID', 'gl_VertexIndex')
    text = text.replace('gl_InstanceID', 'gl_InstanceIndex')
    text = text.replace('#version 460 core', '#version 460 core\n#define SBR_METAL 1', 1)
    return text, loose


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        raise SystemExit(1)
    return r.stdout


def main():
    shader_dir, out_dir = sys.argv[1], sys.argv[2]
    glslang = os.environ.get('GLSLANG', 'glslangValidator')
    spirv_cross = os.environ.get('SPIRV_CROSS', 'spirv-cross')
    os.makedirs(out_dir, exist_ok=True)

    names = sorted(n for n in os.listdir(shader_dir)
                   if os.path.splitext(n)[1] in STAGES)
    for name in names:
        base, ext = os.path.splitext(name)
        stage = STAGES[ext]
        src, loose = to_vulkan_glsl(splice(shader_dir, name), stage)
        vk = os.path.join(out_dir, name + '.vk.glsl')
        spv = os.path.join(out_dir, name + '.spv')
        msl = os.path.join(out_dir, '%s_%s.metal' % (base, stage))
        with open(vk, 'w', encoding='utf-8') as f:
            f.write(src)
        run([glslang, '-V', '--target-env', 'vulkan1.1', '-S', stage, vk, '-o', spv])
        entry = '%s_%s' % (base, stage)
        run([spirv_cross, '--msl', '--msl-version', '20300', '--msl-decoration-binding',
             '--rename-entry-point', 'main', entry, stage,
             spv, '--output', msl])
        print('%-14s -> %s  loose: %s' % (name, os.path.basename(msl),
                                         ', '.join('%s %s' % t for t in loose) or '-'))


if __name__ == '__main__':
    main()
