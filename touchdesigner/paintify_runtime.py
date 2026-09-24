"""Runtime embedded in Paintify.tox by install_paintify.py."""
import builtins
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


def _registry():
    if not hasattr(builtins, '_paintify_processes'):
        builtins._paintify_processes = {}
    return builtins._paintify_processes


def _watchers():
    if not hasattr(builtins, '_paintify_watchers'):
        builtins._paintify_watchers = set()
    return builtins._paintify_watchers


def names(comp):
    base = comp.path.replace('/', '_').strip('_')
    return ('Paintify_' + base + '_input', 'Paintify_' + base + '_output')


def _runtime_files(comp):
    return sorted(
        (item for item in comp.vfs.find() if item.name.startswith('runtime/')),
        key=lambda item: item.name)


def prepare_runtime(comp):
    override = comp.par.Executable.eval().strip()
    if override:
        exe = Path(override)
        if not exe.is_file():
            raise FileNotFoundError('Paintify renderer not found: ' + override)
        return exe, None

    files = _runtime_files(comp)
    if not files:
        raise RuntimeError('Paintify.tox has no bundled renderer')
    digest = hashlib.sha256()
    for item in files:
        digest.update(item.name.encode('utf-8'))
        digest.update(item.byteArray)
    cache_base = Path(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir())
    runtime_dir = cache_base / 'Paintify' / digest.hexdigest()[:20]
    for item in files:
        relative = Path(item.name).relative_to('runtime')
        destination = runtime_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        contents = bytes(item.byteArray)
        if destination.is_file() and hashlib.sha256(destination.read_bytes()).digest() == hashlib.sha256(contents).digest():
            continue
        temporary = destination.with_name(destination.name + '.tmp')
        temporary.write_bytes(contents)
        os.replace(temporary, destination)
    exe = runtime_dir / 'gpu-sbr.exe'
    if not exe.is_file():
        raise FileNotFoundError('Bundled Paintify renderer is missing')
    return exe, runtime_dir / 'shaders'


def _append_numeric(args, comp, parameter, flag):
    value = getattr(comp.par, parameter).eval()
    if value >= 0:
        args.extend((flag, str(value)))


def renderer_args(comp, exe, stopfile, settingsfile):
    incoming, outgoing = names(comp)
    args = [str(exe), '--live-spout', '--spout-in', incoming,
            '--spout-out', outgoing, '--live-stop-file', str(stopfile),
            '--live-parent-pid', str(os.getpid()),
            '--live-settings-file', str(settingsfile),
            '--target-fps', str(comp.par.Fps.eval()),
            '--preset', str(comp.par.Preset.eval())]
    for parameter, flag in (
            ('Relax', '--relax'),
            ('Brushtexture', '--brush-texture'),
            ('Impasto', '--impasto'),
            ('Impastolight', '--impasto-light'),
            ('Temporal', '--temporal-diff'),
            ('Flow', '--flow'),
            ('Threshold', '--threshold'),
            ('Curvature', '--curvature'),
            ('Opacity', '--opacity'),
            ('Gridfactor', '--grid-factor'),
            ('Maxlen', '--max-len'),
            ('Minlen', '--min-len'),
            ('Tensorsigma', '--tensor-sigma'),
            ('Etf', '--etf'),
            ('Etfradius', '--etf-radius'),
            ('Passes', '--passes'),
            ('Bristledensity', '--bristle-density'),
            ('Texturetaper', '--texture-taper'),
            ('Drybrush', '--dry-brush'),
            ('Lightangle', '--light-angle'),
            ('Sizejitter', '--size-jitter'),
            ('Anglejitter', '--angle-jitter'),
            ('Opacityjitter', '--opacity-jitter'),
            ('Relaxarea', '--relax-area'),
            ('Relaxmove', '--relax-move'),
            ('Relaxcandidates', '--relax-candidates'),
            ('Relaxremove', '--relax-remove'),
            ('Relaxsubpasses', '--relax-subpasses'),
            ('Flowiters', '--flow-iters')):
        _append_numeric(args, comp, parameter, flag)
    radii = comp.par.Radii.eval().strip()
    if radii:
        args.extend(('--radii', radii))
    underpaint = comp.par.Underpaint.eval()
    if underpaint != 'preset':
        args.extend(('--underpaint', underpaint))
    if comp.par.Jitterperframe.eval():
        args.append('--jitter-per-frame')
    return args


def _write_settings(settingsfile, args):
    # One argument per line; atomic replace prevents the renderer reading half a change.
    temporary = settingsfile.with_name(settingsfile.name + '.tmp')
    temporary.write_text('\n'.join(args[1:]) + '\n', encoding='utf-8')
    os.replace(temporary, settingsfile)


def update(comp):
    entry = _registry().get(comp.id)
    override = comp.par.Executable.eval().strip()
    if (entry is None or entry[0].poll() is not None
            or not comp.par.Active.eval() or entry[4] != override):
        start(comp)
        return
    proc, log, stopfile, settingsfile, _old_override = entry
    args = renderer_args(comp, proc.args[0], stopfile, settingsfile)
    _write_settings(settingsfile, args)

def stop_id(comp_id):
    entry = _registry().pop(comp_id, None)
    if entry is None:
        return
    proc, log, stopfile, settingsfile, _override = entry
    if proc.poll() is None:
        stopfile.write_text('stop', encoding='utf-8')
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
    log.close()
    if stopfile.exists():
        stopfile.unlink()
    if settingsfile.exists():
        settingsfile.unlink()


def stop(comp):
    stop_id(comp.id)


def _watch(comp_id, path):
    from td import op, run
    if comp_id not in _registry():
        _watchers().discard(comp_id)
        return
    current = op(path)
    if current is None or current.id != comp_id:
        stop_id(comp_id)
        _watchers().discard(comp_id)
        return
    run(_watch, comp_id, path, delayMilliSeconds=1000,
        delayRef=op.TDResources)


def start(comp):
    stop(comp)
    if not comp.par.Active.eval():
        return
    exe, shader_dir = prepare_runtime(comp)
    control_dir = (exe.parent if shader_dir is not None else
                   Path(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir()) / 'Paintify')
    control_dir.mkdir(parents=True, exist_ok=True)
    stopfile = control_dir / ('paintify-stop-' + str(comp.id) + '.flag')
    settingsfile = control_dir / ('paintify-settings-' + str(comp.id) + '.txt')
    if stopfile.exists():
        stopfile.unlink()
    args = renderer_args(comp, exe, stopfile, settingsfile)
    _write_settings(settingsfile, args)
    log_path = exe.parent / 'paintify-live.log'
    log = log_path.open('a', encoding='utf-8')
    env = os.environ.copy()
    if shader_dir is not None:
        env['PAINTIFY_SHADER_DIR'] = str(shader_dir)
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        proc = subprocess.Popen(args, cwd=str(exe.parent), env=env,
                                creationflags=flags, stdout=log,
                                stderr=subprocess.STDOUT)
    except Exception:
        log.close()
        raise
    _registry()[comp.id] = (proc, log, stopfile, settingsfile, comp.par.Executable.eval().strip())
    if comp.id not in _watchers():
        from td import op, run
        _watchers().add(comp.id)
        run(_watch, comp.id, comp.path, delayMilliSeconds=1000,
            delayRef=op.TDResources)
    print('Paintify started: ' + names(comp)[0] + ' -> ' + names(comp)[1])
    print('Paintify log: ' + str(log_path))
