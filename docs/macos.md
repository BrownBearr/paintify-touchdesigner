# Paintify on macOS

`Paintify-macOS.tox` is the Mac build of the component: the same parameters,
Looks and live-update behaviour as the Windows `Paintify.tox`, with a Metal
renderer and Syphon instead of OpenGL 4.6 and Spout.

**Requirements:** Apple Silicon Mac, macOS 13 or newer, TouchDesigner 2025.
Tested on an M3 Pro, macOS 26.6, TouchDesigner 2025.33230. Intel Macs have
not been built or tested (build.sh targets arm64).

## Install

Drag `Paintify-macOS.tox` into a network, wire a TOP into it, view its output.
On first load it unpacks the renderer to
`~/Library/Caches/Paintify/<bundle-id>/` and starts it; the log
(`paintify-live.log`) is in the same folder. A Windows `Paintify.tox` does
not work on a Mac -- its renderer is a Windows executable -- so replace it
rather than keeping both.

## How the port is built

Nothing about the painting is reimplemented. `mac/tools/glsl_to_msl.py`
translates the shaders in `shaders/` -- the files the Windows renderer
compiles -- through SPIR-V (glslang) to Metal Shading Language
(spirv-cross). The GL binding spaces are shifted apart so that each GL
binding lands on exactly one Metal slot (table in `mac/src/mtl_pipeline.h`),
which lets `mac/src/mtl_pipeline.mm` be a line-for-line port of
`src/pipeline.cpp`: it keeps GL-style binding state and applies it at each
dispatch. Brush tiles (`src/brush_atlas.cpp`) and the settings parser
(`src/paramfile.cpp`) are shared source.

| Windows | macOS |
|---|---|
| OpenGL 4.6 compute + `glDrawArraysIndirect` | Metal compute + `drawPrimitives:indirectBuffer:` (same 16-byte command) |
| `glMemoryBarrier` / `glTextureBarrier` | serial compute encoder with tracked resources; a separate render encoder per stroke pass |
| GL program uniforms | `Locals` struct at `buffer(12)`, laid out from Metal reflection |
| Spout sender/receiver | `SyphonMetalServer` / `SyphonMetalClient` |
| `%LOCALAPPDATA%\Paintify` | `~/Library/Caches/Paintify` |

### Binding map

glslang compiles each stage with relaxed Vulkan rules (`-R`), so the loose
`uniform int uPass;` declarations survive as a default uniform block, and
with the image and SSBO binding spaces shifted (`--sib 8 --sbb 1`).
spirv-cross then emits MSL with `--msl-decoration-binding`, which keeps those
bindings as the Metal slots:

| GL | Metal |
|---|---|
| sampler unit *n* | `texture(n)` + `sampler(n)` |
| image binding *n* | `texture(8 + n)` |
| SSBO binding *n* | `buffer(1 + n)` |
| `Params` UBO | `buffer(0)` |
| loose uniforms | `buffer(12)` (`Locals`) |
| `buffer.length()` | `buffer(25)`, sizes indexed by buffer slot |

The shift is not cosmetic. Without it `CellBuf` (SSBO 0) and `Params` (UBO 0)
share set 0 binding 0, and spirv-cross treats them as aliases of one buffer.

`stroke.vert` is translated with `--flip-vert-y`. GL NDC y = -1 is texel row
0 of the canvas framebuffer and Metal's is the last row; flipping there keeps
the rule `architecture.md` describes (image y is NDC y, orientation applied
only at the edges) without editing the shader.

### Host

`MetalPipeline` emulates the parts of GL state the pipeline relies on: bound
texture units, images and SSBOs are recorded by `bindTex` / `bindImage` /
`bindBuf` and applied at each `dispatch`; `Locals` persist per kernel, as GL
program uniforms do; `Params` is passed with `setBytes`, which snapshots it,
so changing `chunk` between dispatches in one command buffer is safe. Each
`Locals` layout is read from Metal reflection rather than written by hand,
because spirv-cross lays it out by MSL rules (a `float3` takes 16 bytes).
Slots a shader declares but the current pass does not use get a 1x1 texture
or a small buffer, which GL tolerates and Metal's validation does not.

GL readbacks that stall (the pool count before relaxation, the 1x1 mip for
the `average` underpaint) become a commit and wait. Stats are read after the
frame's final wait, not a frame late. Buffers are shared storage (Apple
GPUs have unified memory); textures are private.

