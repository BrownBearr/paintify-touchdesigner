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

## Native TOX UI and second runtime pass

`touchdesigner/tox_ui.py` is embedded as the `paintify_ui` DAT in both TOXs.
It moves parameters between native pages without renaming/removing controls,
sets contextual help and soft slider maxima, and embeds `paintify_help`.
UI callbacks run before renderer callbacks and consume readonly status/help
changes. Performance presets only change their mapped fields; manual changes
mark them Custom. Main controls open first through `currentPage` and
`showCustomOnly`; builtin settings remain accessible via the native flag.
Parameter Execute observes an explicit custom-parameter scope. Preset changes
are deferred one frame outside that callback, so its own baseline sees the
new field values and later manual edits back to original values still fire.
Readonly/internal fields and builtin tab changes do not touch render settings.

Watchers resolve stable component IDs. Windows restarts path-derived Spout
channels on rename; Mac retains the worker. Delayed watcher guards/tokens
prevent duplicate chains. Windows status and log paths are readonly controls.
Mac cached frames upload only when the output Script TOP or pixel object
changes; real TD retention was verified before applying this optimization.
