"""Embedded native-Metal frame worker and TouchDesigner control bridge."""
import builtins
import hashlib
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import tempfile
import time

MAGIC = 0x31544450


def registry():
    if not hasattr(builtins, "_paintify_mac_workers"):
        builtins._paintify_mac_workers = {}
    return builtins._paintify_mac_workers


def metadata(comp):
    return json.loads(comp.op("brushkit_data").text), comp.op("mac_controls").module


def prepare(comp):
    files = sorted((f for f in comp.vfs.find() if f.name.startswith("runtime/")), key=lambda f: f.name)
    digest = hashlib.sha256()
    for item in files:
        digest.update(item.name.encode())
        digest.update(item.byteArray)
    root = Path.home() / "Library/Caches/Paintify/TouchDesigner" / digest.hexdigest()[:20]
    for item in files:
        relative = Path(item.name).relative_to("runtime")
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid bundled runtime path")
        path = root / relative
        contents = bytes(item.byteArray)
        if not path.is_file() or hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(contents).digest():
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_bytes(contents)
            os.replace(temporary, path)
    exe = root / "PaintifyMetal.app/Contents/MacOS/paintify-metal"
    compiler = root / "PaintifyMetal.app/Contents/Resources/tools/glslang"
    if not exe.is_file() or not compiler.is_file():
        raise FileNotFoundError("Paintify.tox is missing its Mac renderer")
    exe.chmod(0o755)
    compiler.chmod(0o755)
    return exe


def block(comp):
    data, controls = metadata(comp)
    values = list(comp.fetch("style_block", data["defaults"]))
    for i, field in enumerate(data["fields"]):
        par = getattr(comp.par, controls.parname(field, "Bk"), None)
        if par is not None:
            values[i] = float(par.eval())
    for prefix, names in (("Groundcolour", ("groundR", "groundG", "groundB")),
                          ("Varnishcolour", ("varnishR", "varnishG", "varnishB")),
                          ("Contourcolour", ("contourR", "contourG", "contourB"))):
        for suffix, field in zip("rgb", names):
            values[data["fields"].index(field)] = float(getattr(comp.par, prefix + suffix).eval())
    for i in range(32):
        for j, suffix in enumerate("rgb"):
            values[132 + 4 * i + j] = float(getattr(comp.par, "Pigment" + str(i + 1) + suffix).eval())
    values[0] = float(comp.par.Material.eval())
    return values


def settings_text(comp):
    data, controls = metadata(comp)
    radii = controls.RADIUS_SETS.get(comp.par.Radiusset.eval())
    if radii is None:
        radii = [float(v) for v in comp.par.Radii.eval().replace(",", " ").split()]
    if not radii or any(v < 1 or v > 128 for v in radii) or len(radii) > 8:
        raise ValueError("Brush radii must contain 1-8 values between 1 and 128")
    scale = float(comp.par.Brushsize.eval())
    core = {key: float(getattr(comp.par, controls.parname(key)).eval()) for key, *_ in controls.CORE}
    return controls.serialize(core, [max(1, v * scale) for v in radii], comp.par.Underpaint.eval(),
                              block(comp), comp.par.Brush.eval(), comp.fetch("layers", ()))


def apply_block(comp, values):
    data, controls = metadata(comp)
    comp.store("style_block", list(values))
    for field, value in zip(data["fields"], values):
        par = getattr(comp.par, controls.parname(field, "Bk"), None)
        if par is not None:
            par.val = value
    comp.par.Material = values[0] > .5
    for prefix, names in (("Groundcolour", ("groundR", "groundG", "groundB")),
                          ("Varnishcolour", ("varnishR", "varnishG", "varnishB")),
                          ("Contourcolour", ("contourR", "contourG", "contourB"))):
        for suffix, field in zip("rgb", names):
            getattr(comp.par, prefix + suffix).val = values[data["fields"].index(field)]
    for i in range(32):
        for j, suffix in enumerate("rgb"):
            getattr(comp.par, "Pigment" + str(i + 1) + suffix).val = values[132 + 4 * i + j]


def change(comp, par):
    if comp.fetch("updating", False):
        return
    if par.name == "Brush":
        comp.store("updating", True)
        try:
            data, controls = metadata(comp)
            apply_block(comp, controls.select_brush(data, comp.par.Brush.eval(), block(comp)))
        finally:
            comp.store("updating", False)
    if par.name == "Active" and not par.eval():
        stop(comp)


