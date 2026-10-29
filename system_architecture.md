# Runtime architecture after performance refinement

Windows uses Spout GPU texture exchange with the C++ OpenGL renderer. Live
settings are atomic text files; `--live-max-dimension` controls aspect-preserving
GPU input downsampling and the published output dimensions. A settings change
marks the cached input dirty so frozen sources repaint. Live mode disables
diagnostic timer queries and nonessential CPU/GPU statistics readbacks; offline
rendering retains diagnostic behavior. The original input texture remains at
sender dimensions, while Pipeline owns working-resolution render targets.

Mac uses a bundled Metal worker from upstream `codex/mac-metal-tox` (`a24bf95`).
`working_resolution` resizes on the GPU before Script TOP readback. The embedded
runtime detects that node by its input name and avoids double scaling; original
components retain the CPU fallback. A cached cook returns before downloading or
converting source pixels when the painted-FPS interval has not elapsed.
Parameter callbacks invalidate serialized settings. Settings files change only
when serialized contents change. Bypass reads the original `source` TOP at its
native dimensions. Metal transport still synchronously reads/writes pixels
through pipes for each painted frame; Mac GPU execution is unverified here.

Optimized artifacts are separate from preserved upstream originals. Windows
installer supports `PAINTIFY_TOX_OUTPUT` for candidate packaging. Re-embedding
runtime DATs is necessary after Python edits; rebuilding the native executable
is necessary after C++ edits. Do not assume changing source updates a saved TOX.
