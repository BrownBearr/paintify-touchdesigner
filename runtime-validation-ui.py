"""Exercise real native parameters and preserve every upstream control."""
from pathlib import Path
import json
from td import op

root_path = Path(__file__).resolve().parent
root = op('/project1')
results = {}
components = {}
for platform, filename in (('windows', 'Paintify.tox'), ('mac', 'Paintify-Mac.tox')):
    comp = root.loadTox(str(root_path / filename))
    comp.par.Active = False
    comp.store('updating', True)
    runtime_name = 'mac_runtime' if platform == 'mac' else 'paintify_runtime'
    runtime_source = 'mac_runtime.py' if platform == 'mac' else 'paintify_runtime.py'
    comp.op(runtime_name).text = (root_path / 'touchdesigner' / runtime_source).read_text(encoding='utf-8')
    before = {p.name: p.eval() for p in comp.customPars}
    ui = comp.create('textDAT', 'paintify_ui')
    ui.text = (root_path / 'touchdesigner/tox_ui.py').read_text(encoding='utf-8')
    ui.module.configure(comp)
    after = {p.name: p.eval() for p in comp.customPars}
    missing = sorted(set(before) - set(after))
    changed = sorted(name for name in before if before[name] != after.get(name))
    assert not missing and not changed, (platform, missing, changed)
    look_name = 'Brush' if platform == 'mac' else 'Preset'
    look = getattr(comp.par, look_name).eval()
    fps = comp.par.Fps.eval()
    presets = {}
    for key, expected in ui.module.modes(comp).items():
        comp.par.Performance = key
        ui.module.change(comp, comp.par.Performance)
        actual = {name: getattr(comp.par, name).eval() for name in expected}
        assert actual == expected, (key, actual, expected)
        assert getattr(comp.par, look_name).eval() == look and comp.par.Fps.eval() == fps
        presets[key] = actual
    manual = comp.par.Resolution if platform == 'mac' else comp.par.Maxdimension
    manual.val = '25' if platform == 'mac' else 1920
    ui.module.change(comp, manual)
    assert comp.par.Performance.eval() == 'custom'
    assert comp.par.Status.readOnly and comp.par.Guide.readOnly
    assert len(comp.op('paintify_help').text) > 500
    main = next(page for page in comp.customPages if page.name == 'Paintify')
    results[platform] = {
        'all_original_controls_preserved': not missing,
        'original_control_count': len(before),
        'values_preserved_during_upgrade': not changed,
        'pages': [page.name for page in comp.customPages],
        'main_parameters': [p.name for p in main.pars],
        'presets': presets,
        'manual_switches_to_custom': comp.par.Performance.eval() == 'custom',
        'fps_and_look_preserved': True,
        'fps_tooltip': comp.par.Fps.help,
        'errors': comp.errors(recurse=True),
    }
    comp.par.Performance = 'balanced'
    ui.module.change(comp, comp.par.Performance)
    components[platform] = comp
(root_path / 'runtime-validation-ui.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
components['windows'].openParameters()
print('NATIVE_UI_VALIDATED', {key: value['original_control_count'] for key, value in results.items()})
