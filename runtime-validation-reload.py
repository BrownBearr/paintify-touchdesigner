"""Verify the saved distributable components load and embed current code."""
from pathlib import Path
import hashlib
import json
from td import op

root_path = Path(__file__).resolve().parent
root = op('/project1')
results = {}
for filename, dat_name, source_file in (
    ('Paintify-Optimized.tox', 'paintify_runtime', 'paintify_runtime.py'),
    ('Paintify-Mac-Optimized.tox', 'mac_runtime', 'mac_runtime.py'),
):
    comp = root.loadTox(str(root_path / filename))
    comp.par.Active = False
    result = {
        'loaded': True,
        'runtime_matches_source': comp.op(dat_name).text == (root_path / 'touchdesigner' / source_file).read_text(encoding='utf-8'),
        'sha256': hashlib.sha256((root_path / filename).read_bytes()).hexdigest(),
        'bytes': (root_path / filename).stat().st_size,
    }
    if dat_name == 'paintify_runtime':
        result['reset_defaults'] = {name: getattr(comp.par, name).default for name in ('Fps', 'Relax', 'Passes', 'Maxdimension')}
        result['renderer_matches_build'] = any(item.name == 'runtime/gpu-sbr.exe' and hashlib.sha256(item.byteArray).digest() == hashlib.sha256((root_path / 'build-package/gpu-sbr.exe').read_bytes()).digest() for item in comp.vfs.find())
    else:
        result['resolution_default'] = comp.par.Resolution.default
        result['resize_mode'] = comp.op('working_resolution').par.outputresolution.eval()
    results[filename] = result
assert all(result['runtime_matches_source'] for result in results.values())
assert results['Paintify-Optimized.tox']['renderer_matches_build']
(root_path / 'runtime-validation-reload.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
print('SAVED_ARTIFACT_RELOAD_VERIFIED')
