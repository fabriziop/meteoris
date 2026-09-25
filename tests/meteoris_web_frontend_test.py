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
        self.assertIn('>Vertical</button>', html)
        self.assertIn("let waterfallOrientation = 'horizontal';", app)
        self.assertIn("function drawHorizontal(frame, floor, ceiling)", app)
        self.assertIn("function drawVertical(frame, floor, ceiling)", app)
        self.assertIn("canvas.width-1, 0", app)
        self.assertIn("range.last - Math.floor(y * count / canvas.height)", app)
        self.assertIn("waterfallOrientation === 'horizontal' ? 'vertical' : 'horizontal'", app)

    def test_waterfall_uses_meteoris_plot_gqrx_palette(self) -> None:
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn("Match meteoris_plot.gqrx_colormap()", app)
        self.assertIn("if (i < 20)", app)
        self.assertIn("else if (i < 70)", app)
        self.assertIn("else if (i < 100)", app)
        self.assertIn("else if (i < 150)", app)
        self.assertIn("else if (i < 250)", app)
        self.assertIn("let floorDb = -130;", app)
        self.assertIn("let ceilingDb = -60;", app)
        self.assertIn("drawHorizontal(frame, floorDb, ceilingDb)", app)

    def test_waterfall_stop_go_button_toggles_live_updates(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="waterfallToggle"', html)
        self.assertIn('>Stop</button>', html)
        self.assertIn("let waterfallRunning = true;", app)
        self.assertIn("waterfallRunning ? 'Stop' : 'Go'", app)
        self.assertIn("if (!waterfallRunning) return;", app)
        self.assertIn("waterfallRunning = !waterfallRunning;", app)

    def test_waterfall_sound_is_muted_by_default_and_limited_to_5khz(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="waterfallSound"', html)
        self.assertIn('aria-pressed="false"', html)
        self.assertIn('>Sound</button>', html)
        self.assertIn("const AUDIO_MAX_HZ = 5000;", app)
        self.assertIn("const AUDIO_GATE_DB = 10;", app)
        self.assertIn("let audioEnabled = false;", app)
        self.assertIn("function updatePsdAudio(frame)", app)
        self.assertIn("const noisePower = sortedPower[Math.floor(sortedPower.length / 2)]", app)
        self.assertIn("let peakPower = 1e-30;", app)
        self.assertIn("const normalization = 0.35 / Math.max(1, weightSum);", app)
        self.assertIn("waterfallSound.textContent = audioEnabled ? 'Mute' : 'Sound';", app)
        self.assertIn("updatePsdAudio(f);", app)


    def test_waterfall_controls_are_below_canvas_and_labels_are_compact(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        canvas_pos = html.index('id="waterfall"')
        controls_pos = html.index('class="spectrum-controls"')
        self.assertGreater(controls_pos, canvas_pos)
        self.assertNotIn('Stop waterfall', html)
        self.assertNotIn('Waterfall: horizontal', html)

    def test_frequency_scale_and_configured_band_controls(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('id="frequencyScale"', html)
        self.assertIn('id="bandMin"', html)
        self.assertIn('id="bandMax"', html)
        self.assertIn('.waterfall-stage.vertical', css)
        self.assertIn('.waterfall-stage.horizontal', css)
        self.assertIn("function parseConfiguredBandwidth(toml)", app)
        self.assertIn("inDsp = section[1].trim() === 'dsp'", app)
        self.assertIn("bandwidth_hz", app)
        self.assertIn("bandMinHz = -bandwidthHz / 2", app)
        self.assertIn("bandMaxHz = bandwidthHz / 2", app)
        self.assertIn("function visibleBinRange(frame)", app)
        self.assertIn("function updateFrequencyScale()", app)
        self.assertIn("tick.style.top = `${100 - position}%`", app)

    def test_frequency_scale_has_major_medium_and_minor_ticks(self) -> None:
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn("function scaleTickMarks(lo, hi)", app)
        self.assertIn("subdivision === 2 ? 'medium' : 'minor'", app)
        self.assertIn("kind: 'major'", app)
        self.assertIn(".frequency-tick.medium::before", css)
        self.assertIn(".frequency-tick.major::before", css)

    def test_waterfall_controls_use_one_aligned_standardized_row(self) -> None:
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        self.assertIn(".spectrum-controls {", css)
        self.assertIn("align-items:flex-end", css)
        self.assertIn("flex-wrap:nowrap", css)
        self.assertIn('.spectrum-controls button { width:172px; }', css)
        self.assertIn('.spectrum-controls input[type="number"] { width:118px;', css)
        self.assertIn("border-color:#4a4a4a", css)

    def test_all_waterfall_controls_scroll_when_the_row_does_not_fit(self) -> None:
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        controls = css[css.index(".spectrum-controls {"):css.index(".spectrum-controls .toolbar-actions,")]
        self.assertIn("min-width:0", controls)
        self.assertIn("max-width:100%", controls)
        self.assertIn("flex-wrap:nowrap", controls)
        self.assertIn("overflow-x:auto", controls)
        self.assertIn("overflow-y:hidden", controls)
        self.assertIn("-webkit-overflow-scrolling:touch", controls)
        self.assertIn(".spectrum-controls .toolbar-actions {\n  flex:0 0 auto;", css)
        self.assertIn(".spectrum-controls .range { flex:0 0 auto; }", css)


    def test_vertical_scale_ticks_share_canvas_side_and_endpoint_labels_stay_inside(self) -> None:
        css = (ROOT / "web" / "style.css").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn(".waterfall-stage.vertical .frequency-tick {", css)
        self.assertIn("bottom:0", css)
        self.assertIn(".waterfall-stage.vertical .frequency-tick.edge-start", css)
        self.assertIn(".waterfall-stage.vertical .frequency-tick.edge-end", css)
        self.assertIn("tick.classList.add('edge-start')", app)
        self.assertIn("tick.classList.add('edge-end')", app)

    def test_orientation_button_names_the_target_orientation(self) -> None:
        html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertIn('>Vertical</button>', html)
        self.assertIn("orientationButton.textContent = horizontal ? 'Vertical' : 'Horizontal';", app)

    def test_numeric_control_edits_commit_on_enter(self) -> None:
        app = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
        self.assertNotIn("bandMinInput.addEventListener('change'", app)
        self.assertNotIn("bandMaxInput.addEventListener('change'", app)
        self.assertIn("function applyLevelInputs()", app)
        self.assertIn("if (e.key === 'Enter')", app)
        self.assertIn("applyLevelInputs();", app)
        self.assertIn("applyBandInputs();", app)
        self.assertIn("document.querySelector(`button[data-input=\"${id}\"]`)?.click();", app)

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
