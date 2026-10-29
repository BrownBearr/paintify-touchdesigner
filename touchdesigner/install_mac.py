"""Build the self-contained Paintify-Mac.tox inside TouchDesigner."""
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "touchdesigner"))
import controls
import mac_controls


def install():
    from td import op
    data = json.loads((ROOT / "touchdesigner/brushkit_data.json").read_text())
    root = op("/project1") or op("/")
    # Use a distinct name; never replace the user's existing Windows component.
    comp = root.create("baseCOMP", "paintify_mac")
    comp.store("updating", True)
    page = comp.appendCustomPage("Paintify")
    page.appendToggle("Active", label="Live Paint")
    page.appendMenu("Brush", label="Brush family")
    comp.par.Brush.menuNames = [b["key"] for b in data["brushes"]]
    comp.par.Brush.menuLabels = [b["name"] for b in data["brushes"]]
    page.appendMenu("Radiusset", label="Radius set")
    comp.par.Radiusset.menuNames = list(mac_controls.RADIUS_SETS) + ["custom"]
    comp.par.Radiusset.menuLabels = ["Balanced", "Broad", "Fine", "Large wash", "Tiny marks", "Custom"]
    page.appendStr("Radii", label="Custom radii")
    page.appendFloat("Brushsize", label="Brush size")
    page.appendFloat("Fps", label="Painted FPS")
    page.appendMenu("Resolution", label="Resolution")
    comp.par.Resolution.menuNames = ["25", "50", "100"]
    comp.par.Resolution.menuLabels = ["25%", "50%", "100%"]
    page.appendToggle("Temporal", label="Temporal painting")
    page.appendToggle("Material", label="Brushkit material")
    page.appendPulse("Restart", label="Restart renderer")
    page.appendStr("Status", label="Renderer status")
    comp.par.Active, comp.par.Brush, comp.par.Radiusset = True, "tempera", "balanced"
    comp.par.Radii, comp.par.Brushsize, comp.par.Fps = "8, 4, 2", 1, 12
    # Interactive default: a quarter of the native pixel count. Full resolution
    # remains available for final output without changing the brush controls.
    comp.par.Resolution, comp.par.Temporal, comp.par.Material = "50", True, True
    comp.par.Radii.enableExpr = "me.par.Radiusset == 'custom'"
    for name, low, high in (("Brushsize", .1, 4), ("Fps", 1, 60)):
        par = getattr(comp.par, name)
        par.min, par.max, par.clampMin, par.clampMax = low, high, True, True
        par.normMin, par.normMax = low, high

    stroke = comp.appendCustomPage("Strokes")
    core_pages = {"Paintify": page, "Strokes": stroke,
                  "Motion": comp.appendCustomPage("Motion"),
                  "Classic Brush": comp.appendCustomPage("Classic Brush")}
    stroke.appendMenu("Underpaint", label="Underpaint")
    comp.par.Underpaint.menuNames = ["blur", "average", "none"]
    comp.par.Underpaint.menuLabels = ["Blur", "Average", "None"]
    comp.par.Underpaint = "blur"
    stroke.appendPulse("Globallayers", label="Use global layer controls")
    for key, label, value, low, high, integer in mac_controls.CORE:
        name = mac_controls.parname(key)
        target = core_pages[mac_controls.core_page(key)]
        (target.appendToggle if key == "jitter-per-frame" else
         target.appendInt if integer else target.appendFloat)(name, label=label)
        par = getattr(comp.par, name)
        par.val, par.normMin, par.normMax = value, low, high
        par.min, par.max = low, high
        if key.startswith("flow-") or key in {"temporal-diff", "fresh-paint", "jitter-per-frame"}:
            par.enableExpr = "me.par.Temporal"
        if mac_controls.core_page(key) == "Classic Brush":
            par.enableExpr = "me.par.Brush == 'none'"

    pages = {name: comp.appendCustomPage(name) for name in controls.GROUPS}
    for group, fields in controls.GROUPS.items():
        for field, label, low, high in fields:
            name = mac_controls.parname(field, "Bk")
            target = page if field in mac_controls.QUICK_FIELDS else pages[group]
            target.appendFloat(name, label=label)
            par = getattr(comp.par, name)
            low, high = mac_controls.SLIDER_RANGES.get(field, (low, high))
            par.normMin, par.normMax = low, high
            par.min, par.max = low, high
    enum_pages = {"tip": "Brush", "fieldMode": "Direction", "fallbackMode": "Direction",
                  "support": "Ground", "groundMode": "Ground"}
    for field, (label, choices) in controls.ENUMS.items():
        name = mac_controls.parname(field, "Bk")
        pages[enum_pages[field]].appendMenu(name, label=label)
        par = getattr(comp.par, name)
        par.menuNames, par.menuLabels = [str(i) for i in range(len(choices))], list(choices)
    pages["Finish"].appendMenu("Bkfinish", label="Finish")
    comp.par.Bkfinish.menuNames = ["0", "0.5", "1"]
    comp.par.Bkfinish.menuLabels = ["Off", "Brush relief only", "Full finish"]
    for group, field, label in (("Ground", "strokesOff", "Flat ground only"),
                                ("Direction", "autoVortex", "Track vortex")):
        pages[group].appendToggle(mac_controls.parname(field, "Bk"), label=label)
    pages["Effects"].appendFloat("Bktemporalrefresh", label="Paint renewal")
    comp.par.Bktemporalrefresh.normMin, comp.par.Bktemporalrefresh.normMax = 0, 1
    pages["Colour"].appendInt("Bkpalettecount", label="Palette colours")
    count = comp.par.Bkpalettecount
    count.min, count.max, count.normMin, count.normMax = 0, 32, 0, 32
    count.clampMin, count.clampMax = True, True
    for group, name in (("Ground", "Groundcolour"), ("Finish", "Varnishcolour"),
                        ("Effects", "Contourcolour")):
        pages[group].appendRGB(name, label=name.replace("colour", " colour"))
    palette = comp.appendCustomPage("Palette")
    for i in range(32):
        pars = palette.appendRGB("Pigment" + str(i + 1), label="Pigment " + str(i + 1))
        for par in pars:
            par.enableExpr = "me.par.Bkpalettecount > " + str(i)
    files = comp.appendCustomPage("Look Files")
    files.appendFile("Lookfile", label="Brushkit look (.sbr)")
    files.appendPulse("Loadlook", label="Load look")
    files.appendPulse("Savelook", label="Save look")
    files.appendFile("Logfile", label="Renderer log")
    comp.par.Status = "Ready"
    for name in ("Status", "Logfile"):
        getattr(comp.par, name).readOnly = True

    for name, filename in (("mac_runtime", "mac_runtime.py"), ("mac_controls", "mac_controls.py"),
                           ("brushkit_data", "brushkit_data.json")):
        dat = comp.create("textDAT", name)
        dat.text = (ROOT / "touchdesigner" / filename).read_text()
    source = comp.create("inTOP", "source")
    resolution = comp.create("resolutionTOP", "working_resolution")
    resolution.par.outputresolution.expr = (
        "{'25':'quarter','50':'half','100':'useinput'}[parent().par.Resolution.eval()]")
    painted = comp.create("scriptTOP", "metal_paint")
    callbacks = comp.create("textDAT", "paint_callbacks")
    callbacks.text = "def onCook(scriptOp):\n    scriptOp.parent().op('mac_runtime').module.cook(scriptOp)\n"
    painted.par.callbacks = "paint_callbacks"
    source.outputConnectors[0].connect(resolution.inputConnectors[0])
    resolution.outputConnectors[0].connect(painted.inputConnectors[0])
    result = comp.create("outTOP", "painted")
    painted.outputConnectors[0].connect(result.inputConnectors[0])
    for i, node in enumerate((source, resolution, painted, result)):
        node.nodeX, node.nodeY = i * 220, 0
    result.display, result.viewer = True, True
    comp.par.opviewer.expr = "me.path + '/painted'"
    comp.viewer = True
    par_callbacks = comp.create("parameterexecuteDAT", "controls_changed")
    par_callbacks.par.op, par_callbacks.par.pars = "..", "*"
    par_callbacks.par.valuechange, par_callbacks.par.onpulse = True, True
    par_callbacks.text = ("def onValueChange(par, prev):\n"
                          "    parent().op('mac_runtime').module.change(parent(), par)\n"
                          "def onPulse(par):\n"
                          "    parent().op('mac_runtime').module.pulse(parent(), par.name)\n")
    lifecycle = comp.create("executeDAT", "lifecycle")
    lifecycle.par.exit = True
    lifecycle.text = "def onExit():\n    parent().op('mac_runtime').module.stop(parent())\n"
    bundle = ROOT / "build-package-mac/PaintifyMetal.app"
    for path in sorted(bundle.rglob("*")):
        if path.is_file():
            comp.vfs.addFile(str(path), overrideName="runtime/PaintifyMetal.app/" + path.relative_to(bundle).as_posix())
    comp.op("mac_runtime").module.apply_block(comp, mac_controls.select_brush(data, "tempera", data["defaults"]))
    for par in comp.customPars:
        if par.style != "Pulse" and not par.readOnly:
            par.default = par.eval()
    comp.store("updating", False)
    comp.save(str(ROOT / "Paintify-Mac.tox"))
    print("PAINTIFY_MAC_TOX_SAVED", flush=True)
    return comp


if __name__ == "__main__":
    CREATED = install()