Shaders are compiled at load with fast math off, matching GL; the Lab error
threshold is calibrated against unrelaxed float maths. The renderer looks for
them in `$PAINTIFY_SHADER_DIR` (set by the component), else `shaders/` next
to the executable.

### Live bridge and component

`--live-syphon` replaces `--live-spout`; the Spout spellings
(`--live-spout`, `--spout-in`, `--spout-out`) are accepted as aliases, so the
component passes the same arguments on both platforms. The settings-file
protocol, stop file, parent-PID check and five-second exit after the input
disappears are unchanged. The renderer turns an `NSRunLoop` instead of
sleeping, because Syphon's server directory learns about servers from
distributed notifications that are only delivered while a run loop runs.

Differences in the TouchDesigner component, all switched on `sys.platform`.
Each one was found by a blank or upside-down output in testing:

- TouchDesigner lists other apps' Syphon servers as `appname:servername`, so
  the receiver asks for `gpu-sbr:Paintify_..._output`
  (`paintify_runtime.receive_name`).
- Syphon returns the painting upright, so `upright_output` does not flip.
- A TOX's VFS holds plain files only. The runtime recreates
  `Syphon.framework`'s version symlinks after extraction and clears the
  quarantine flag a downloaded TOX can pass on to the files it unpacks.
- TouchDesigner publishes a still input once. The renderer paints the first
  frame after it connects even if Syphon does not flag it as new, and
  republishes its last painting every tick so a late-attaching receiver gets
  a frame.

## Verification

- Every Look, relaxation, impasto, ETF, structure tensor, the average
  underpaint, temporal repaint and optical flow run and produce the expected
  pictures on stills and live video.
- Run-to-run difference between two Mac renders of the same still is
  ~5/255 mean absolute RGB, the `atomicAdd` ordering noise `architecture.md`
  describes; changes to the Mac build should be judged against that floor.
- Optical flow on a synthetic 6 px/frame pan: 21 strokes repainted on the
  eighth frame (temporal 20, flow 3), against 2,416 without flow.
- A hard cut under temporal + flow repaints the new scene with nothing
  carried over from the old one.
- In TouchDesigner: first-run extraction from a quarantined TOX, live Look
  changes without restart, Active off/on, and renderer exit after the
  component is deleted.

**Not done:** a pixel comparison against the Windows renderer on the same
image. The shaders are the same source, but GPU and float differences have
not been measured; `comparison.md`'s method would apply unchanged.

## Cost

GPU time per painted frame, 1280x720, M3 Pro. The first frame of a run also
pays for pipeline creation (85 ms) and is excluded.

| Setting | GPU ms |
|---|---|
| Impressionist | 16-22 |
| Pointillist (124K strokes) | 26 |
| Impressionist + relaxation 4 + impasto (the component's defaults), still | 66 |
| Same, live video in TouchDesigner | 45-65 |
| Expressionist + ETF 3 + tensor sigma 2 | 70 |
| Temporal 20 + flow 3, live video | 17-23 |

At the component's defaults the default 12 painted fps holds. Relaxation is
most of the cost; setting it to 0 is the first thing to try on a busy
project. Every render waits for the GPU before publishing, which keeps the
code simple and costs nothing at 12 fps.

## Building the Mac TOX

```sh
brew install glslang spirv-cross
xcodebuild -downloadComponent MetalToolchain   # Syphon's shaders need it
mac/build.sh
```

`build.sh` clones Syphon at a pinned revision, builds it, translates the
shaders and writes `mac/build/runtime/`. Then, in a blank TouchDesigner
project's Textport:

```python
import runpy; runpy.run_path('<path-to-repo>/touchdesigner/install_paintify.py')
```

On a Mac this writes `Paintify-macOS.tox` at the repository root. Like the
Windows build, it replaces a component named `paintify` in `/project1`. To
build somewhere else:

```python
import runpy
m = runpy.run_path('<repo>/touchdesigner/install_paintify.py',
                   init_globals={'PAINTIFY_NO_INSTALL': True})
m['install']('/some/comp', save=True)
```

The renderer also runs standalone, which is the quickest way to check a
shader change:

```sh
mac/build/runtime/gpu-sbr --in photo.jpg --out painted.png --preset wash
mac/build/runtime/gpu-sbr --in a.png,b.png,c.png --repeat 3 --temporal-diff 20 --flow 3 --out last.png
```

The binary is ad-hoc signed. That is enough on the machine that built it and
for a TOX someone downloads (the runtime clears quarantine on extraction).
Wider distribution would want a Developer ID signature and notarization.
