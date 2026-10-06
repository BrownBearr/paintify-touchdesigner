# Paintify for TouchDesigner

Turn any TouchDesigner TOP into a live, GPU-painted oil painting.

## Install (one file)

1. **[Download Paintify.tox](https://github.com/BrownBearr/paintify-touchdesigner/raw/main/Paintify.tox)**
2. Drag it into a TouchDesigner Network Editor.
3. Connect any TOP (Movie File In, Video Device In, ...) to its input and view its output.

That is all. The TOX carries its own renderer and shaders and unpacks them to
`%LOCALAPPDATA%\Paintify` (Windows) or `~/Library/Caches/Paintify` (macOS) the
first time it loads. There is nothing to build, clone or install, and no other
files from this repository are needed.

**Requirements**

- **Windows:** Windows 10/11, TouchDesigner (any recent build), and a GPU with
  OpenGL 4.6. TouchDesigner's own installer provides the VC++ runtime. Frames
  travel over Spout.
- **macOS:** macOS 12 or newer, TouchDesigner 2023 or newer, any Metal GPU
  (Apple Silicon recommended). Frames travel over Syphon. The macOS renderer is
  a Metal port that has **not yet been run on a Mac**; see
  [macOS](docs/macos-touchdesigner-feasibility.md) for status and how to build
  and test it.

TouchDesigner Non-Commercial limits images to 1280x1280. A `Paintify.tox` only
runs on the platforms whose renderer was bundled into it.

## Controls

**Look** picks Impressionist, Expressionist, Pointillist, Wash or Detail. The
main page also has painted FPS, relaxation, brush texture, impasto, temporal
repaint and optical flow. **Brush texture = -1** uses the look's own value.

The **Paintify Advanced** page exposes radii, threshold, curvature, opacity,
stroke lengths, underpaint, tensor smoothing, edge tangent flow, bristle
density, dry brush, lighting angle, jitters and relaxation tuning. Numeric
advanced controls default to `-1`, which keeps the look's preset. Look, FPS
and painting controls apply live without restarting the renderer.

## Troubleshooting

- **Blank output:** check the Textport (Dialogs > Textport). The component
  prints its log path on start; the log lives in
  `%LOCALAPPDATA%\Paintify\<bundle-id>\paintify-live.log` (Windows) or
  `~/Library/Caches/Paintify/<bundle-id>/paintify-live.log` (macOS).
- **macOS, upside-down output or input:** toggle **macOS: flip Syphon input**
  and **macOS: flip Syphon output** on the Paintify Advanced page.
- Verify the input TOP displays an image on its own before connecting it.
- Multiple Paintify components can coexist in one project.
- Toggle **Paintify active** off and on to restart the renderer.

## For developers

The rest of the repository is only needed to modify or rebuild Paintify.

- [Building the TOX](docs/building-the-tox.md)
- [macOS port](docs/macos-touchdesigner-feasibility.md)
- [Standalone renderer (CLI and GUI), pipeline and measurements](docs/standalone-renderer.md)
- [Architecture](docs/architecture.md)
