import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "touchdesigner"))
import mac_controls as controls

DATA = json.loads((ROOT / "touchdesigner/brushkit_data.json").read_text())


class ControlTests(unittest.TestCase):
    def test_slider_units_and_pages(self):
        core = {row[0]: row for row in controls.CORE}
        self.assertEqual(core["angle-jitter"][3:5], (0, 45))
        self.assertEqual(core["bristle-density"][3:5], (2, 30))
        self.assertEqual(controls.SLIDER_RANGES["widthScale"], (.3, 2.5))
        self.assertEqual(controls.core_page("threshold"), "Paintify")
        self.assertEqual(controls.core_page("flow-levels"), "Motion")
        self.assertEqual(controls.core_page("brush-texture"), "Classic Brush")

    def test_all_brushes_preserve_independent_settings(self):
        block = list(DATA["defaults"])
        block[DATA["fields"].index("saturation")] = .42
        block[DATA["fields"].index("finish")] = 1
        for brush in DATA["brushes"]:
            selected = controls.select_brush(DATA, brush["key"], block)
            self.assertEqual(selected[56], .42)
            self.assertEqual(selected[76], 1)

    def test_shared_format_roundtrip_including_palette_layers(self):
        block = controls.select_brush(DATA, "tempera", DATA["defaults"])
        block[132:136] = [.2, .3, .4, 1]
        layers = ["layer 0 16 4 50 1 .9 1"]
        text = controls.serialize({"opacity": .75}, [8, 4, 2], "blur", block, "tempera", layers)
        restored = controls.parse(text, DATA)
        self.assertEqual(restored["layers"], layers)
        self.assertEqual(restored["radii"], [8, 4, 2])
        self.assertEqual(restored["core"]["opacity"], .75)
        self.assertEqual(restored["block"][132:136], [.2, .3, .4, 1])
        self.assertEqual(len(restored["block"]), 260)

    def test_parameter_names_are_unique(self):
        names = [controls.parname(field, "Bk") for field in DATA["fields"]]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(name[0].isupper() and name[1:].islower() for name in names))

    def test_invalid_style_block_rejected(self):
        with self.assertRaises(ValueError):
            controls.parse("style-block 1 2 3", DATA)
