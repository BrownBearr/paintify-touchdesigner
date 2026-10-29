"""Bootstrap TouchDesigner's native builder without editing a user project."""
from pathlib import Path
import shutil
import subprocess
import time
import plistlib
import struct
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--test", action="store_true")
args = parser.parse_args()
(ROOT / "build-bootstrap/result.txt").unlink(missing_ok=True)
TD = Path("/Applications/TouchDesigner.app/Contents/MacOS")
folder = ROOT / "build-bootstrap" / str(time.time_ns())
folder.mkdir(parents=True, exist_ok=True)
bootstrap = folder / "bootstrap.tox"
shutil.copy2(ROOT / "Paintify.tox", bootstrap)
result = subprocess.run([str(TD / "toeexpand"), str(bootstrap)])
if not Path(str(bootstrap) + ".dir").is_dir():
    raise RuntimeError("toeexpand failed")
expanded = Path(str(bootstrap) + ".dir")
# The bootstrap uses only old, compatible DAT/TOP operators. toecollapse is
# from the installed build; mark its temporary output with that build too.
version = plistlib.loads(Path("/Applications/TouchDesigner.app/Contents/Info.plist").read_bytes())["CFBundleShortVersionString"]
build_info = expanded / ".build"
text = build_info.read_text()
text = "\n".join("build " + version if line.startswith("build ") else line for line in text.splitlines()) + "\n"
build_info.write_text(text)
dat = expanded / "paintify/paintify_lifecycle.text"
contents = dat.read_bytes()
source_start = contents.index(b"\ndef onCreate")
header = contents[:source_start - 4]
assert source_start == 27, "Unexpected TouchDesigner DAT encoding"
script = """
_scheduled = False
def onCreate():
    global _scheduled
    if _scheduled:
        return
    _scheduled = True
    def build():
        import runpy
        import traceback
        from pathlib import Path
        try:
            Path(REPORT).write_text('BUILDING')
            result = runpy.run_path(SCRIPT, run_name='__main__')
            TEST_CALL
            Path(REPORT).write_text('RESULT_PLACEHOLDER')
        except Exception:
            Path(REPORT).write_text(traceback.format_exc())
        project.quit(force=True)
    run(build, delayMilliSeconds=1000, delayRef=op.TDResources)

def onStart():
    onCreate()
""".replace("SCRIPT", repr(str(ROOT / "touchdesigner/install_mac.py"))).replace(
    "REPORT", repr(str(ROOT / "build-bootstrap/result.txt")))
script = script.replace("TEST_CALL", "runpy.run_path(" + repr(str(ROOT / "tests/touchdesigner_smoke.py")) +
                        ", init_globals={'component': result['CREATED']})" if args.test else "pass")
script = script.replace("RESULT_PLACEHOLDER", "TESTED" if args.test else "BUILT")
compile(script, "bootstrap", "exec")
encoded = script.encode()
dat.write_bytes(header + struct.pack(">I", len(encoded)) + encoded)
parms = expanded / "paintify/paintify_lifecycle.parm"
parms.write_text("?\ncreate 0 on\nstart 0 on\nlanguage 0 python\n?\n")
# Keep only a blank Base COMP and its builder DAT. In particular, do not
# import the Windows Spout operator parameters or bundled Windows files.
(expanded / "paintify.parm").write_text("?\nenableexternaltox 0 off\n?\n")
(expanded / "paintify.network").write_text("1\nend\n")
toc = Path(str(bootstrap) + ".toc")
lines = toc.read_text().splitlines()
keep = {".build", "paintify.n", "paintify.parm", "paintify.network",
        "paintify/paintify_lifecycle.n", "paintify/paintify_lifecycle.parm",
        "paintify/paintify_lifecycle.text"}
toc.write_text("\n".join(line for line in lines if line.startswith("#") or line in keep) + "\n")
subprocess.run([str(TD / "toecollapse"), str(bootstrap)], check=True)
subprocess.run(["open", "-n", "-a", "/Applications/TouchDesigner.app", str(bootstrap)], check=True)
print("Build result:", ROOT / "build-bootstrap/result.txt")
