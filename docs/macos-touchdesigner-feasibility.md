# macOS

Status: **implemented, never compiled or run on a Mac.** It was written on a
Linux machine with no access to Apple's toolchain. The shader translation was
checked there (glslang and spirv-cross run clean over every shader); the
Objective-C++ host code, the Metal shader compile, Syphon and the TouchDesigner
wiring were not. Expect a round of build and runtime fixes on first contact.

## Why it is a separate renderer

macOS stops at OpenGL 4.1, which has no compute shaders, and the Windows
renderer is thirteen compute shaders on OpenGL 4.6. So the Mac build is a Metal
port of the same pipeline, not a recompile:

| | Windows | macOS |
| --- | --- | --- |
| Renderer | OpenGL 4.6, `src/pipeline.cpp` | Metal, `mac/metal_pipeline.mm` |
| Shaders | `shaders/*.comp *.vert *.frag` | the same files, translated at build time |
| Video I/O | Spout, `src/live_spout.cpp` | Syphon, `mac/syphon_live.mm` |
| Executable | `gpu-sbr.exe` | `paintify-mac` |
| Options, presets, settings file | `src/options.cpp` | the same file |

The GLSL stays the single source of truth. `tools/glsl2msl.py` rewrites the
OpenGL-only constructs, runs glslang (GLSL to SPIR-V) and spirv-cross (SPIR-V to
MSL), and `mac/CMakeLists.txt` compiles the result into `paintify.metallib`. The
`#ifdef SBR_METAL` blocks in a few shaders cover what has no Metal equivalent:
`.length()` on unsized buffers, read-write storage textures, and the framebuffer
y direction.

Not ported: the interactive GUI, video and batch modes, and the diagnostic
readbacks (energy, flow coherence, stroke dumps). A single still image does work
(`paintify-mac --in photo.jpg --out painted.png`), which is the quickest way to
check the renderer without TouchDesigner.

## Build

Needs a Mac with full Xcode (the Metal compiler is not in the Command Line
Tools) and:

```sh
brew install cmake glslang spirv-cross
tools/build-mac.sh --test
```

The script builds Syphon.framework from source (override with
`SYPHON_FRAMEWORK_DIR` or `SYPHON_REF`), builds the renderer into `build-mac/`,
ad-hoc signs it, writes `build-mac/paintify-mac-runtime.tar`, and with `--test`
paints the built-in subject to `build-mac/test.png`. Set `CODESIGN_IDENTITY` to
sign with a Developer ID instead.

Then bundle it: run `touchdesigner/install_paintify.py` in TouchDesigner on the
Mac (see [Building the TOX](building-the-tox.md)). The TOX stores the tarball
and extracts it to `~/Library/Caches/Paintify/<bundle-id>/` on first load.

## Verifying it

1. `tools/build-mac.sh --test`, open `build-mac/test.png`. Compare with the
   Windows renderer's still of the same subject (`gpu-sbr --headless`). The
   shaders are the same, so differences point at the host port.
2. Drag the TOX into TouchDesigner: `Video Device In TOP -> Paintify -> Null`.
3. Try each Look, then relaxation (`Relaxation iterations` > 0) and optical flow
   (`Optical flow levels` > 0). Those exercise the paths that need CPU readbacks
   and mipmapped luma textures, which are the likeliest to need fixes.

## Things most likely to need adjusting

- **Orientation.** Syphon frames are bottom-row-first, Metal textures are
  top-row-first. The defaults assume the Syphon Metal client hands back the
  raw bottom-first surface and that publishing with `flipped: YES` makes it
  upright for TouchDesigner. If the input or output is upside down, use the
  **macOS: flip Syphon input / output** toggles on the Paintify Advanced page
  (they map to `--flip-in` / `--flip-out`), then change the defaults in
  `install_paintify.py` and `mac/syphon_live.h`. The Windows-only
  `upright_output` flip in the TOX is disabled on macOS.
- **Syphon API drift.** `mac/syphon_live.mm` uses `SyphonMetalClient`,
  `SyphonMetalServer` and `SyphonServerDirectory` as documented in
  [Syphon-Framework](https://github.com/Syphon/Syphon-Framework). The
  `Syphon.framework` header layout changed between releases; the import is
  `<Syphon/Syphon.h>`.
- **Resource binding.** Compute stages bind by the argument indices reflected
  from the compiled pipeline, and report `no buffer bound at index N` on stderr
  if a stage needs a buffer the host did not supply. The `Loose*` structs in
  `mac/metal_pipeline.mm` must match the `Loose` blocks in the generated MSL
  (`build-mac/mac/msl/*.metal`); `tools/glsl2msl.py` prints each shader's
  loose-uniform list when it runs.
- **Colour.** The canvas is RGBA16F; it is converted to 8-bit BGRA only to hand
  to Syphon, so Syphon's 8-bit interchange is the one quantization point.
- **GPU families.** Nothing here needs read-write textures or 32-bit float
  filtering, so Apple Silicon and AMD/Intel Macs with Metal 2.3 should behave
  alike. A 16x16 threadgroup is the largest used.

## Primary sources

- [TouchDesigner system requirements](https://docs.derivative.ca/System_Requirements) and [macOS support](https://docs.derivative.ca/MacOS)
- [TouchDesigner Syphon Spout In TOP](https://docs.derivative.ca/Syphon_Spout_In_TOP) and [Syphon Spout Out TOP](https://docs.derivative.ca/Syphon_Spout_Out_TOP)
- [Apple on macOS OpenGL 4.1 and Metal](https://developer.apple.com/videos/play/wwdc2020/10631/)
- [SyphonMetalClient](https://github.com/Syphon/Syphon-Framework/blob/main/SyphonMetalClient.h) and [SyphonMetalServer](https://github.com/Syphon/Syphon-Framework/blob/main/SyphonMetalServer.h)
