# Building the Paintify TOX

End users only need `Paintify.tox` from the repository root (see the main README). This page is for maintainers.

Paintify is a Windows TouchDesigner component with one TOP input and one TOP
output. It paints incoming images or video on the GPU and shares textures with
TouchDesigner through Spout.

## For users: one-file install

Download [Paintify.tox](../Paintify.tox) from the repository root, then drag it into the
TouchDesigner Network Editor. Connect any TOP to its input and connect its
output to a viewer or another TOP. The component flips the returned Spout texture vertically so the painted output matches the input orientation. The TOX contains the renderer executable,
its runtime DLLs, and its shaders. On first use it extracts them to
`%LOCALAPPDATA%\Paintify\<bundle-id>`; no compiler, vcpkg, source checkout,
or separate Paintify installer is needed. The Windows VC++ runtime must
be available. Leave **Renderer executable override** blank
to use the bundled copy.

The component starts automatically when loaded. If the timeline is paused,
its startup callback still runs. On exit or component deletion, it stops its
renderer process. This TOX uses Windows, OpenGL 4.6, TouchDesigner, and Spout.
The separate Mac Metal worker and its packaged TOX use a different runtime and
are maintained independently of this Windows Spout component.

## For maintainers: build the TOX

1. Build the independent renderer with `tools\build-package.bat`. This uses
   MSVC Build Tools, CMake, Ninja, and vcpkg. The package build is separate
   from `build-live`, so a running development instance is unaffected.
2. Open a new blank TouchDesigner project.
3. Open **Dialogs → Textport**, set it to Python, and run:

   ```python
   import os, runpy
   os.environ['PAINTIFY_TOX_OUTPUT'] = r'C:\Users\I3row\AI-Work\ChatGPT\Paintify-Touchdesigner-latest-20261029\Paintify-candidate.tox'
   runpy.run_path(r'C:\Users\I3row\AI-Work\ChatGPT\Paintify-Touchdesigner-latest-20261029\touchdesigner\install_paintify.py')
   ```

4. The installer embeds the `build-package` executable, DLLs and repository
   shaders, then saves the candidate at the requested path. Reload that copy
   in a fresh TouchDesigner project and validate it before replacing the
   checked-in Windows `Paintify.tox`. The Mac `Paintify-Mac.tox` has its own
   installer and packaging path.

The installer replaces a component named `paintify` in `/project1`, so build
the release TOX in a blank project.

## Controls

**Look** is a dropdown: Impressionist, Expressionist, Pointillist, Wash,
Detail. The main page also provides painted FPS, maximum working dimension,
relaxation, brush texture, impasto, temporal repaint, and optical flow.
Maximum working dimension defaults to 1280 with aspect ratio preserved; set it
to 0 for native-size rendering. This cap changes rendering resolution and
therefore visible detail. **Brush texture = -1** uses the chosen look's preset
value.

**Paintify Advanced** exposes the other live-compatible GPU-SBR controls:
radii, threshold, curvature, opacity, grid spacing, stroke lengths,
underpaint, painting passes, tensor smoothing, edge tangent flow, bristle
density, texture taper, dry brush, light angle, jitters, relaxation tuning,
and optical flow iterations. Numeric advanced controls default to `-1`,
which leaves the renderer's preset or default intact. Empty radii and
**Underpaint: Use look** likewise leave the preset intact. The interactive
defaults are **Relax = 0** and **Passes = 4**. For quality output, set **Passes
= 8** and **Relax = 4**; relaxation repaints the full canvas per iteration and
can sharply reduce live throughput. Look, FPS, and painting controls update
the running renderer without restarting it. Toggling **Paintify active** or
changing the executable override still starts or stops the renderer.

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
