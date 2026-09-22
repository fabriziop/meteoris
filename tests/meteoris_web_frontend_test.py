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

    def test_bottom_status_bar_replaces_header_status(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="statusBar"', html)
        self.assertIn('id="statusBadge"', html)
        self.assertNotIn('id="connection"', html)
        self.assertNotIn("position:fixed", css.replace(" ", ""))
        self.assertIn(".status-badge.connecting", css)
        self.assertIn(".status-badge.connected", css)
        self.assertIn("connected' : 'connecting", app)
        self.assertIn("to meteoris", app)
        self.assertIn("setDspStatus(false", app)



    def test_session_uses_http_listen_address_once(self) -> None:
        gateway = (ROOT / "web" / "meteoris_web.py").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('return web.json_response({"ok": True, "listen_ip": listen_ip, "version": meteoris_version()})', gateway)
        self.assertIn('app.router.add_get("/api/session", session)', gateway)
        self.assertIn("fetch('/api/session')", app)
        self.assertEqual(app.count("fetch('/api/session')"), 1)
        self.assertNotIn('j.status.listen_ip', app)
        self.assertIn('make_app(gateway, find_web_root(args.web_root), args.listen)', gateway)
        self.assertIn("sessionVersion = j.version || '';", app)

    def test_connecting_and_connected_text_share_same_suffix(self) -> None:
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn("const suffix = `to meteoris", app)
        self.assertIn("sessionVersion ? ` ${sessionVersion}`", app)
        self.assertIn("`${connected ? 'connected' : 'connecting'} ${suffix}`", app)

    def test_waterfall_orientation_toggle_defaults_horizontal(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="waterfallOrientation"', html)
        self.assertIn('Waterfall: horizontal', html)
        self.assertIn("let waterfallOrientation = 'horizontal';", app)
        self.assertIn("function drawHorizontal(frame, floor, ceiling)", app)
        self.assertIn("function drawVertical(frame, floor, ceiling)", app)
        self.assertIn("canvas.width-1, 0", app)
        self.assertIn("waterfallOrientation === 'horizontal' ? 'vertical' : 'horizontal'", app)

    def test_web_assets_disable_browser_cache(self) -> None:
        gateway = (ROOT / "web" / "meteoris_web.py").read_text(encoding="utf-8")
        self.assertIn('Cache-Control', gateway)
        self.assertIn('no-store', gateway)
        self.assertIn('static_response(web_root / "index.html")', gateway)
        self.assertIn('static_response(web_root / "app.js")', gateway)
        self.assertIn('static_response(web_root / "style.css")', gateway)

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
