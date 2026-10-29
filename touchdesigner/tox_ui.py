"""Native TouchDesigner parameter UX; embedded so the TOX stays standalone."""

WINDOWS_MODES = {
    'preview': {'Maxdimension': 640, 'Passes': 4, 'Relax': 0},
    'balanced': {'Maxdimension': 1280, 'Passes': 4, 'Relax': 0},
    'quality': {'Maxdimension': 0, 'Passes': 8, 'Relax': 4},
}
MAC_MODES = {
    'preview': {'Resolution': '25'},
    'balanced': {'Resolution': '50'},
    'quality': {'Resolution': '100'},
}

HELP = '''PAINTIFY — QUICK START

1. Connect any image or video TOP to Paintify's input.
2. View Paintify's output, or connect it to your next TOP.
3. Choose a Look (Windows) or Brush family (Mac).
4. Choose a Performance preset. Balanced is a good starting point.

PERFORMANCE WITHOUT LOSING YOUR CONTROLS
Preview reduces working resolution. Balanced uses the interactive defaults.
Full quality uses native resolution; Windows also enables extra painting and
refinement passes. These choices change image detail and can cost more GPU time.
Painted FPS controls how often a new painting is made, not the project FPS.
Use Performance / Refinement for individual settings. Editing a preset-owned
setting changes the label to Custom; it does not reset your look or controls.

FIND YOUR CONTROLS
Paintify: everyday look, brush and performance controls.
Strokes: size, direction and placement. Surface / Paint: material appearance.
Motion: frame-to-frame behavior. Refinement: expensive stroke improvement.
Setup: renderer restart and diagnostic paths. All original controls remain.
Mac adds Brush, Direction, Colour, Ground, Finish, Effects and Palette pages.

ADVANCED VALUES
Windows numeric -1 and blank brush radii mean automatic / use the look.
Slider ranges are suggestions; extended numeric entry remains available.
Alt-hover a parameter for help. Right-click a parameter to reset that control.
Changing performance presets does not change Painted FPS or the chosen look.

IF THE OUTPUT IS BLANK
Check the input TOP first. Enable Live paint and read Renderer status.
Use Restart renderer in Setup. Windows needs a Spout-capable GPU and renderer;
the Mac component needs Apple Silicon and the bundled Metal worker.
Windows Live paint off holds the last painting. Mac Live paint off bypasses
the effect at the original input resolution.
'''


def _par(comp, name):
    return getattr(comp.par, name, None)


def _page(comp, name):
    return next((p for p in comp.customPages if p.name == name), None) or comp.appendCustomPage(name)


def _move(comp, page_name, names):
    page = _page(comp, page_name)
    for name in names:
        par = _par(comp, name)
        if par is not None:
            par.page = page_name
    page.sort(*[name for name in names if _par(comp, name) is not None])


def _hint(comp, name, text, label=None, section=False):
    par = _par(comp, name)
    if par is not None:
        par.help = text
        par.startSection = section
        if label:
            par.label = label


def _range(comp, name, low, high, minimum=None):
    par = _par(comp, name)
    if par is not None:
        par.normMin, par.normMax = low, high
        if minimum is not None:
            par.min, par.clampMin = minimum, True
        # Suggested slider maxima must never clip imported looks or expressions.
        par.clampMax = False


def modes(comp):
    return MAC_MODES if _par(comp, 'Resolution') is not None else WINDOWS_MODES


def apply_preset(identifier, mode):
    from td import op
    comp = op(identifier)
    if comp is not None and comp.par.Performance.eval() == mode:
        change(comp, comp.par.Performance)


