"""Runtime embedded in Paintify.tox by install_paintify.py."""
import builtins
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

IS_MAC = sys.platform == 'darwin'
# macOS ships the renderer as one tarball: the TOX's virtual file system would
# lose the executable bit and the framework's symlinks, a tarball keeps both.
MAC_RUNTIME = 'runtime/mac/runtime.tar'
MAC_EXECUTABLE = 'paintify-mac'


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


def _cache_base():
    if IS_MAC:
        return Path.home() / 'Library' / 'Caches'
    return Path(os.environ.get('LOCALAPPDATA') or tempfile.gettempdir())


def _runtime_files(comp):
    return sorted(
        (item for item in comp.vfs.find()
         if item.name.startswith('runtime/') and not item.name.startswith('runtime/mac/')),
        key=lambda item: item.name)


def _safe_members(archive, destination):
    root = destination.resolve()
    for member in archive.getmembers():
        target = (root / member.name).resolve()
        if root != target and root not in target.parents:
            raise RuntimeError('Unsafe path in the bundled renderer: ' + member.name)
        yield member


def _prepare_mac(comp):
    blobs = [item for item in comp.vfs.find() if item.name == MAC_RUNTIME]
    if not blobs:
        raise RuntimeError('This Paintify.tox has no macOS renderer bundled. '
                           'Build it with tools/build-mac.sh and re-run '
                           'touchdesigner/install_paintify.py on the Mac.')
    data = bytes(blobs[0].byteArray)
    digest = hashlib.sha256(data).hexdigest()[:20]
    runtime_dir = _cache_base() / 'Paintify' / digest
    marker = runtime_dir / '.complete'
    if not marker.is_file():
        staging = runtime_dir.with_name(digest + '.tmp')
        if staging.exists():
            shutil.rmtree(staging)
        staging.mkdir(parents=True)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            members = list(_safe_members(archive, staging))
            try:
                archive.extractall(staging, members=members, filter='data')
            except TypeError:   # Python without extraction filters
                archive.extractall(staging, members=members)
        if runtime_dir.exists():
            shutil.rmtree(runtime_dir)
        os.replace(staging, runtime_dir)
        marker.write_text('ok', encoding='utf-8')
    exe = runtime_dir / MAC_EXECUTABLE
    if not exe.is_file():
        raise FileNotFoundError('Bundled Paintify renderer is missing')
    exe.chmod(0o755)
    return exe, runtime_dir / 'shaders'


def prepare_runtime(comp):
    override = comp.par.Executable.eval().strip()
    if override:
        exe = Path(override)
        if not exe.is_file():
            raise FileNotFoundError('Paintify renderer not found: ' + override)
        return exe, None

    if IS_MAC:
        return _prepare_mac(comp)

    files = _runtime_files(comp)
    if not files:
        raise RuntimeError('Paintify.tox has no bundled renderer')
    digest = hashlib.sha256()
    for item in files:
        digest.update(item.name.encode('utf-8'))
        digest.update(item.byteArray)
    runtime_dir = _cache_base() / 'Paintify' / digest.hexdigest()[:20]
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
    if IS_MAC:
        # Syphon frames are bottom-row-first; see the Paintify Advanced page.
        for parameter, flag, default in (('Flipinput', '--flip-in', True),
                                         ('Flipoutput', '--flip-out', True)):
            par = getattr(comp.par, parameter, None)
            value = par.eval() if par is not None else default
            args.extend((flag, '1' if value else '0'))
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
                   _cache_base() / 'Paintify')
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
    extra = {}
    if not IS_MAC:
        extra['creationflags'] = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        proc = subprocess.Popen(args, cwd=str(exe.parent), env=env,
                                stdout=log, stderr=subprocess.STDOUT, **extra)
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
