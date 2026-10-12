"""Run in a disposable TouchDesigner project after building the Mac tox."""
from pathlib import Path
import tempfile
import time
import numpy as np
from td import op

comp = component
root = comp.parent()
source = root.create("scriptTOP", "paintify_test_source")
callbacks = root.create("textDAT", "paintify_test_source_callbacks")
callbacks.text = '''def onCook(scriptOp):
    import numpy as np
    y, x = np.mgrid[0:64, 0:96].astype(np.float32)
    pixels = np.ones((64, 96, 4), dtype=np.float32)
    pixels[:,:,0] = x / 96
    pixels[:,:,1] = y / 64
    pixels[:,:,2] = .4
    scriptOp.copyNumpyArray(pixels)
'''
source.par.callbacks = callbacks.path
source.outputConnectors[0].connect(comp.inputConnectors[0])
runtime = comp.op("mac_runtime").module
comp.par.Fps = 60
output = comp.op("painted")

def pixels():
    entry = runtime.registry().get(comp.id)
    if entry:
        entry["last"] = 0
    source.cook(force=True)
    output.cook(force=True)
    result = output.numpyArray(delayed=False)
    assert result is not None and result.shape == (64, 96, 4), (result.shape if result is not None else None)
    return result.copy()

try:
    painted = pixels()
    assert comp.par.Status.eval() == "Painting", comp.par.Status.eval()
    raw = source.numpyArray(delayed=False)
    assert float(np.abs(painted - raw).mean()) > .005
    assert painted[-8:,:,1].mean() > painted[:8,:,1].mean(), "Vertical orientation reversed"
    comp.par.Bksaturation = .2
    changed = pixels()
    assert float(np.abs(changed - painted).mean()) > .001
    comp.par.Brush = "knife"
    runtime.change(comp, comp.par.Brush)
    assert abs(comp.par.Bksaturation.eval() - .2) < .0001, "Brush selection reset colour"
    assert float(np.abs(pixels() - changed).mean()) > .001
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "look.sbr"
        comp.par.Lookfile = str(path)
        runtime.pulse(comp, "Savelook")
        comp.par.Bksaturation = .9
        runtime.pulse(comp, "Loadlook")
        assert abs(comp.par.Bksaturation.eval() - .2) < .0001
    comp.par.Active = False
    bypass = pixels()
    assert np.array_equal(bypass, raw), "Inactive component must bypass input unchanged"
    assert comp.id not in runtime.registry()
    # Load the actual distributable again, not only the builder's live object.
    path = Path(__file__).resolve().parents[1] / "Paintify-Mac.tox"
    loaded = root.loadTox(str(path))
    assert loaded.op("mac_runtime") is not None and len(loaded.vfs.find()) > 10
    assert loaded.par.Brush.eval() == "tempera"
    source.outputConnectors[0].connect(loaded.inputConnectors[0])
    loaded_output = loaded.op("painted")
    loaded_output.cook(force=True)
    reloaded_pixels = loaded_output.numpyArray(delayed=False)
    assert loaded.par.Status.eval() == "Painting", loaded.par.Status.eval()
    assert reloaded_pixels is not None and reloaded_pixels.shape == (64, 96, 4)
    assert float(np.abs(reloaded_pixels - raw).mean()) > .005
    loaded.op("mac_runtime").module.stop(loaded)
    loaded.destroy()
    print("PAINTIFY_TOUCHDESIGNER_SMOKE_OK", flush=True)
finally:
    runtime.stop(comp)
    source.destroy()
    callbacks.destroy()
