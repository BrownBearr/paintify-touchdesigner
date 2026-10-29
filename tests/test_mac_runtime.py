"""Control-path regression tests; no TouchDesigner or Metal execution claimed."""
from pathlib import Path
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

    def test_parameter_change_invalidates_settings(self):
        self.comp.fetch = lambda key, default=None: default
        entry = {'settings_dirty': False}
        mac_runtime.registry()[self.comp.id] = entry
        mac_runtime.change(self.comp, SimpleNamespace(name='Opacity'))
        self.assertTrue(entry['settings_dirty'])
