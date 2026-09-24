# Paintify for TouchDesigner

Paintify is a Windows TouchDesigner component with one TOP input and one TOP
output. It paints incoming images or video on the GPU and shares textures with
TouchDesigner through Spout.

## For users: one-file install

Download [Paintify.tox](../Paintify.tox) from the repository root, then drag it into the
TouchDesigner Network Editor. Connect any TOP to its input and connect its
output to a viewer or another TOP. The TOX contains the renderer executable,
its runtime DLLs, and its shaders. On first use it extracts them to
`%LOCALAPPDATA%\Paintify\<bundle-id>`; no compiler, vcpkg, source checkout,
or separate Paintify installer is needed. The Windows VC++ runtime must
be available. Leave **Renderer executable override** blank
to use the bundled copy.

The component starts automatically when loaded. If the timeline is paused,
its startup callback still runs. On exit or component deletion, it stops its
renderer process. Windows, an OpenGL 4.6 capable GPU, TouchDesigner, and
Spout support are required. The TOX is Windows-only because the bundled
renderer is a Windows executable. A Mac build would need a native GPU
renderer and Syphon integration; this Windows TOX does not run on macOS. See the [macOS feasibility and port plan](../docs/macos-touchdesigner-feasibility.md).

## For maintainers: build the TOX

1. Build the independent renderer with `tools\build-package.bat`. This uses
   MSVC Build Tools, CMake, Ninja, and vcpkg. The package build is separate
   from `build-live`, so a running development instance is unaffected.
2. Open a new blank TouchDesigner project.
3. Open **Dialogs → Textport**, set it to Python, and run:

   ```python
   import runpy; runpy.run_path(r'C:\Users\I3row\paintify-touchdesigner\touchdesigner\install_paintify.py')
   ```

   Replace the path if the repository lives elsewhere.

4. The script creates `Paintify.tox` with all runtime files in
   its virtual file system. Upload this single file as a GitHub release asset.
   Keep source and build scripts in the repository for maintainability.

The installer replaces a component named `paintify` in `/project1`, so build
the release TOX in a blank project.

## Controls

**Look** is a dropdown: Impressionist, Expressionist, Pointillist, Wash,
Detail. The main page also provides painted FPS, relaxation, brush texture,
impasto, temporal repaint, and optical flow. **Brush texture = -1** uses the
chosen look's preset value.

**Paintify Advanced** exposes the other live-compatible GPU-SBR controls:
radii, threshold, curvature, opacity, grid spacing, stroke lengths,
underpaint, painting passes, tensor smoothing, edge tangent flow, bristle
density, texture taper, dry brush, light angle, jitters, relaxation tuning,
and optical flow iterations. Numeric advanced controls default to `-1`,
which leaves the renderer's preset or default intact. Empty radii and
**Underpaint: Use look** likewise leave the preset intact. Look, FPS, and
painting controls update the running renderer without restarting it. Toggling
**Paintify active** or changing the executable override still starts or stops
the renderer.

CLI-only options for loading/saving files, batch rendering, video encoding,
and diagnostics are not relevant to a live TOP component.

## Test and troubleshooting

1. Add a Movie File In TOP or Video Device In TOP and verify it displays an
   image by itself.
2. Connect it to Paintify, then connect Paintify to a Null TOP or viewer.
3. Change **Look** and verify the output changes. Try a threshold override
   on Paintify Advanced, then set it back to `-1`.
4. If the output is blank, open **Dialogs → Textport**. The component prints
   its log path when the renderer starts. The bundled log is under
   `%LOCALAPPDATA%\Paintify\<bundle-id>\paintify-live.log`.
5. TouchDesigner Non-Commercial limits images to 1280×1280.

Multiple components can coexist because their Spout sender names derive from
their operator paths.