def pulse(comp, name):
    data, controls = metadata(comp)
    if name == "Restart":
        stop(comp)
        return
    path = Path(comp.par.Lookfile.eval()).expanduser()
    if name == "Savelook":
        path.write_text(settings_text(comp), encoding="utf-8")
    elif name == "Loadlook":
        look = controls.parse(path.read_text(encoding="utf-8"), data)
        comp.store("updating", True)
        try:
            comp.par.Brush = look["brush"]
            apply_block(comp, look["block"])
            for key, value in look["core"].items():
                getattr(comp.par, controls.parname(key)).val = value
            if "radii" in look:
                comp.par.Radiusset = "custom"
                comp.par.Radii = ", ".join(str(v) for v in look["radii"])
                comp.par.Brushsize = 1
            if "underpaint" in look:
                comp.par.Underpaint = look["underpaint"]
            comp.store("layers", look["layers"])
        finally:
            comp.store("updating", False)
    elif name == "Globallayers":
        comp.store("layers", [])


def read_exact(proc, count, timeout=90):
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([proc.stdout], [], [], max(0, remaining))[0]:
            raise TimeoutError("Metal renderer timed out")
        part = os.read(proc.stdout.fileno(), count - len(result))
        if not part:
            raise RuntimeError("Metal renderer stopped; see the component's Logfile")
        result.extend(part)
    return bytes(result)


def start(comp):
    exe = prepare(comp)
    temp = Path(tempfile.mkdtemp(prefix="paintify-td-"))
    settings = temp / "live.sbr"
    settings.write_text(settings_text(comp))
    logpath = temp / "metal.log"
    log = logpath.open("wb")
    proc = subprocess.Popen([str(exe), str(settings)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=log, bufsize=0)
    entry = {"proc": proc, "temp": temp, "settings": settings, "log": log,
             "key": None, "pixels": None, "last": 0.0, "path": comp.path}
    registry()[comp.id] = entry
    comp.par.Logfile = str(logpath)
    ready = struct.unpack("<4I", read_exact(proc, 16))
    if ready != (MAGIC, 0, 0, 0):
        raise RuntimeError("Invalid Metal renderer startup response")
    from td import op, run
    run(watch, comp.id, delayMilliSeconds=1000, delayRef=op.TDResources)
    return entry


def stop_id(identifier):
    entry = registry().pop(identifier, None)
    if not entry:
        return
    proc = entry["proc"]
    proc.stdin.close()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    proc.stdout.close()
    entry["log"].close()
    # Keep the log for troubleshooting, but not frame settings.
    entry["settings"].unlink(missing_ok=True)


def stop(comp):
    stop_id(comp.id)


def watch(identifier):
    from td import op, run
    entry = registry().get(identifier)
    if not entry:
        return
    comp = op(entry["path"])
    if comp is None or comp.id != identifier:
        stop_id(identifier)
        return
    run(watch, identifier, delayMilliSeconds=1000, delayRef=op.TDResources)


def cook(script):
    import numpy as np
    comp = script.parent()
    source = script.inputs[0] if script.inputs else None
    if source is None:
        script.copyNumpyArray(np.zeros((1, 1, 4), dtype=np.float32))
        return
    if not comp.par.Active.eval():
        stop(comp)
        pixels = source.numpyArray(delayed=False)
        if pixels is not None:
            script.copyNumpyArray(pixels)
        return
    try:
        pixels = source.numpyArray(delayed=False)
        if pixels is None:
            return
        step = max(1, round(100 / int(comp.par.Resolution.eval())))
        pixels = np.ascontiguousarray(np.clip(pixels[::-step, ::step, :4], 0, 1) * 255, dtype=np.uint8)
        h, w = pixels.shape[:2]
        text = settings_text(comp)
        key = hashlib.sha256(pixels.tobytes() + text.encode()).digest()
        entry = registry().get(comp.id) or start(comp)
        now = time.monotonic()
        animated = comp.par.Jitterperframe.eval() or float(comp.par.Bkrain.eval()) > 0
        if entry["pixels"] is not None and (now - entry["last"] < 1 / comp.par.Fps.eval()
                                            or (entry["key"] == key and not animated)):
            script.copyNumpyArray(entry["pixels"])
            return
        entry["settings"].write_text(text)
        payload = struct.pack("<4I", MAGIC, w, h, int(comp.par.Temporal.eval())) + pixels.tobytes()
        view = memoryview(payload)
        while view:
            written = entry["proc"].stdin.write(view)
            if not written:
                raise RuntimeError("Metal input pipe closed")
            view = view[written:]
        header = struct.unpack("<4I", read_exact(entry["proc"], 16))
        if header != (MAGIC, w, h, w * h * 4):
            raise RuntimeError("Invalid Metal frame response")
        painted = np.frombuffer(read_exact(entry["proc"], w * h * 4), dtype=np.uint8).reshape(h, w, 4)
        entry["pixels"] = np.ascontiguousarray(painted[::-1].astype(np.float32) / 255)
        entry["key"], entry["last"] = key, now
        script.copyNumpyArray(entry["pixels"])
        comp.par.Status = "Painting"
    except Exception as error:
        comp.par.Status = str(error)
        stop(comp)
        pixels = source.numpyArray(delayed=False)
        if pixels is not None:
            script.copyNumpyArray(pixels)
