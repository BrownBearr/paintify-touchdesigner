"""Exercise the packaged Metal renderer, all materials, resize, and live settings."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "touchdesigner"))
import mac_controls
import mac_runtime

data = json.loads((ROOT / "touchdesigner/brushkit_data.json").read_text())
exe = ROOT / "build-package-mac/PaintifyMetal.app/Contents/MacOS/paintify-metal"
with tempfile.TemporaryDirectory() as folder:
    settings = Path(folder) / "live.sbr"
    log = (Path(folder) / "metal.log").open("wb")
    proc = subprocess.Popen([str(exe), str(settings)], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=log, bufsize=0, cwd="/private/tmp")
    try:
        assert struct.unpack("<4I", mac_runtime.read_exact(proc, 16)) == (mac_runtime.MAGIC, 0, 0, 0)
        outputs = set()
        for index, brush in enumerate(data["brushes"]):
            w, h = (96, 64) if index % 2 else (80, 48)
            pixels = bytes(v for y in range(h) for x in range(w)
                           for v in (x * 255 // w, y * 255 // h, 100, 255))
            block = mac_controls.select_brush(data, brush["key"], data["defaults"])
            core = {key: value for key, _, value, *_ in mac_controls.CORE}
            settings.write_text(mac_controls.serialize(core, [8, 4, 2], "blur", block, brush["key"]))
            payload = struct.pack("<4I", mac_runtime.MAGIC, w, h, 0) + pixels
            view = memoryview(payload)
            while view:
                view = view[proc.stdin.write(view):]
            assert struct.unpack("<4I", mac_runtime.read_exact(proc, 16)) == (mac_runtime.MAGIC, w, h, len(pixels))
            result = mac_runtime.read_exact(proc, len(pixels))
            assert result != pixels and max(result) > 0
            outputs.add(result)
            print("METAL_BRUSH_OK", brush["key"], flush=True)
        assert len(outputs) > 12
        proc.stdin.close()
        assert proc.wait(timeout=5) == 0
    except Exception:
        log.flush()
        print((Path(folder) / "metal.log").read_text(), file=sys.stderr)
        raise
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        log.close()
print("PAINTIFY_MAC_WORKER_OK")
