# Paintify for TouchDesigner

Turn any TouchDesigner TOP into a live, GPU-painted oil painting.

## Optimized local components

Use **[Paintify-Optimized.tox](Paintify-Optimized.tox)** on Windows or
**[Paintify-Mac-Optimized.tox](Paintify-Mac-Optimized.tox)** on Apple Silicon.
The original upstream TOXs are preserved under their original filenames.
The Windows build uses a 1280-pixel working cap, four painting passes and zero
relaxation; Mac defaults to half width/height and skips source readback on
throttled cooks. Full resolution and quality controls remain available.
See [Mac controls and runtime](docs/macos-metal-component.md) and the local
validation report for verification limits.

The native **Paintify** page now contains the everyday controls, performance
presets, live status and a built-in quick-start guide. Advanced controls are
grouped by task; every original parameter remains available. See the
[TOX control guide](docs/tox-controls.md).

## Original upstream install (one file)

1. **[Download Paintify.tox](https://github.com/BrownBearr/paintify-touchdesigner/raw/main/Paintify.tox)**
2. Drag it into a TouchDesigner Network Editor.
3. Connect any TOP (Movie File In, Video Device In, ...) to its input and view its output.

That is all. The TOX carries its own renderer, DLLs and shaders and unpacks
them to `%LOCALAPPDATA%\Paintify` the first time it loads. There is nothing to
build, clone or install, and no other files from this repository are needed.

**Windows Spout TOX requirements:** Windows 10/11, TouchDesigner (any recent
build), and a GPU with OpenGL 4.6. TouchDesigner's own installer provides the
VC++ runtime the renderer uses. TouchDesigner Non-Commercial limits images to
1280x1280. This Spout component is the Windows integration. A separate Mac
Metal worker branch is integrated in this repository; its runtime and packaged
TOX are maintained separately from this Windows installer.

## Controls

**Look** picks Impressionist, Expressionist, Pointillist, Wash or Detail. The
main page also has painted FPS, maximum working dimension, relaxation, brush
texture, impasto, temporal repaint and optical flow. The live default caps the
longest working dimension at 1280 pixels while preserving aspect ratio; set it
to 0 to use the TOP's native size. **Brush texture = -1** uses the look's own
value.

The **Paintify Advanced** page exposes radii, threshold, curvature, opacity,
stroke lengths, underpaint, tensor smoothing, edge tangent flow, bristle
density, dry brush, lighting angle, jitters and relaxation tuning. Numeric
advanced controls default to `-1`, which keeps the look's preset. The
interactive defaults use 4 painting passes and no relaxation; for quality
output set **Passes** to 8 and **Relax** to 4. Relaxation repaints the canvas
for every iteration, so it can substantially reduce live throughput. Look,
FPS and painting controls apply live without restarting the renderer.

## Troubleshooting

- **Blank output:** check the Textport (Dialogs > Textport). The component
  prints its log path on start; the log lives in
  `%LOCALAPPDATA%\Paintify\<bundle-id>\paintify-live.log`.
- Verify the input TOP displays an image on its own before connecting it.
- Multiple Paintify components can coexist in one project.
- Toggle **Paintify active** off and on to restart the renderer.

## For developers

The rest of the repository is only needed to modify or rebuild Paintify.

- [Building the TOX](docs/building-the-tox.md)
- [Standalone renderer (CLI and GUI), pipeline and measurements](docs/standalone-renderer.md)
- [Architecture](docs/architecture.md)