def change(comp, par, defer=False):
    """Return True when a UI-only change needs no renderer update."""
    presets = modes(comp)
    if par.name == 'Performance':
        mode = par.eval()
        values = presets.get(mode)
        if values is None:
            comp.store('ui_preset_applied', 'custom')
            return True
        if defer:
            # Changes inside Parameter Execute can be excluded from its next
            # baseline. Apply the group outside that callback so subsequent
            # manual edits back to their original values are still observed.
            from td import op, run
            run(apply_preset, comp.id, mode, delayFrames=1, delayRef=op.TDResources)
            return True
        for name, value in values.items():
            getattr(comp.par, name).val = value
        comp.store('ui_preset_applied', mode)
        return False
    performance = _par(comp, 'Performance')
    if performance is not None:
        values = presets.get(performance.eval())
        # Parameter Execute can deliver other changed fields before the menu
        # callback on load or during a multi-parameter edit. Let a newly chosen
        # preset apply before interpreting those callbacks as manual edits.
        if values and par.name in values and comp.fetch('ui_preset_applied', None) == performance.eval():
            if any(getattr(comp.par, name).eval() != value for name, value in values.items()):
                performance.val = 'custom'
                comp.store('ui_preset_applied', 'custom')
    return par.name in {'Guide', 'Status', 'Logfile'}


def pulse(comp, name):
    if name == 'Help':
        comp.op('paintify_help').openViewer()
        return True
    return False


