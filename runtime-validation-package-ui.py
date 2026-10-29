"""Refresh the embedded UI after verified runtime builds, preserving controls."""
from pathlib import Path
from td import op

root_path = Path(__file__).resolve().parent
for filename in ('Paintify-Optimized.tox', 'Paintify-Mac-Optimized.tox'):
    comp = op('/project1').loadTox(str(root_path / filename))
    comp.par.Active = False
    ui = comp.op('paintify_ui')
    ui.text = (root_path / 'touchdesigner/tox_ui.py').read_text(encoding='utf-8')
    ui.module.configure(comp)
    controls = comp.op('paintify_controls') or comp.op('controls_changed')
    controls.par.builtin = False
    comp.par.Guide = 'Connect a TOP'
    comp.par.Active = True
    comp.save(str(root_path / filename))
    comp.par.Active = False
print('FINAL_UI_EMBEDDED')
