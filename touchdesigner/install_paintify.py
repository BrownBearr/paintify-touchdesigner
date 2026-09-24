"""Run inside TouchDesigner to create a reusable Paintify.tox component.

Textport: import runpy; runpy.run_path(r'PATH_TO_THIS_FILE')
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / 'build-live' / 'gpu-sbr.exe'
TOX = ROOT / 'touchdesigner' / 'Paintify.tox'

if not EXE.is_file():
    raise FileNotFoundError(f'Build the Paintify GPU renderer first: {EXE}')


RUNTIME = r'''
import builtins
import os
import subprocess

def _registry():
    if not hasattr(builtins, '_paintify_processes'):
        builtins._paintify_processes = {}
    return builtins._paintify_processes

def names(comp):
    # A path-based name lets multiple Paintify components coexist.
    base = comp.path.replace('/', '_').strip('_')
    return ('Paintify_' + base + '_input', 'Paintify_' + base + '_output')

def stop(comp):
    entry = _registry().pop(comp.id, None)
    if entry is None:
        return
    proc, log, stopfile = entry
    if proc is not None and proc.poll() is None:
        with open(stopfile, 'w', encoding='utf-8') as signal:
            signal.write('stop')
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
    log.close()
    if os.path.exists(stopfile):
        os.remove(stopfile)

def start(comp):
    stop(comp)
    if not comp.par.Active.eval():
        return
    exe = comp.par.Executable.eval()
    if not os.path.isfile(exe):
        print('Paintify: missing renderer: ' + exe)
        return
    incoming, outgoing = names(comp)
    stopfile = os.path.join(os.path.dirname(exe),
                            'paintify-stop-' + str(comp.id) + '.flag')
    if os.path.exists(stopfile):
        os.remove(stopfile)
    args = [exe, '--live-spout', '--spout-in', incoming,
            '--spout-out', outgoing,
            '--live-stop-file', stopfile,
            '--live-parent-pid', str(os.getpid()),
            '--target-fps', str(comp.par.Fps.eval()),
            '--preset', str(comp.par.Preset.eval()),
            '--relax', str(comp.par.Relax.eval()),
            '--brush-texture', str(comp.par.Brushtexture.eval()),
            '--impasto', str(comp.par.Impasto.eval()),
            '--impasto-light', str(comp.par.Impastolight.eval()),
            '--temporal-diff', str(comp.par.Temporal.eval()),
            '--flow', str(comp.par.Flow.eval())]
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    log = open(os.path.join(os.path.dirname(exe), 'paintify-live.log'), 'a',
               encoding='utf-8')
    try:
        proc = subprocess.Popen(args, cwd=os.path.dirname(os.path.dirname(exe)),
                                creationflags=flags, stdout=log,
                                stderr=subprocess.STDOUT)
    except Exception:
        log.close()
        raise
    _registry()[comp.id] = (proc, log, stopfile)
    print('Paintify started: ' + incoming + ' -> ' + outgoing)
'''

EXECUTE = r'''
def create():
    comp = parent()
    run(lambda: comp.op('paintify_runtime').module.start(comp), delayFrames=2)
    return

def onExit():
    comp = parent()
    comp.op('paintify_runtime').module.stop(comp)
    return
'''

PAR_EXECUTE = r'''
def onValueChange(par, prev):
    comp = parent()
    comp.op('paintify_runtime').module.start(comp)
    return
'''

OP_EXECUTE = r'''
def onDestroy(*args):
    comp = parent()
    comp.op('paintify_runtime').module.stop(comp)
    return
'''


def install():
    # TD injects op() in the Textport; import it explicitly for runpy.run_path.
    from td import op

    root = op('/project1')
    if root is None:
        raise RuntimeError('TouchDesigner project /project1 is unavailable')
    previous = root.op('paintify')
    if previous is not None:
        previous.destroy()

    comp = root.create('baseCOMP', 'paintify')
    comp.nodeX = 0
    comp.nodeY = 0
    page = comp.appendCustomPage('Paintify')
    page.appendToggle('Active', label='Paintify active')
    page.appendStr('Executable', label='Renderer executable')
    page.appendStr('Preset', label='Look')
    page.appendFloat('Fps', label='Painted frames / sec')
    page.appendInt('Relax', label='Relaxation iterations')
    page.appendFloat('Brushtexture', label='Brush texture')
    page.appendFloat('Impasto', label='Impasto height')
    page.appendFloat('Impastolight', label='Impasto lighting')
    page.appendFloat('Temporal', label='Temporal repaint threshold')
    page.appendInt('Flow', label='Optical flow levels')
    comp.par.Active = True
    comp.par.Executable = str(EXE)
    comp.par.Preset = 'impressionist'
    comp.par.Fps = 12
    comp.par.Relax = 4
    comp.par.Brushtexture = 0.45
    comp.par.Impasto = 0.35
    comp.par.Impastolight = 0.5
    comp.par.Temporal = 0
    comp.par.Flow = 0

    source = comp.create('inTOP', 'source')
    send = comp.create('syphonspoutoutTOP', 'send_to_paintify')
    receive = comp.create('syphonspoutinTOP', 'painted_from_paintify')
    result = comp.create('outTOP', 'painted')
    source.outputConnectors[0].connect(send.inputConnectors[0])
    receive.outputConnectors[0].connect(result.inputConnectors[0])
    send.par.active = True
    send.par.sendername.expr = "parent().op('paintify_runtime').module.names(parent())[0]"
    receive.par.sendername.expr = "parent().op('paintify_runtime').module.names(parent())[1]"
    source.nodeX, source.nodeY = 0, 0
    send.nodeX, send.nodeY = 200, 0
    receive.nodeX, receive.nodeY = 400, 0
    result.nodeX, result.nodeY = 600, 0

    runtime = comp.create('textDAT', 'paintify_runtime')
    runtime.text = RUNTIME
    lifecycle = comp.create('executeDAT', 'paintify_lifecycle')
    lifecycle.text = EXECUTE
    lifecycle.par.create = True
    lifecycle.par.exit = True
    controls = comp.create('parameterexecuteDAT', 'paintify_controls')
    controls.text = PAR_EXECUTE
    controls.par.op = '..'
    controls.par.pars = 'Active Executable Preset Fps Relax Brushtexture Impasto Impastolight Temporal Flow'
    controls.par.valuechange = True
    controls.par.custom = True
    cleanup = comp.create('opexecuteDAT', 'paintify_cleanup')
    cleanup.text = OP_EXECUTE
    cleanup.par.op = '..'
    cleanup.par.destroy = True

    comp.save(str(TOX), createFolders=True)
    print('Paintify component saved to ' + str(TOX))
    return comp


install()
