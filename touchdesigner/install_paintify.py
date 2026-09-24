"""Run inside TouchDesigner to create a reusable Paintify.tox component.

Textport: import runpy; runpy.run_path(r'PATH_TO_THIS_FILE')
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ('build-package' if (ROOT / 'build-package' / 'gpu-sbr.exe').is_file() else 'build-live')
EXE = BUILD / 'gpu-sbr.exe'
TOX = ROOT / 'Paintify.tox'

if not EXE.is_file():
    raise FileNotFoundError(f'Build the Paintify GPU renderer first: {EXE}')


RUNTIME = (Path(__file__).parent / 'paintify_runtime.py').read_text(encoding='utf-8')

EXECUTE = r'''
def onCreate():
    comp = parent()
    run(lambda: comp.op('paintify_runtime').module.start(comp),
        delayFrames=2, delayRef=op.TDResources)
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
        old_runtime = previous.op('paintify_runtime')
        if old_runtime is not None:
            old_runtime.module.stop(previous)
        previous.destroy()

    comp = root.create('baseCOMP', 'paintify')
    comp.nodeX = 0
    comp.nodeY = 0
    page = comp.appendCustomPage('Paintify')
    page.appendToggle('Active', label='Paintify active')
    page.appendStr('Executable', label='Renderer executable override')
    page.appendMenu('Preset', label='Look')
    comp.par.Preset.menuNames = ['impressionist', 'expressionist',
                                 'pointillist', 'wash', 'detail']
    comp.par.Preset.menuLabels = ['Impressionist', 'Expressionist',
                                  'Pointillist', 'Wash', 'Detail']
    page.appendFloat('Fps', label='Painted frames / sec')
    page.appendInt('Relax', label='Relaxation iterations')
    page.appendFloat('Brushtexture', label='Brush texture (-1 = look)')
    page.appendFloat('Impasto', label='Impasto height')
    page.appendFloat('Impastolight', label='Impasto lighting')
    page.appendFloat('Temporal', label='Temporal repaint threshold')
    page.appendInt('Flow', label='Optical flow levels')

    advanced = comp.appendCustomPage('Paintify Advanced')
    advanced.appendStr('Radii', label='Brush radii (blank = look)')
    advanced.appendFloat('Threshold', label='Stroke threshold (-1 = look)')
    advanced.appendFloat('Curvature', label='Curvature (-1 = look)')
    advanced.appendFloat('Opacity', label='Opacity (-1 = look)')
    advanced.appendFloat('Gridfactor', label='Grid factor (-1 = look)')
    advanced.appendFloat('Maxlen', label='Maximum stroke length (-1 = look)')
    advanced.appendFloat('Minlen', label='Minimum stroke length (-1 = look)')
    advanced.appendMenu('Underpaint', label='Underpaint')
    comp.par.Underpaint.menuNames = ['preset', 'blur', 'average', 'none']
    comp.par.Underpaint.menuLabels = ['Use look', 'Blur', 'Average', 'None']
    advanced.appendInt('Passes', label='Painting passes (-1 = default)')
    advanced.appendFloat('Tensorsigma', label='Tensor sigma (-1 = default)')
    advanced.appendInt('Etf', label='Edge tangent iterations (-1 = default)')
    advanced.appendFloat('Etfradius', label='Edge tangent radius (-1 = default)')
    advanced.appendFloat('Bristledensity', label='Bristle density (-1 = default)')
    advanced.appendFloat('Texturetaper', label='Texture taper (-1 = default)')
    advanced.appendFloat('Drybrush', label='Dry brush (-1 = default)')
    advanced.appendFloat('Lightangle', label='Light angle (-1 = default)')
    advanced.appendFloat('Sizejitter', label='Size jitter (-1 = default)')
    advanced.appendFloat('Anglejitter', label='Angle jitter (-1 = default)')
    advanced.appendFloat('Opacityjitter', label='Opacity jitter (-1 = default)')
    advanced.appendFloat('Relaxarea', label='Relax area (-1 = default)')
    advanced.appendFloat('Relaxmove', label='Relax move (-1 = default)')
    advanced.appendInt('Relaxcandidates', label='Relax candidates (-1 = default)')
    advanced.appendFloat('Relaxremove', label='Relax remove (-1 = default)')
    advanced.appendInt('Relaxsubpasses', label='Relax subpasses (-1 = default)')
    advanced.appendInt('Flowiters', label='Optical flow iterations (-1 = default)')
    advanced.appendToggle('Jitterperframe', label='Jitter each frame')

    comp.par.Active = True
    comp.par.Executable = ''
    comp.par.Preset = 'impressionist'
    comp.par.Fps = 12
    comp.par.Relax = 4
    comp.par.Brushtexture = -1
    comp.par.Impasto = 0.35
    comp.par.Impastolight = 0.5
    comp.par.Temporal = 0
    comp.par.Flow = 0
    comp.par.Radii = ''
    comp.par.Underpaint = 'preset'
    for name in ('Threshold', 'Curvature', 'Opacity', 'Gridfactor',
                 'Maxlen', 'Minlen', 'Passes', 'Tensorsigma', 'Etf',
                 'Etfradius', 'Bristledensity', 'Texturetaper', 'Drybrush',
                 'Lightangle', 'Sizejitter', 'Anglejitter', 'Opacityjitter',
                 'Relaxarea', 'Relaxmove', 'Relaxcandidates', 'Relaxremove',
                 'Relaxsubpasses', 'Flowiters'):
        getattr(comp.par, name).val = -1
    comp.par.Jitterperframe = False

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
    controls.par.pars = ('Active Executable Preset Fps Relax Brushtexture Impasto '
                         'Impastolight Temporal Flow Radii Threshold Curvature '
                         'Opacity Gridfactor Maxlen Minlen Underpaint Passes '
                         'Tensorsigma Etf Etfradius Bristledensity Texturetaper '
                         'Drybrush Lightangle Sizejitter Anglejitter Opacityjitter '
                         'Relaxarea Relaxmove Relaxcandidates Relaxremove '
                         'Relaxsubpasses Flowiters Jitterperframe')
    controls.par.valuechange = True
    controls.par.custom = True
    cleanup = comp.create('opexecuteDAT', 'paintify_cleanup')
    cleanup.text = OP_EXECUTE
    cleanup.par.op = '..'
    cleanup.par.destroy = True

    for name in ('gpu-sbr.exe', 'glfw3.dll', 'Spout.dll'):
        path = BUILD / name
        if not path.is_file():
            raise FileNotFoundError('Build the Paintify live renderer first: ' + str(path))
        comp.vfs.addFile(str(path), overrideName='runtime/' + name)
    for path in sorted((ROOT / 'shaders').glob('*')):
        if path.is_file():
            comp.vfs.addFile(str(path), overrideName='runtime/shaders/' + path.name)
    comp.save(str(TOX), createFolders=True)
    runtime.module.start(comp)
    print('Paintify component saved to ' + str(TOX))
    return comp


install()
