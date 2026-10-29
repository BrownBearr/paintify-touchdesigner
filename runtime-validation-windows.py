"""Package and exercise the rebuilt Windows component inside TouchDesigner."""
from pathlib import Path
import hashlib
import json
import os
import runpy
import time
from td import op, run

root_path = Path(__file__).resolve().parent
original = root_path / 'Paintify.tox'
original_hash = hashlib.sha256(original.read_bytes()).hexdigest()
output = root_path / 'Paintify-Optimized.tox'
previous_output = os.environ.get('PAINTIFY_TOX_OUTPUT')
os.environ['PAINTIFY_TOX_OUTPUT'] = str(output)
try:
    runpy.run_path(str(root_path / 'touchdesigner/install_paintify.py'))
finally:
    if previous_output is None:
        os.environ.pop('PAINTIFY_TOX_OUTPUT', None)
    else:
        os.environ['PAINTIFY_TOX_OUTPUT'] = previous_output

root = op('/project1')
comp = root.op('paintify')
source = root.op('paintify_validation_input') or root.create('rampTOP', 'paintify_validation_input')
source.par.outputresolution = 'custom'
source.par.resolutionw, source.par.resolutionh = 640, 360
source.outputConnectors[0].connect(comp.inputConnectors[0])
comp.op('send_to_paintify').cook(force=True)
record = {
    'original_unchanged': original_hash == hashlib.sha256(original.read_bytes()).hexdigest(),
    'packaged_output': str(output),
    'runtime_matches_source': comp.op('paintify_runtime').text == (root_path / 'touchdesigner/paintify_runtime.py').read_text(encoding='utf-8'),
    'renderer_matches_build': any(item.name == 'runtime/gpu-sbr.exe' and hashlib.sha256(item.byteArray).digest() == hashlib.sha256((root_path / 'build-package/gpu-sbr.exe').read_bytes()).digest() for item in comp.vfs.find()),
    'defaults': {name: getattr(comp.par, name).eval() for name in ('Fps', 'Relax', 'Passes', 'Maxdimension')},
    'reset_defaults': {name: getattr(comp.par, name).default for name in ('Fps', 'Relax', 'Passes', 'Maxdimension')},
    'stages': [],
}
startup_deadline = time.monotonic() + 40

def capture(label):
    comp.op('send_to_paintify').cook(force=True)
    result = comp.op('painted')
    result.cook(force=True)
    pixels = result.numpyArray(delayed=False)
    runtime = comp.op('paintify_runtime').module
    entry = runtime._registry().get(comp.id)
    record['stages'].append({
        'label': label,
        'output_size': [result.width, result.height],
        'mean': float(pixels[:, :, :3].mean()) if pixels is not None else None,
        'std': float(pixels[:, :, :3].std()) if pixels is not None else None,
        'errors': comp.errors(recurse=True),
        'renderer_alive': entry is not None and entry[0].poll() is None,
    })
    (root_path / 'runtime-validation-windows.json').write_text(json.dumps(record, indent=2), encoding='utf-8')

def initial():
    comp.op('send_to_paintify').cook(force=True)
    result = comp.op('painted')
    result.cook(force=True)
    if (result.width, result.height) != (640, 360) and time.monotonic() < startup_deadline:
        record['startup_retries'] = record.get('startup_retries', 0) + 1
        run(initial, delayMilliSeconds=1000, delayRef=op.TDResources)
        return
    capture('initial_640x360')
    comp.par.Maxdimension = 320
    run(resized, delayMilliSeconds=4000, delayRef=op.TDResources)

def resized():
    capture('working_limit_320')
    comp.par.Preset = 'pointillist'
    run(changed, delayMilliSeconds=4000, delayRef=op.TDResources)

def changed():
    capture('pointillist_live_update')
    comp.par.Active = False
    run(stopped, delayMilliSeconds=1000, delayRef=op.TDResources)

def stopped():
    record['stopped'] = comp.id not in comp.op('paintify_runtime').module._registry()
    comp.par.Active = True
    run(restarted, delayMilliSeconds=4000, delayRef=op.TDResources)

def restarted():
    capture('restart')
    comp.op('paintify_runtime').module.stop(comp)
    comp.par.Active = False
    record['final_stopped'] = comp.id not in comp.op('paintify_runtime').module._registry()
    (root_path / 'runtime-validation-windows.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print('WINDOWS_RUNTIME_VALIDATION_SAVED')

run(initial, delayMilliSeconds=5000, delayRef=op.TDResources)
print('WINDOWS_VALIDATION_STARTED')
