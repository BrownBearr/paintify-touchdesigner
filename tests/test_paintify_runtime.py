import importlib.util
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
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
            Active=Parameter(True), Executable=Parameter(''),
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


if __name__ == '__main__':
    unittest.main()
