#!/usr/bin/env python3
"""Smoke checks for the browser frontend assets."""

from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MeteorisWebFrontendTest(unittest.TestCase):
    def test_required_assets_exist(self) -> None:
        for name in ("index.html", "app.js", "style.css", "meteoris_web.py"):
            self.assertTrue((ROOT / "web" / name).is_file(), name)

    def test_status_refresh_preserves_live_control_edits(self) -> None:
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn("state.dirty", app)
        self.assertIn("document.activeElement !== input", app)
        self.assertIn("input.addEventListener('input'", app)
        self.assertIn("if (j.ok) liveInputs[inputId].dirty = false", app)

    @unittest.skipUnless(shutil.which("node"), "node is not installed")
    def test_app_js_syntax(self) -> None:
        subprocess.run(
            [shutil.which("node"), "--check", str(ROOT / "web" / "app.js")],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
