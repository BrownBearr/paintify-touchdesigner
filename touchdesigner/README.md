# Paintify for TouchDesigner

Paintify is a reusable TouchDesigner component with one TOP input and one TOP
output. Any TOP can feed it: Movie File In TOP for MP4 files, Video Device In TOP
for cameras/capture cards, Video Stream In TOP for network feeds, or another
TouchDesigner composition. The output is the existing GPU Hertzmann renderer's
curved strokes, brush texture and optional impasto/relaxation.

## Install on this computer

**Windows:** run `tools\build-live.bat` from this repository to build the live
renderer in `build-live/gpu-sbr.exe`.

**macOS** (Apple Silicon or Intel): run `tools/build-mac.sh`, which builds
`build/gpu-sbr`. The component and everything below are the same on both;
the bridge is Syphon instead of Spout, which TouchDesigner's Syphon Spout
In/Out TOPs already speak on a Mac.

In TouchDesigner, open
**Dialogs → Textport**, set the Textport to Python, and run this one line:

```python
import runpy, pathlib; runpy.run_path(str(pathlib.Path.home() / 'paintify-touchdesigner' / 'touchdesigner' / 'install_paintify.py'))
```

The installer creates a `paintify` component in `/project1` and saves
`touchdesigner/Paintify.tox`. No code editing is required. The `.tox` can be
dragged into other TouchDesigner projects. The component holds the renderer
executable path in its **Paintify → Renderer executable** parameter.

## Use

1. Add a **Movie File In TOP** for an MP4, or a **Video Device In TOP** for a
   camera. Connect its output to the `paintify` component's input.
2. Connect the `paintify` output to a viewer, a Window COMP, or a Movie File Out
   TOP. The component starts its hidden GPU renderer when loaded and stops it
   when TouchDesigner exits or the component is removed.
3. The default is **12 painted frames per second**. TouchDesigner may display
   those frames on a faster timeline, holding each painted image in between.
   Set the Movie File Out TOP's output rate to 12 if you want a 12 fps file.

The Paintify page exposes **Look**, **Relaxation iterations**, **Brush texture**,
**Impasto height**, **Impasto lighting**, **Temporal repaint threshold**, and
**Optical flow levels**. Changing a control restarts the hidden renderer so
the new look takes effect. Set **Paintify active** off to stop it.

Live texture sharing uses Spout on the same Windows computer. There is no CPU
readback in the normal input/output path. A log is written to
`build-live/paintify-live.log` if a sender is missing or the bridge fails.

On macOS it uses Syphon, and frames cross through the CPU: the renderer reads
TouchDesigner's IOSurface, paints on the GPU through MoltenVK, and writes the
painting into its own IOSurface. At 12 painted frames per second that costs a
few milliseconds a frame on Apple Silicon's unified memory. The log is
`build/paintify-live.log`. Syphon carries 8-bit BGRA frames, so the painting
comes back 8-bit whatever the input TOP's format. If the Syphon Spout In TOP
inside the component shows no image while the log reports painted frames,
pick the `Paintify_…_output` entry from that TOP's sender menu.

TouchDesigner Non-Commercial limits images to 1280×1280, so full 1920×1080
requires a license without that limit. The standalone renderer remains able to
process 1080p independently of TouchDesigner's license.

## Architecture and limits

The In TOP feeds a Syphon Spout Out TOP. The hidden `gpu-sbr.exe --live-spout`
process receives that texture, copies it into the renderer's source texture,
paints on the GPU, and publishes the canvas to a Syphon Spout In TOP. An Out TOP
exposes the painting from the component. Input resize resets temporal state.

The renderer is paced at 12 fps, but actual throughput depends on capture,
Spout transfer, brush settings, resolution and the rest of the TouchDesigner
network. Moving footage with temporal repaint and optical flow can still soften
or flicker: optical flow carries the painted canvas rather than persistent
stroke objects. For a deliberately repainted stop-motion look, leave Temporal
repaint threshold at zero.
