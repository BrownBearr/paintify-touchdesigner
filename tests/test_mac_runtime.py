"""Control-path regression tests; no TouchDesigner or Metal execution claimed."""
from pathlib import Path
import tempfile
import hashlib
from types import SimpleNamespace
from unittest import TestCase, mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'touchdesigner'))
import mac_runtime


def parameter(value):
    return SimpleNamespace(eval=lambda: value)


class RuntimeTests(TestCase):
    def setUp(self):
        self.source = mock.Mock()
        self.comp = SimpleNamespace(id=987654, inputs=[self.source],
            par=SimpleNamespace(Active=parameter(True), Fps=parameter(12)))
        self.script = mock.Mock(inputs=[self.source])
        self.script.id = 101
        self.script.parent.return_value = self.comp

    def tearDown(self):
        mac_runtime.registry().pop(self.comp.id, None)

    def test_throttled_cook_does_not_download_or_serialize(self):
        cached = object()
        mac_runtime.registry()[self.comp.id] = {'pixels': cached, 'last': 10}
        with mock.patch.object(mac_runtime.time, 'monotonic', return_value=10.01), \
             mock.patch.object(mac_runtime, 'settings_text') as serialize:
            mac_runtime.cook(self.script)
        self.source.numpyArray.assert_not_called()
        serialize.assert_not_called()
        self.script.copyNumpyArray.assert_called_once_with(cached)

    def test_throttled_cook_skips_copy_when_script_already_owns_cached_output(self):
        cached = object()
        entry = {'pixels': cached, 'last': 10, 'output_script_id': self.script.id,
                 'output_pixels': cached}
        mac_runtime.registry()[self.comp.id] = entry
        with mock.patch.object(mac_runtime.time, 'monotonic', return_value=10.01):
            mac_runtime.cook(self.script)
        self.script.copyNumpyArray.assert_not_called()

    def test_unchanged_key_uploads_once_per_script_instance(self):
        import numpy as np
        source_pixels = np.zeros((2, 3, 4), dtype=np.float32)
        cached = np.ones((2, 3, 4), dtype=np.float32)
        source_bytes = np.ascontiguousarray(
            np.clip(source_pixels[::-1, :, :4], 0, 1) * 255, dtype=np.uint8).tobytes()
        key = hashlib.sha256(source_bytes)
        key.update(b'same settings')
        self.source.name = 'working_resolution'
        self.source.numpyArray.return_value = source_pixels
        self.comp.par.Jitterperframe = parameter(False)
        self.comp.par.Bkrain = parameter(0)
        self.comp.par.Temporal = parameter(0)
        entry = {'pixels': cached, 'last': 0, 'key': key.digest(),
                 'settings_text': 'same settings', 'settings_dirty': False,
                 'output_script_id': self.script.id, 'output_pixels': cached}
        mac_runtime.registry()[self.comp.id] = entry
        with mock.patch.object(mac_runtime.time, 'monotonic', return_value=1), \
                mock.patch.object(mac_runtime, 'settings_text', return_value='same settings'):
            mac_runtime.cook(self.script)
        self.script.copyNumpyArray.assert_not_called()

        replacement_script = mock.Mock(inputs=[self.source])
        replacement_script.id = 202
        replacement_script.parent.return_value = self.comp
        with mock.patch.object(mac_runtime.time, 'monotonic', return_value=1), \
                mock.patch.object(mac_runtime, 'settings_text', return_value='same settings'):
            mac_runtime.cook(replacement_script)
        replacement_script.copyNumpyArray.assert_called_once_with(cached)
        self.assertEqual(entry['output_script_id'], replacement_script.id)

    def test_new_painted_pixels_upload_for_existing_script_owner(self):
        previous_pixels = object()
        new_pixels = object()
        entry = {'pixels': new_pixels, 'output_script_id': self.script.id,
                 'output_pixels': previous_pixels}
        mac_runtime.copy_cached_once(self.script, entry)
        self.script.copyNumpyArray.assert_called_once_with(new_pixels)
        self.assertIs(entry['output_pixels'], new_pixels)

    def test_parameter_change_invalidates_settings(self):
        self.comp.fetch = lambda key, default=None: default
        entry = {'settings_dirty': False}
        mac_runtime.registry()[self.comp.id] = entry
        mac_runtime.change(self.comp, SimpleNamespace(name='Opacity'))
        self.assertTrue(entry['settings_dirty'])

    def test_runtime_owned_parameters_do_not_invalidate_settings(self):
        self.comp.fetch = lambda key, default=None: default
        entry = {'settings_dirty': False}
        mac_runtime.registry()[self.comp.id] = entry
        mac_runtime.change(self.comp, SimpleNamespace(name='Status'))
        mac_runtime.change(self.comp, SimpleNamespace(name='Logfile'))
        self.assertFalse(entry['settings_dirty'])

    def test_watch_resolves_component_by_stable_id_and_updates_path(self):
        component = SimpleNamespace(id=self.comp.id, path='/project1/renamed')
        entry = {'path': '/project1/old', 'watch_token': 5}
        scheduled = []
        mac_runtime.registry()[self.comp.id] = entry
        op = lambda identifier: component
        op.TDResources = object()
        td = SimpleNamespace(op=op,
                             run=lambda *args, **kwargs: scheduled.append((args, kwargs)))
        with mock.patch.dict('sys.modules', {'td': td}):
            mac_runtime.watch(self.comp.id, 5)
        self.assertEqual(entry['path'], component.path)
        self.assertEqual(len(scheduled), 1)

    def test_stale_watch_token_cannot_schedule_duplicate_chain(self):
        mac_runtime.registry()[self.comp.id] = {'watch_token': 2}
        scheduled = []
        op = lambda identifier: self.comp
        op.TDResources = object()
        td = SimpleNamespace(op=op,
                             run=lambda *args, **kwargs: scheduled.append((args, kwargs)))
        with mock.patch.dict('sys.modules', {'td': td}):
            mac_runtime.watch(self.comp.id, 1)
        self.assertFalse(scheduled)

    def test_global_layers_pulse_invalidates_settings(self):
        self.comp.fetch = lambda key, default=None: default
        self.comp.store = mock.Mock()
        entry = {'settings_dirty': False}
        mac_runtime.registry()[self.comp.id] = entry
        with mock.patch.object(mac_runtime, 'metadata', return_value=({}, object())):
            mac_runtime.pulse(self.comp, 'Globallayers')
        self.assertTrue(entry['settings_dirty'])
        self.comp.store.assert_called_once_with('layers', [])

    def test_save_look_pulse_does_not_invalidate_settings(self):
        self.comp.par.Lookfile = parameter('unused')
        entry = {'settings_dirty': False}
        mac_runtime.registry()[self.comp.id] = entry
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'look.sbr'
            self.comp.par.Lookfile = parameter(str(path))
            with mock.patch.object(mac_runtime, 'metadata', return_value=({}, object())), \
                    mock.patch.object(mac_runtime, 'settings_text', return_value='settings'):
                mac_runtime.pulse(self.comp, 'Savelook')
            self.assertEqual(path.read_text(encoding='utf-8'), 'settings')
        self.assertFalse(entry['settings_dirty'])
