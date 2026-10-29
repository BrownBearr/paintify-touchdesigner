# Runtime audit, round 2

- Both process watchers resolve the component by its original path and stop on rename or move. TouchDesigner documents `td.op(id)` and stable IDs across rename. Resolve by ID and stop only when absent. On Windows, restart the renderer when the path changes because the documented Spout names derive from that path; guard against duplicate watcher chains during restart. On Mac, refreshing the stored path needs no worker restart. Test rename, move, destruction, restart, and duplicate watcher scheduling in TouchDesigner.
- Mac `change()` dirties settings for runtime-owned `Status` and `Logfile` updates. Exclude those fields. `Savelook` does not mutate settings; `Loadlook` and `Globallayers` do and must invalidate them. Test the callback cases and painting after real edits.
- Mac cached cooks still upload a NumPy array. The documentation specifies `copyNumpyArray` but does not guarantee output retention when a Script TOP cooks without writing. Do not remove uploads without a real TouchDesigner test that compares output pixels across skipped cooks, resolution changes, bypass, reconnect, and restart. The existing unit test expects an upload.
- Windows read-only status changes must be filtered from the wildcard parameter callback to avoid redundant update or recursion. Restart pulse must explicitly restart while respecting Active.

References: https://docs.derivative.ca/OP_Class, https://docs.derivative.ca/Td_Module, https://docs.derivative.ca/ScriptTOP_Class.

## Implemented and checked

Windows now tracks the component by stable ID, restarts the path-derived Spout
transport after rename/move, reports process state, supports Restart, and cleans
failed/exited processes. Watcher guards remain in place across quick stop/start
to prevent duplicate delayed callback chains. Mac watchers use worker tokens
and stable component IDs; internal Status/Logfile writes and nonmutating pulses
no longer invalidate serialized render settings.

The parent verified actual Script TOP output retention in TouchDesigner
2025.33070 (`runtime-validation-cache.json`, retained=true, no errors). Cached
Mac output now skips `copyNumpyArray` only when the same Script TOP already owns
the same pixel object. New painted arrays and replacement Script TOPs upload
once. This retains every rendering feature and does not alter quality defaults.

Final runtime review caught and corrected both a queued-watcher race and a
new-painted-frame upload invalidation issue, with regression tests. All 27
Python tests pass; syntax and scoped whitespace checks pass. Native Metal
execution remains unverified on this Windows host.
