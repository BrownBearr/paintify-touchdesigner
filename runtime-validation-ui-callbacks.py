"""Verify real Parameter Execute callbacks in the saved TOXs."""
from pathlib import Path
import json
from td import op, run

root_path = Path(__file__).resolve().parent
root = op('/project1')
components = {}
results = {}
for key, filename in (('windows', 'Paintify-Optimized.tox'), ('mac', 'Paintify-Mac-Optimized.tox')):
    comp = root.loadTox(str(root_path / filename))
    comp.par.Active = False
    components[key] = comp
    results[key] = {'look': (comp.par.Brush if key == 'mac' else comp.par.Preset).eval(), 'fps': comp.par.Fps.eval()}

def begin_preview():
    # Parameter Execute snapshots initial values on the first loaded frame.
    # Exercise a user edit after that initialization, not an initial value.
    for comp in components.values():
        comp.par.Performance = 'preview'
    run(check_preview, delayFrames=3, delayRef=op.TDResources)

def check_preview():
    for key, comp in components.items():
        expected = comp.op('paintify_ui').module.modes(comp)['preview']
        actual = {name: getattr(comp.par, name).eval() for name in expected}
        assert actual == expected, (key, actual, expected, comp.par.Performance.eval())
        results[key]['preset_callback'] = True
        par = comp.par.Resolution if key == 'mac' else comp.par.Maxdimension
        par.val = '50' if key == 'mac' else 1920
    run(check_manual, delayFrames=3, delayRef=op.TDResources)

def check_manual():
    for key, comp in components.items():
        assert comp.par.Performance.eval() == 'custom', key
        assert comp.par.Fps.eval() == results[key]['fps']
        look = (comp.par.Brush if key == 'mac' else comp.par.Preset).eval()
        assert look == results[key]['look']
        assert comp.currentPage.name == 'Paintify'
        assert comp.showCustomOnly
        results[key].update(manual_callback=True, original_look_preserved=True,
                            original_fps_preserved=True, errors=comp.errors(recurse=True))
    (root_path / 'runtime-validation-ui-callbacks.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print('NATIVE_UI_CALLBACKS_VALIDATED')

run(begin_preview, delayFrames=3, delayRef=op.TDResources)