def configure(comp):
    """Upgrade an existing component without removing or renaming parameters."""
    mac = _par(comp, 'Resolution') is not None
    main = _page(comp, 'Paintify')
    setup = _page(comp, 'Setup')
    if not mac and _par(comp, 'Maxdimension') is None:
        _page(comp, 'Performance').appendInt('Maxdimension', label='Working size cap (0 = native)')
        comp.par.Maxdimension = 1280
        comp.par.Maxdimension.default = 1280
    if _par(comp, 'Guide') is None:
        main.appendStr('Guide', label='Start here')
        comp.par.Guide = 'Connect a TOP'
        comp.par.Guide.readOnly = True
    if _par(comp, 'Status') is None:
        main.appendStr('Status', label='Renderer status')
        comp.par.Status = 'Ready'
        comp.par.Status.readOnly = True
    if _par(comp, 'Performance') is None:
        main.appendMenu('Performance', label='Performance preset')
        comp.par.Performance.menuNames = ['preview', 'balanced', 'quality', 'custom']
        comp.par.Performance.menuLabels = ['Fast preview', 'Balanced', 'Full quality', 'Custom']
        selected = next((key for key, values in modes(comp).items()
                         if all(getattr(comp.par, name).eval() == value for name, value in values.items())), 'custom')
        comp.par.Performance = selected
        comp.par.Performance.default = selected
    comp.store('ui_preset_applied', comp.par.Performance.eval())
    if _par(comp, 'Restart') is None:
        setup.appendPulse('Restart', label='Restart renderer')
    if not mac and _par(comp, 'Logfile') is None:
        setup.appendFile('Logfile', label='Renderer log')
        comp.par.Logfile.readOnly = True
    if _par(comp, 'Help') is None:
        main.appendPulse('Help', label='Quick start / Help')
    help_dat = comp.op('paintify_help') or comp.create('textDAT', 'paintify_help')
    help_dat.text = HELP
    help_dat.par.wordwrap = 'on'
    if mac:
        _move(comp, 'Paintify', ('Guide', 'Active', 'Status', 'Brush', 'Brushsize',
              'Radiusset', 'Radii', 'Performance', 'Fps', 'Threshold', 'Opacity',
              'Material', 'Temporal', 'Help'))
        _move(comp, 'Performance', ('Resolution', 'Passes', 'Tensorsigma', 'Etfiterations', 'Etfradius'))
        _move(comp, 'Refinement', ('Relaxiterations', 'Relaxarea', 'Relaxmove',
              'Relaxcandidates', 'Relaxremove', 'Relaxsubpasses'))
        _move(comp, 'Strokes', ('Curvature', 'Gridfactor', 'Underpaint', 'Minlength', 'Maxlength', 'Globallayers'))
        _move(comp, 'Brush', ('Bkwidthscale', 'Bkbristles', 'Bkload'))
        _move(comp, 'Colour', ('Bksaturation', 'Bkwarmcool'))
        _move(comp, 'Finish', ('Bkimpasto',))
        _move(comp, 'Setup', ('Restart', 'Logfile'))
        _hint(comp, 'Resolution', 'Working width and height, before GPU readback. 50% uses one quarter of the pixels; 100% keeps native resolution.', 'Working resolution')
        _hint(comp, 'Radiusset', 'Choose a brush-radius recipe. Custom enables the radii field below.')
        _hint(comp, 'Radii', 'Custom comma-separated radii, 1 to 128 pixels. Enable Custom in Radius set to edit.')
        _hint(comp, 'Material', 'Enable the Brushkit material model. Classic Brush controls apply when material is off or the classic family is selected.', 'Brushkit material')
        for classic in _page(comp, 'Classic Brush').pars:
            classic.enableExpr = "not me.par.Material or me.par.Brush == 'none'"
        _hint(comp, 'Jitterperframe', 'Generate fresh stroke variation for every painted frame. Off keeps variation stable.', 'Animate brush variation')
        _hint(comp, 'Relaxiterations', 'Extra stroke refinement passes. Each pass can substantially increase GPU time. Zero disables refinement.', section=True)
        _hint(comp, 'Bkimpasto', 'Paint thickness for the Brushkit material, independent of classic brush impasto.', 'Paint thickness')
    else:
        _move(comp, 'Paintify', ('Guide', 'Active', 'Status', 'Preset', 'Performance',
              'Fps', 'Brushtexture', 'Impasto', 'Impastolight', 'Help'))
        _move(comp, 'Performance', ('Maxdimension', 'Passes', 'Tensorsigma', 'Etf', 'Etfradius'))
        _move(comp, 'Strokes', ('Radii', 'Threshold', 'Curvature', 'Opacity',
              'Gridfactor', 'Minlen', 'Maxlen', 'Underpaint', 'Sizejitter', 'Anglejitter', 'Opacityjitter'))
        _move(comp, 'Surface', ('Bristledensity', 'Texturetaper', 'Drybrush', 'Lightangle'))
        _move(comp, 'Motion', ('Temporal', 'Flow', 'Flowiters', 'Jitterperframe'))
        _move(comp, 'Refinement', ('Relax', 'Relaxarea', 'Relaxmove', 'Relaxcandidates', 'Relaxremove', 'Relaxsubpasses'))
        _move(comp, 'Setup', ('Restart', 'Logfile', 'Executable'))
        for page in list(comp.customPages):
            if page.name == 'Paintify Advanced' and not page.pars:
                page.destroy()
        for name, high in {'Threshold': 150, 'Curvature': 1, 'Opacity': 1, 'Gridfactor': 2.5,
                'Minlen': 32, 'Maxlen': 32, 'Passes': 16, 'Tensorsigma': 8, 'Etf': 8,
                'Etfradius': 12, 'Bristledensity': 30, 'Texturetaper': 1, 'Drybrush': 1,
                'Lightangle': 360, 'Sizejitter': .6, 'Anglejitter': 45, 'Opacityjitter': .6,
                'Relaxarea': .4, 'Relaxmove': 2, 'Relaxcandidates': 16, 'Relaxremove': 2,
                'Relaxsubpasses': 4, 'Flowiters': 12, 'Brushtexture': 1}.items():
            _range(comp, name, -1, high, -1)
            _hint(comp, name, 'Adjust ' + getattr(comp.par, name).label + '. -1 uses the look or renderer default. Changes apply live.')
        _range(comp, 'Maxdimension', 0, 3840, 0)
        _range(comp, 'Relax', 0, 8, 0)
        _range(comp, 'Temporal', 0, 255, 0)
        _range(comp, 'Flow', 0, 5, 0)
        _range(comp, 'Impasto', 0, 1, 0)
        _range(comp, 'Impastolight', 0, 2, 0)
        _hint(comp, 'Maxdimension', 'Longest working image dimension in pixels; aspect ratio is preserved. 0 uses native size. A smaller size reduces detail and GPU work.', 'Working size cap (0 = native)')
        _hint(comp, 'Relax', 'Additional full-canvas stroke refinement. Each pass can be expensive. 0 is fastest; 4 is a quality-oriented setting.', 'Refinement passes', True)
        _hint(comp, 'Executable', 'Advanced troubleshooting only. Leave blank to use the renderer bundled in this TOX.', 'Renderer override (optional)')
        _hint(comp, 'Radii', 'Comma-separated brush radii in pixels. Leave blank to use the chosen look.')
        _hint(comp, 'Temporal', 'Repaint only changed regions when above zero. 0 repaints the full image.', 'Repaint threshold', True)
        _hint(comp, 'Flow', 'Motion estimation pyramid levels. More levels cost GPU time; 0 disables optical flow.', 'Optical flow levels')
    _range(comp, 'Fps', 1, 60, 1)
    _hint(comp, 'Fps', 'Number of new paintings per second. Lowering this saves work while TouchDesigner can still run at its own project frame rate.', 'Painted FPS')
    _hint(comp, 'Active', 'Enable the live renderer. Off bypasses at full input size on Mac, or holds the last painting on Windows.', 'Live paint')
    _hint(comp, 'Status', 'Current renderer state. If painting fails, check the input TOP and use Restart renderer in Setup.')
    comp.par.Status.label = 'Status'
    look = _par(comp, 'Brush' if mac else 'Preset')
    look.startSection = True
    if not mac:
        comp.par.Brushtexture.label = 'Brush texture'
        comp.par.Impasto.label = 'Paint thickness'
        comp.par.Impastolight.label = 'Relief lighting'
    _hint(comp, 'Performance', 'Applies working-resolution settings; Windows also sets painting and refinement passes. Your FPS, look and material controls are preserved. Manual edits switch this label to Custom.', section=True)
    _hint(comp, 'Help', 'Open the built-in quick start and troubleshooting guide. No download or external files needed.', section=True)
    _hint(comp, 'Restart', 'Stop and restart the bundled renderer while keeping your current look and controls.')
    _hint(comp, 'Guide', 'Connect a TOP to this component, then view its output. Choose a look and a performance preset. Use Quick start / Help for details.')
    order = ('Paintify', 'Performance', 'Strokes', 'Surface', 'Brush', 'Direction', 'Colour', 'Ground', 'Finish', 'Motion', 'Refinement', 'Classic Brush', 'Effects', 'Palette', 'Look Files', 'Setup')
    existing_pages = {page.name for page in comp.customPages}
    comp.sortCustomPages(*[name for name in order if name in existing_pages])
    controls = comp.op('controls_changed' if mac else 'paintify_controls')
    if controls is not None:
        runtime = 'mac_runtime' if mac else 'paintify_runtime'
        update = 'change(comp, par)' if mac else 'update(comp)'
        controls.text = f'''def onValueChange(par, prev):
    comp = parent()
    if not comp.op('paintify_ui').module.change(comp, par, defer=True):
        comp.op('{runtime}').module.{update}
def onPulse(par):
    comp = parent()
    if not comp.op('paintify_ui').module.pulse(comp, par.name):
        comp.op('{runtime}').module.pulse(comp, par.name)
'''
        controls.par.custom, controls.par.builtin = True, False
        controls.par.pars = ' '.join(par.name for par in comp.customPars
                                     if par.name not in {'Guide', 'Status', 'Logfile'})
    comp.currentPage = 'Paintify'
    comp.showCustomOnly = True
    comp.par.opviewer.expr = "me.path + '/painted'"
    comp.viewer = True
    return comp
