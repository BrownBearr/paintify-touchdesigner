"""Run in a disposable TouchDesigner project after building the Mac tox."""
from pathlib import Path
import tempfile
import time
import runpy
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
    data, controls = runtime.metadata(comp)
    assert comp.par.Brush.default == "tempera"
    assert abs(comp.par.Opacity.default - .9) < 1e-5
    assert abs(comp.par.Brushsize.default - 1) < 1e-5
    groups = runpy.run_path(str(Path(__file__).resolve().parents[1] / "touchdesigner/controls.py"))["GROUPS"]
    # Exercise every slider's actual TD parameter and serialized worker value.
    comp.store("updating", True)
    for group, fields in groups.items():
        for field, label, low, high in fields:
            par = getattr(comp.par, controls.parname(field, "Bk"))
            low, high = controls.SLIDER_RANGES.get(field, (low, high))
            assert abs(par.normMin - low) < 1e-5 and abs(par.normMax - high) < 1e-5, field
            original = par.eval()
            for value in (low, (low + high) / 2, high):
                par.val = value
                actual = controls.parse(runtime.settings_text(comp), data)["block"][data["fields"].index(field)]
                assert abs(actual - value) < 1e-4, (field, value, actual)
            par.val = original
    for key, label, default, low, high, integer in controls.CORE:
        par = getattr(comp.par, controls.parname(key))
        original = par.eval()
        for value in (low, high):
            par.val = value
            actual = controls.parse(runtime.settings_text(comp), data)["core"][key]
            assert abs(actual - value) < 1e-4, (key, value, actual)
        par.val = original
    for value in ("0", "0.5", "1"):
        comp.par.Bkfinish = value
        assert runtime.block(comp)[data["fields"].index("finish")] == float(value)
    restored = controls.select_brush(data, "tempera", data["defaults"])
    restored[data["fields"].index("finish")] = .5
    runtime.apply_block(comp, restored)
    assert comp.par.Bkfinish.eval() == "0.5", "Brush relief state lost"
    comp.par.Bkpalettecount = 99
    assert comp.par.Bkpalettecount.eval() == 32, "Palette count must clamp to 32"
    comp.par.Bkpalettecount = 0
    assert comp.par.Bksaturation.page.name == "Paintify"
    comp.store("updating", False)
    comp.par.Minlength = 20
    runtime.change(comp, comp.par.Minlength)
    assert comp.par.Maxlength.eval() >= comp.par.Minlength.eval()
    comp.par.Minlength, comp.par.Maxlength = 4, 16
    painted = pixels()
    assert comp.par.Status.eval() == "Painting", comp.par.Status.eval()
    raw = source.numpyArray(delayed=False)
    assert float(np.abs(painted - raw).mean()) > .005
    assert painted[-8:,:,1].mean() > painted[:8,:,1].mean(), "Vertical orientation reversed"
    for name, value in (("Brushsize", 2), ("Anglejitter", 35), ("Opacity", .25)):
        par = getattr(comp.par, name)
        original = par.eval()
        par.val = value
        assert float(np.abs(pixels() - painted).mean()) > .001, name + " did not change rendered pixels"
        par.val = original
        painted = pixels()
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
    relocated = root.create("baseCOMP", "paintify_portability_test")
    loaded = relocated.loadTox(str(path))
    loaded.name = "portable_paint"
    assert loaded.op("metal_paint").par.callbacks.eval() == loaded.op("paint_callbacks"), "Callback points outside the saved component"
    viewer = loaded.par.opviewer.eval()
    target = op(viewer) if isinstance(viewer, str) else viewer
    assert target == loaded.op("painted"), ("Viewer points outside the saved component", repr(viewer))
    assert loaded.op("mac_runtime") is not None and len(loaded.vfs.find()) > 10
    assert loaded.par.Brush.eval() == "tempera"
    relocated_source = relocated.copy(source)
    relocated_source.par.callbacks = callbacks.path
    relocated_source.cook(force=True)
    relocated_source.outputConnectors[0].connect(loaded.inputConnectors[0])
    loaded.op("source").cook(force=True)
    loaded.op("metal_paint").cook(force=True)
    loaded_output = loaded.op("painted")
    loaded_output.cook(force=True)
    reloaded_pixels = loaded_output.numpyArray(delayed=False)
    assert loaded.par.Status.eval() == "Painting", loaded.par.Status.eval()
    assert reloaded_pixels is not None and reloaded_pixels.shape == (64, 96, 4), {
        "shape": None if reloaded_pixels is None else reloaded_pixels.shape,
        "input": (loaded.op("source").width, loaded.op("source").height),
        "paint": (loaded.op("metal_paint").width, loaded.op("metal_paint").height),
        "errors": loaded.errors(recurse=True),
    }
    assert float(np.abs(reloaded_pixels - raw).mean()) > .005
    loaded.op("mac_runtime").module.stop(loaded)
    loaded.destroy()
    relocated.destroy()
    print("PAINTIFY_TOUCHDESIGNER_SMOKE_OK", flush=True)
finally:
    runtime.stop(comp)
    source.destroy()
    callbacks.destroy()
