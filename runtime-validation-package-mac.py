"""Run inside TouchDesigner; preserve the upstream Mac component and binary."""
from pathlib import Path
import hashlib
import json
from td import op

root_path = Path(__file__).resolve().parent
root = op('/project1')
source_path = root_path / 'Paintify-Mac.tox'
source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
comp = root.loadTox(str(source_path))
comp.par.Active = False
comp.op('mac_runtime').text = (root_path / 'touchdesigner/mac_runtime.py').read_text(encoding='utf-8')
resize = comp.op('working_resolution') or comp.create('resolutionTOP', 'working_resolution')
resize.par.outputresolution.expr = "{'25':'quarter','50':'half','100':'useinput'}[parent().par.Resolution.eval()]"
resize.par.inputfiltertype = 'linear'
comp.op('source').outputConnectors[0].connect(resize.inputConnectors[0])
resize.outputConnectors[0].connect(comp.op('metal_paint').inputConnectors[0])
comp.par.Resolution = '50'
comp.par.Resolution.default = '50'
comp.par.Active = True
output = root_path / 'Paintify-Mac-Optimized.tox'
comp.save(str(output))
record = {
    'platform': 'Windows packaging only; native Metal runtime not executed',
    'original_unchanged': source_hash == hashlib.sha256(source_path.read_bytes()).hexdigest(),
    'output': str(output),
    'embedded_runtime_sha256': hashlib.sha256(comp.op('mac_runtime').text.encode()).hexdigest(),
    'runtime_matches_source': comp.op('mac_runtime').text == (root_path / 'touchdesigner/mac_runtime.py').read_text(encoding='utf-8'),
    'resolution': comp.par.Resolution.eval(),
    'resize_mode': resize.par.outputresolution.eval(),
    'vfs_files': [item.name for item in comp.vfs.find()],
}
(root_path / 'runtime-validation-mac.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
comp.par.Active = False
test_source = root.op('paintify_validation_input')
if test_source is not None:
    test_source.outputConnectors[0].connect(comp.inputConnectors[0])
    resize.cook(force=True)
    comp.op('metal_paint').cook(force=True)
    record['gpu_resize_size'] = [resize.width, resize.height]
    record['bypass_size'] = [comp.op('metal_paint').width, comp.op('metal_paint').height]
    record['bypass_errors'] = comp.errors(recurse=True)
    (root_path / 'runtime-validation-mac.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
print('MAC_OPTIMIZED_PACKAGED', str(output))
