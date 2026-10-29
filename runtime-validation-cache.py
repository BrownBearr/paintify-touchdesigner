"""Check actual Script TOP behavior when a cached cook returns no pixels."""
from pathlib import Path
import json
from td import op

root = op('/project1')
node = root.create('scriptTOP', 'paintify_cache_probe')
callback = root.create('textDAT', 'paintify_cache_probe_callbacks')
node.par.callbacks = callback.path
callback.text = '''def onCook(scriptOp):
    import numpy as np
    if not scriptOp.fetch('initialized', False):
        pixels = np.ones((16, 32, 4), dtype=np.float32)
        pixels[:, :, 0] = .25
        scriptOp.copyNumpyArray(pixels)
        scriptOp.store('initialized', True)
'''
node.cook(force=True)
first = node.numpyArray(delayed=False).copy()
node.cook(force=True)
second = node.numpyArray(delayed=False)
record = {'first_shape': list(first.shape), 'second_shape': list(second.shape),
          'retained': bool((first == second).all()), 'errors': node.errors()}
Path(__file__).with_suffix('.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
node.destroy()
callback.destroy()
print('SCRIPT_TOP_CACHE_PROBE', record)
