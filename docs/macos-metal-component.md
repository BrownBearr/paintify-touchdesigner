# Paintify for TouchDesigner on macOS

The Mac component uses the updated Brushkit / Paintify-GPU Metal renderer,
including the 23 procedural brush families, independent colour and finish,
radius sets, tensor / ETF direction, optical flow, relaxation, ground and effects.
The original Windows `Paintify.tox` remains unchanged.

## Component

`Paintify-Mac.tox` has one TOP input and one painted TOP output. Drag it into a
network, connect a TOP, and turn **Live Paint** on. Off bypasses painting and
stops the worker. **Brush family** changes only the brush material; it does not
reset colour, ground, finish, direction or effects. The default is Tempera.

Use **Painted FPS** and **Resolution** to control the rendering cost. Still
inputs are cached until pixels or settings change. Colour palettes have up to
32 pigments, with RGB swatches. Brushkit `.sbr` files preserve the full style
block, palette and imported per-layer overrides. **Use global layer controls**
clears those overrides when global controls should take over.

## Runtime

This implementation paints on Metal but downloads and uploads frames at the
TouchDesigner boundary. It is not a zero-copy Syphon implementation, and no
real-time frame-rate guarantee is implied. Input and output are RGBA8 at this
boundary; HDR inputs are clamped. The first frame compiles the GPU shaders and
can take several seconds. No separate Brushkit installation is needed.

The signed (ad-hoc, not notarized) renderer, compiler and shaders are embedded
in the component VFS. They extract into
`~/Library/Caches/Paintify/TouchDesigner/<bundle-hash>/PaintifyMetal.app`.
Each component gets a separate renderer and settings file. A watchdog stops
the worker after deletion; the exit callback stops it when the project closes.
The main **Paintify** page shows renderer status, including missing input and
errors. The **Look Files** page includes a per-instance log path.

Target: Apple Silicon, macOS 13+, TouchDesigner 2025. The development Mac uses
TouchDesigner 2025.31550 and an M1. Intel and other builds are unverified.

## Build

1. `python3 tools/sync_mac.py /path/to/Paintify-GPU /path/to/Paintify-Blender`
2. `cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=Release`
3. `cmake --build build-metal -j 6 --target paintify-metal`
4. `python3 tools/package_mac.py`
5. In a separate blank TouchDesigner session, run
   `import runpy; runpy.run_path('/path/to/repo/touchdesigner/install_mac.py', run_name='__main__')`.
   This creates a new `paintify_mac` component and saves `Paintify-Mac.tox`.

`tools/build_tox_mac.py` uses Derivative's `toeexpand` / `toecollapse` utilities
to launch a temporary builder component without touching an existing project.
A startup or licensing dialog must be dismissed before callbacks can run.

Tests: `python3 -m unittest discover -s tests -p 'test_*.py'` and
`python3 tools/test_mac_worker.py` (requires Metal and the packaged renderer).
