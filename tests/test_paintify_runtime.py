import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
import sys
from unittest.mock import patch


RUNTIME_PATH = Path(__file__).resolve().parents[1] / 'touchdesigner' / 'paintify_runtime.py'
SPEC = importlib.util.spec_from_file_location('paintify_runtime', RUNTIME_PATH)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class Parameter:
    def __init__(self, value):
        self.value = value

    def eval(self):
        return self.value


class FakeComp:
    def __init__(self, with_max_dimension=True):
        self.path = '/project1/paintify'
        self.id = 101
        self.par = SimpleNamespace(
            Active=Parameter(True), Executable=Parameter(''), Status=Parameter('Stopped'),
            Fps=Parameter(12), Preset=Parameter('impressionist'),
            Relax=Parameter(0), Brushtexture=Parameter(-1),
            Impasto=Parameter(0.35), Impastolight=Parameter(0.5),
            Temporal=Parameter(0), Flow=Parameter(0), Threshold=Parameter(-1),
            Curvature=Parameter(-1), Opacity=Parameter(-1), Gridfactor=Parameter(-1),
            Maxlen=Parameter(-1), Minlen=Parameter(-1), Tensorsigma=Parameter(-1),
            Etf=Parameter(-1), Etfradius=Parameter(-1), Passes=Parameter(4),
            Bristledensity=Parameter(-1), Texturetaper=Parameter(-1),
            Drybrush=Parameter(-1), Lightangle=Parameter(-1), Sizejitter=Parameter(-1),
            Anglejitter=Parameter(-1), Opacityjitter=Parameter(-1),
            Relaxarea=Parameter(-1), Relaxmove=Parameter(-1),
            Relaxcandidates=Parameter(-1), Relaxremove=Parameter(-1),
            Relaxsubpasses=Parameter(-1), Flowiters=Parameter(-1),
            Radii=Parameter(''), Underpaint=Parameter('preset'),
            Jitterperframe=Parameter(False),
        )
        if with_max_dimension:
            self.par.Maxdimension = Parameter(1280)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        runtime._registry().clear()
        runtime._watchers().clear()

    def tearDown(self):
        runtime._registry().clear()
        runtime._watchers().clear()

    def test_renderer_args_adds_live_resolution_cap(self):
        comp = FakeComp()
        args = runtime.renderer_args(comp, Path('gpu-sbr.exe'), Path('stop'), Path('settings'))
        index = args.index('--live-max-dimension')
        self.assertEqual(args[index + 1], '1280')

    def test_legacy_component_without_cap_remains_supported(self):
        comp = FakeComp(with_max_dimension=False)
        args = runtime.renderer_args(comp, Path('gpu-sbr.exe'), Path('stop'), Path('settings'))
        self.assertNotIn('--live-max-dimension', args)

    def test_identical_settings_do_not_replace_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.txt'
            args = ['gpu-sbr.exe', '--preset', 'impressionist']
            self.assertTrue(runtime._write_settings(path, args))
            contents = path.read_text(encoding='utf-8')
            with patch.object(runtime.os, 'replace', wraps=runtime.os.replace) as replace:
                self.assertFalse(runtime._write_settings(path, args))
                replace.assert_not_called()
            self.assertEqual(path.read_text(encoding='utf-8'), contents)

    def test_changed_settings_are_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.txt'
            self.assertTrue(runtime._write_settings(path, ['renderer', '--passes', '4']))
            self.assertTrue(runtime._write_settings(path, ['renderer', '--passes', '8']))
            self.assertEqual(path.read_text(encoding='utf-8'), '--passes\n8\n')

    def test_update_skips_unchanged_settings_and_writes_changes(self):
        comp = FakeComp()
        process = SimpleNamespace(args=['gpu-sbr.exe'], poll=lambda: None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stopfile = root / 'stop.flag'
            settingsfile = root / 'settings.txt'
            args = runtime.renderer_args(comp, Path(process.args[0]), stopfile, settingsfile)
            runtime._write_settings(settingsfile, args)
            entry = (process, None, stopfile, settingsfile, '')
            with patch.object(runtime, '_registry', return_value={comp.id: entry}), \
                    patch.object(runtime.os, 'replace', wraps=runtime.os.replace) as replace:
                runtime.update(comp)
                replace.assert_not_called()
                comp.par.Maxdimension.value = 0
                runtime.update(comp)
                replace.assert_called_once()

    def test_watcher_resolves_stable_id_and_restarts_after_move_once(self):
        comp = FakeComp()
        comp.path = '/project1/moved'
        process = SimpleNamespace(poll=lambda: None)
        entry = (process, None, Path('stop'), Path('settings'), '', '/project1/paintify')
        scheduled = []
        op = lambda identifier: comp
        op.TDResources = object()
        td = SimpleNamespace(op=op, run=lambda *a, **k: scheduled.append((a, k)))
        with patch.dict(sys.modules, {'td': td}), \
                patch.object(runtime, '_registry', return_value={comp.id: entry}), \
                patch.object(runtime, '_watchers', return_value={comp.id}), \
                patch.object(runtime, 'start') as start:
            runtime._watch(comp.id)
        start.assert_called_once_with(comp)
        self.assertEqual(len(scheduled), 1)
        self.assertEqual(scheduled[0][0], (runtime._watch, comp.id))

    def test_watcher_stops_when_stable_id_is_destroyed(self):
        comp = FakeComp()
        process = SimpleNamespace(poll=lambda: None)
        entry = (process, None, Path('stop'), Path('settings'), '', comp.path)
        scheduled = []
        op = lambda identifier: None
        op.TDResources = object()
        td = SimpleNamespace(op=op, run=lambda *a, **k: scheduled.append(a))
        with patch.dict(sys.modules, {'td': td}), \
                patch.object(runtime, '_registry', return_value={comp.id: entry}), \
                patch.object(runtime, '_watchers', return_value={comp.id}), \
                patch.object(runtime, 'stop_id') as stop_id:
            runtime._watch(comp.id)
        stop_id.assert_called_once_with(comp.id, remove_watcher=True)
        self.assertFalse(scheduled)

    def test_watcher_cleans_up_exited_renderer_without_restart_loop(self):
        comp = FakeComp()
        process = SimpleNamespace(poll=lambda: 7, returncode=7)
        entry = (process, None, Path('stop'), Path('settings'), '', comp.path)
        scheduled = []
        op = lambda identifier: comp
        op.TDResources = object()
        td = SimpleNamespace(op=op, run=lambda *a, **k: scheduled.append(a))
        with patch.dict(sys.modules, {'td': td}), \
                patch.object(runtime, '_registry', return_value={comp.id: entry}), \
                patch.object(runtime, '_watchers', return_value={comp.id}), \
                patch.object(runtime, 'stop_id') as stop_id:
            runtime._watch(comp.id)
        self.assertEqual(comp.par.Status.val, 'Renderer exited (code 7)')
        stop_id.assert_called_once_with(comp.id, remove_watcher=True)
        self.assertFalse(scheduled)

    def test_start_failure_sets_readable_status_and_removes_partial_files(self):
        comp = FakeComp()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / 'renderer.exe'
            executable.write_bytes(b'renderer')
            comp.par.Executable.value = str(executable)
            with patch.object(runtime.subprocess, 'Popen', side_effect=OSError('launch denied')):
                with self.assertRaisesRegex(OSError, 'launch denied'):
                    runtime.start(comp)
            self.assertEqual(comp.par.Status.val, 'Failed: launch denied')
            self.assertFalse((root / ('paintify-stop-' + str(comp.id) + '.flag')).exists())
            self.assertFalse((root / ('paintify-settings-' + str(comp.id) + '.txt')).exists())
            self.assertNotIn(comp.id, runtime._registry())

    def test_pulse_restart_respects_active(self):
        comp = FakeComp()
        comp.par.Active.value = False
        with patch.object(runtime, 'start') as start:
            runtime.pulse(comp, 'Restart')
        start.assert_called_once_with(comp)

    def test_stop_then_start_reuses_pending_watcher_chain(self):
        comp = FakeComp()
        scheduled = []
        op = lambda identifier: comp
        op.TDResources = object()
        td = SimpleNamespace(op=op, run=lambda *a, **k: scheduled.append((a, k)))

        class Process:
            args = ['renderer.exe']
            returncode = None

            def poll(self):
                return None

            def wait(self, timeout=None):
                return 0

        processes = []

        def launch(*args, **kwargs):
            proc = Process()
            processes.append(proc)
            return proc

        with tempfile.TemporaryDirectory() as directory, \
                patch.dict(sys.modules, {'td': td}), \
                patch.dict(runtime.os.environ, {'LOCALAPPDATA': directory}), \
                patch.object(runtime, 'prepare_runtime', return_value=(Path(directory) / 'renderer.exe', None)), \
                patch.object(runtime, 'renderer_args', return_value=['renderer.exe']), \
                patch.object(runtime.subprocess, 'Popen', side_effect=launch):
            runtime.start(comp)
            runtime.stop(comp)
            self.assertIn(comp.id, runtime._watchers())
            runtime.start(comp)
            runtime.stop(comp)

        self.assertEqual(len(processes), 2)
        self.assertEqual(len(scheduled), 1)

    def test_status_helper_tolerates_legacy_components(self):
        comp = SimpleNamespace(par=SimpleNamespace())
        runtime.set_status(comp, 'Running')


if __name__ == '__main__':
    unittest.main()
