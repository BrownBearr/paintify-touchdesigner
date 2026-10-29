# Windows renderer GPU timings

Measured 2026-10-29 on an NVIDIA GeForce RTX 3060 Ti, driver 617.42, OpenGL
4.6, using `build-package/gpu-sbr.exe`. The source was a seeded synthetic
gradient with noise and 100 colored circles at 1920×1080. The 1280×720 case is
that same image resized with Pillow Lanczos, preserving aspect ratio.

The benchmark ran each case once as a warm-up, then four measured fresh-process
runs in an interleaved order. `GpuTotalMs` is the renderer's sum of GPU stage
timer queries, not end-to-end process time. The offline CLI has diagnostics
enabled; process wall time includes context setup, shader startup, PNG I/O and
process launch, so it is not a live-FPS measurement. See `results.csv` for all
samples and `raw-runs.txt` for the renderer output.

| Input | Passes | Relax | Median GPU total | Range |
|---|---:|---:|---:|---:|
| 1280×720 | 4 | 0 | 1.305 ms | 1.28–1.32 ms |
| 1280×720 | 8 | 0 | 1.915 ms | 1.87–1.95 ms |
| 1280×720 | 4 | 4 | 12.525 ms | 11.11–12.90 ms |
| 1920×1080 | 4 | 0 | 2.120 ms | 2.10–2.14 ms |

On this input and GPU, eight passes took about 47% longer than four passes at
1280×720. Four relaxation iterations raised the measured total to about
9.6× the four-pass, no-relax case; the relaxation stage itself had a 10.45 ms
median. The relaxed run also emitted NVIDIA OpenGL warnings that a buffer was
being copied from video memory to host memory. These timings show a large
interactive cost for relaxation, but do not measure its image-quality benefit.

The 1920×1080 eight-pass case was unstable (2.81–12.89 ms across four runs)
while the GPU had other active workloads, so it is deliberately omitted from
the summary. Do not use that case to estimate a pass scaling factor.

This is an offline renderer benchmark. It does not exercise a TouchDesigner
TOP, Spout transfer, the live downsample path, settings reloads, or frozen
sender behavior. Live mode disables GPU timer queries and synchronous stroke
statistics readbacks, but this benchmark does not quantify the speedup from
that diagnostics gate. The 1280×720 comparison resizes the input before the
renderer; it does not include the live GPU downsample cost.

To reproduce, build `build-package/gpu-sbr.exe`, generate the two source images
as described above, then run `run_benchmark.ps1` from PowerShell in this folder.
