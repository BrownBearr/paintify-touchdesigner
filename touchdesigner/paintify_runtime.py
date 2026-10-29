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


def set_status(comp, value):
    """Update the optional read-only status parameter without breaking legacy TOXs."""
    par = getattr(comp.par, 'Status', None)
    if par is None:
        return
    try:
        par.val = value
    except Exception as error:
        print('Paintify could not update Status: ' + str(error))


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
    # Older saved TOXs do not have this custom parameter. Leave their source
    # at native size; newly installed components expose an explicit cap.
    max_dimension = getattr(comp.par, 'Maxdimension', None)
    if max_dimension is not None:
        args.extend(('--live-max-dimension', str(max_dimension.eval())))
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
    contents = '\n'.join(args[1:]) + '\n'
    if settingsfile.is_file() and settingsfile.read_text(encoding='utf-8') == contents:
        return False
    temporary = settingsfile.with_name(settingsfile.name + '.tmp')
    temporary.write_text(contents, encoding='utf-8')
    os.replace(temporary, settingsfile)
    return True


def update(comp):
    entry = _registry().get(comp.id)
    override = comp.par.Executable.eval().strip()
    if entry is not None and entry[0].poll() is not None:
        set_status(comp, 'Renderer exited (code ' + str(entry[0].returncode) + ')')
        # Keep the scheduling guard until its already-pending callback observes
        # the empty registry and ends the chain.
        stop_id(comp.id)
        return
    if entry is None or not comp.par.Active.eval() or entry[4] != override:
        start(comp)
        return
    proc, log, stopfile, settingsfile, _old_override, *_path = entry
    args = renderer_args(comp, proc.args[0], stopfile, settingsfile)
    _write_settings(settingsfile, args)

def stop_id(comp_id, remove_watcher=False):
    entry = _registry().pop(comp_id, None)
    if entry is None:
        if remove_watcher:
            _watchers().discard(comp_id)
        return
    proc, log, stopfile, settingsfile, _override, *_path = entry
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
    if remove_watcher:
        _watchers().discard(comp_id)


def stop(comp):
    # A delayed _watch may already be queued. Preserve its guard so a rapid
    # stop/start reuses that chain instead of scheduling a second one.
    stop_id(comp.id)
    set_status(comp, 'Stopped')


def _watch(comp_id, legacy_path=None):
    from td import op, run
    entry = _registry().get(comp_id)
    if entry is None:
        _watchers().discard(comp_id)
        return
    current = op(comp_id)
    if current is None:
        stop_id(comp_id, remove_watcher=True)
        return
    proc = entry[0]
    if proc.poll() is not None:
        set_status(current, 'Renderer exited (code ' + str(proc.returncode) + ')')
        stop_id(comp_id, remove_watcher=True)
        return
    old_path = entry[5] if len(entry) > 5 else legacy_path
    if old_path is None:
        old_path = current.path
    if len(entry) == 5:
        entry = entry + (old_path,)
        _registry()[comp_id] = entry
    if current.path != old_path:
        # Spout channel names derive from comp.path; restart exactly once while
        # this watcher chain continues to own the component ID.
        try:
            start(current)
        except Exception:
            _watchers().discard(comp_id)
            return
        entry = _registry().get(comp_id)
        if entry is None:
            return
    if comp_id in _registry():
        run(_watch, comp_id, delayMilliSeconds=1000,
            delayRef=op.TDResources)
        return


def start(comp):
    stop_id(comp.id)
    if not comp.par.Active.eval():
        set_status(comp, 'Stopped')
        return
    set_status(comp, 'Waiting')
    log = None
    stopfile = settingsfile = None
    try:
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
        logfile_par = getattr(comp.par, 'Logfile', None)
        if logfile_par is not None:
            logfile_par.val = str(log_path)
        log = log_path.open('a', encoding='utf-8')
        env = os.environ.copy()
        if shader_dir is not None:
            env['PAINTIFY_SHADER_DIR'] = str(shader_dir)
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        proc = subprocess.Popen(args, cwd=str(exe.parent), env=env,
                                creationflags=flags, stdout=log,
                                stderr=subprocess.STDOUT)
    except Exception as error:
        if log is not None:
            log.close()
        for path in (stopfile, settingsfile):
            if path is not None and path.exists():
                path.unlink()
        set_status(comp, 'Failed: ' + str(error))
        raise
    _registry()[comp.id] = (proc, log, stopfile, settingsfile,
                            comp.par.Executable.eval().strip(), comp.path)
    set_status(comp, 'Running')
    if comp.id not in _watchers():
        from td import op, run
        _watchers().add(comp.id)
        run(_watch, comp.id, delayMilliSeconds=1000,
            delayRef=op.TDResources)
    print('Paintify started: ' + names(comp)[0] + ' -> ' + names(comp)[1])
    print('Paintify log: ' + str(log_path))


def pulse(comp, name):
    if name == 'Restart':
        start(comp)
