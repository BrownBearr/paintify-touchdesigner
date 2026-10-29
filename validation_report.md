# Validation

## Automated checks (Windows, 2026-10-29)

- `tools\build-package.bat`: native renderer and Spout smoke target build successfully with MSVC/vcpkg.
- `ctest --test-dir build-package --output-on-failure`: 1/1 passed, including landscape/portrait cap, native/no-upscale and extreme aspect cases.
- `python -m unittest discover -s tests -p 'test_*.py'`: 12 passed, including unchanged settings writes, legacy components, early Mac throttle before GPU download, and control invalidation.
- `python -m compileall -q touchdesigner tests tools`: passed.
- `git diff --check`: clean.

## Real TouchDesigner validation

TouchDesigner 2025.33070 is installed and licensed. Tests were conducted in a
local validation project outside OneDrive. Machine-readable details are in
`runtime-validation-windows.json` and `runtime-validation-mac.json`.
Windows input/output and live changes were exercised; final lifecycle results
are recorded in those reports. Embedded runtime source and native binary hashes
are checked during final packaging. Original TOXs remain preserved.

Final Windows run produced nonblank 640x360 output, changed to 320x180 after
the working limit changed, applied Pointillist live, stopped on the Active
toggle, restarted successfully, and shut down cleanly. All captured component
errors were empty. Startup required one extra one-second readiness retry.
Saved Windows and Mac candidates were independently reloaded successfully;
`runtime-validation-reload.json` confirms embedded source and Windows binary
match the final files, and reset defaults match the optimized settings.
The Mac network resized 640x360 to 320x180 before readback and preserved
640x360 output when bypassed, with no component errors in that bypass test.

## GPU measurement

Actual offline renderer measurements run on the local RTX 3060 Ti. Raw logs and
CSV are under `benchmark/td-performance`. These do not measure the Spout bridge,
TouchDesigner cook cost, or synchronization savings in live mode. Lower passes,
relaxation and resolution trade image detail/refinement for lower rendering cost.

After warm-up, four interleaved samples per case gave median GPU stage totals:
720p four passes/no relaxation **1.305 ms** (1.28-1.32), eight passes/no
relaxation **1.915 ms** (1.87-1.95), four passes/four relaxation rounds
**12.525 ms** (11.11-12.90), and 1080p four passes/no relaxation **2.120 ms**
(2.10-2.14). The relaxation stage alone had median **10.45 ms**. The 1080p
eight-pass case was too noisy for a reliable summary. See benchmark README for
the paired synthetic input and reproducible command sequence.

## Limits

Native Metal execution and Mac performance are untested on this Windows machine.
Mac bundle editing, controls, source hashes and GPU resize network can be checked
here, but an Apple Silicon Mac must verify end-to-end Metal painting. The Mac
worker still transfers pixels through synchronous pipes. Windows retains its
older five-look renderer; full 23-family Brushkit parity is not implemented.
