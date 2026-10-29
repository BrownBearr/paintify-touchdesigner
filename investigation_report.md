# Investigation and implementation evidence

## Revisions and platform discovery

- Windows main checkout: `49c719b5686440d4b7d80d1b646fb3b571697dcc`.
- GitHub releases API returned no published releases. Mac implementation exists on remote branch `codex/mac-metal-tox`, revision `a24bf95`, including `Paintify-Mac.tox` and Metal sources. Initial main-only platform findings in Terra reports are superseded by this discovery.
- GPU reference was inspected separately; see `investigation_reference.md`. Its 23 Brushkit families are present in the Mac branch; the older Windows renderer has five looks. Full Windows style parity is a larger renderer migration.
- Original TOX binaries are preserved; optimized artifacts use separate filenames.

## High-cost paths

- Windows original component defaults to four relaxation rounds. Each adds full-canvas optimization/repaint work. Live defaults now favor four painting passes and zero relaxation, with quality controls retained.
- Windows pipeline diagnostic counter reads may block the CPU waiting on GPU work; live mode can omit diagnostics.
- Original Windows Spout path always renders incoming resolution. A max-dimension cap avoids scaling work quadratically with source size; 3840x2160 to1280x720 processes one ninth of the pixels.
- Mac Script TOP downloaded full source, converted/copy-hashed pixels and serialized controls before checking its requested FPS. Early cache return avoids this work on skipped cooks. Default50% rendering processes one quarter of native pixels, with full resolution available.
- Mac transport remains CPU/pipe based and synchronous per painted frame. A future native TOP/IOSurface or asynchronous worker architecture is required to eliminate that stall; this patch does not claim that migration.

See individual Terra reports for file-level evidence. Validation results and runtime packaging are recorded separately after completion.
