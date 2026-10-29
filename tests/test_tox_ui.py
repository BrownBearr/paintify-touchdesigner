"""Behavioral regressions for native Parameter Execute callback ordering."""
from pathlib import Path
from types import SimpleNamespace
from unittest import TestCase
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'touchdesigner'))
import tox_ui


class Parameter:
    def __init__(self, name, value):
        self.name, self.val = name, value

    def eval(self):
        return self.val


class PresetTests(TestCase):
    def setUp(self):
        self.storage = {'ui_preset_applied': 'balanced'}
        values = dict(Performance='balanced', Maxdimension=1280, Passes=4,
                      Relax=0, Fps=17, Preset='wash')
        self.comp = SimpleNamespace(par=SimpleNamespace(**{
            name: Parameter(name, value) for name, value in values.items()}),
            fetch=lambda name, default=None: self.storage.get(name, default),
            store=lambda name, value: self.storage.__setitem__(name, value))

    def test_stale_field_callback_before_menu_does_not_cancel_new_preset(self):
        self.comp.par.Performance.val = 'preview'
        tox_ui.change(self.comp, self.comp.par.Maxdimension)
        self.assertEqual(self.comp.par.Performance.eval(), 'preview')
        tox_ui.change(self.comp, self.comp.par.Performance)
        self.assertEqual(self.comp.par.Maxdimension.eval(), 640)

    def test_manual_override_after_preset_switches_to_custom(self):
        self.comp.par.Performance.val = 'preview'
        tox_ui.change(self.comp, self.comp.par.Performance)
        self.comp.par.Maxdimension.val = 1920
        tox_ui.change(self.comp, self.comp.par.Maxdimension)
        self.assertEqual(self.comp.par.Performance.eval(), 'custom')
        self.assertEqual(self.comp.par.Maxdimension.eval(), 1920)

    def test_quality_preset_preserves_user_fps_and_look(self):
        self.comp.par.Performance.val = 'quality'
        tox_ui.change(self.comp, self.comp.par.Performance)
        self.assertEqual(self.comp.par.Fps.eval(), 17)
        self.assertEqual(self.comp.par.Preset.eval(), 'wash')
        self.assertEqual(self.comp.par.Passes.eval(), 8)
