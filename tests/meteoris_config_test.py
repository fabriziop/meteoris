#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import tempfile
import sys
import tomllib
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("meteoris_config", ROOT / "tools" / "meteoris_config.py")
assert SPEC and SPEC.loader
mc = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = mc
SPEC.loader.exec_module(mc)


class MeteorisConfigTests(unittest.TestCase):
    def test_schema_is_complete_and_smart_is_subset(self) -> None:
        sections, _ = mc.load_schema(ROOT / "config" / "meteoris.toml")
        all_params = [p for s in sections for p in s.parameters]
        smart = mc._ordered_parameters(sections, "smart")
        self.assertGreater(len(all_params), 50)
        self.assertGreater(len(smart), 10)
        self.assertLess(len(smart), len(all_params))
        self.assertFalse(any(p.explanation.startswith("Meteoris setting") for p in all_params))

    def test_current_file_controls_known_order(self) -> None:
        sections, _ = mc.load_schema(ROOT / "config" / "meteoris.toml")
        with tempfile.TemporaryDirectory() as td:
            current = Path(td) / "current.toml"
            current.write_text(
                '[output]\ndirectory = "/srv/meteor"\nfile_prefix = "site"\n'
                '[sdr]\ngain = 42\ndriver = "hackrf"\n',
                encoding="utf-8",
            )
            ordered = mc.reorder_schema_for_current(sections, current)
            self.assertEqual(ordered[0].name, "output")
            self.assertEqual([p.key for p in ordered[0].parameters[:2]], ["directory", "file_prefix"])
            self.assertEqual(ordered[0].parameters[0].number, "1.1")
            self.assertEqual(ordered[1].name, "sdr")
            self.assertEqual([p.key for p in ordered[1].parameters[:2]], ["gain", "driver"])


    def test_numeric_mode_choice(self) -> None:
        with patch("builtins.input", side_effect=["2"]):
            self.assertEqual(mc._choose_mode(None), "expert")
        with patch("builtins.input", side_effect=[""]):
            self.assertEqual(mc._choose_mode(None), "smart")
        with patch("builtins.input", side_effect=["smart", "1"]):
            self.assertEqual(mc._choose_mode(None), "smart")

    def test_review_edit_loop_modifies_by_number(self) -> None:
        sections, values = mc.load_schema(ROOT / "config" / "meteoris.toml")
        smart = mc._ordered_parameters(sections, "smart")
        target = smart[0]
        original = mc._nested_get(values, target.dotted)[1]
        replacement = original
        if isinstance(original, str):
            replacement_text = original + "_changed"
            replacement = replacement_text
        elif isinstance(original, bool):
            replacement_text = "false" if original else "true"
            replacement = not original
        elif isinstance(original, int):
            replacement_text = str(original + 1)
            replacement = original + 1
        else:
            replacement_text = str(float(original) + 1.0)
            replacement = float(original) + 1.0

        # Review -> no; select number; enter replacement; blank -> review; yes.
        with patch("builtins.input", side_effect=["n", target.number, replacement_text, "", "y"]):
            visible = mc._review_edit_loop(smart, sections, values)
        self.assertIn(target, visible)
        self.assertEqual(mc._nested_get(values, target.dotted)[1], replacement)

    def test_meteoris_version_matches_project(self) -> None:
        self.assertEqual(mc._meteoris_version(), (ROOT / "VERSION").read_text(encoding="utf-8").strip())

    def test_generated_toml_is_complete_and_parseable(self) -> None:
        sections, values = mc.load_schema(ROOT / "config" / "meteoris.toml")
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "new.toml"
            mc._write_toml(out, sections, values, "test")
            parsed = tomllib.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(parsed["detector"]["plugin"], values["detector"]["plugin"])
            self.assertEqual(parsed["output"]["directory"], values["output"]["directory"])
            text = out.read_text(encoding="utf-8")
            self.assertIn("# 1.1 ", text)
            self.assertIn("[detector.echoes]", text)


if __name__ == "__main__":
    unittest.main()
