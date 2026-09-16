#!/usr/bin/env python3
"""Regression test for interactive widget reuse in meteoris_plot."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

os.environ.setdefault("MPLBACKEND", "Agg")

import h5py
import numpy as np


REPO = Path(__file__).resolve().parents[1]
PLOT_TOOL = REPO / "tools" / "meteoris_plot.py"


def load_plot_module():
    spec = importlib.util.spec_from_file_location("meteoris_plot_test_module", PLOT_TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {PLOT_TOOL}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class WidgetReuseTest(unittest.TestCase):
    def test_canvas_widget_callbacks_do_not_accumulate(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            h5_path = Path(tmp) / "plot-test.h5"
            with h5py.File(h5_path, "w") as h5:
                psd = h5.create_group("psd")
                psd.create_dataset("frequency_hz", data=np.linspace(-1000.0, 1000.0, 8))
                psd.create_dataset("power_density", data=np.ones((8, 8), dtype=np.float32))
                psd.create_dataset(
                    "timestamp_ns",
                    data=(
                        np.arange(8, dtype=np.uint64) * np.uint64(100_000_000)
                        + np.uint64(1_700_000_000_000_000_000)
                    ),
                )
                psd.create_dataset(
                    "detector_state",
                    data=np.array([0, 1, 1, 2, 0, 1, 2, 0], dtype=np.uint8),
                )
                h5.create_group("metadata").attrs["software_version"] = mp.__version__

            with h5py.File(h5_path, "r") as h5:
                timestamps = h5["/psd/timestamp_ns"][:]
                event = mp.EventSegment(
                    ordinal=1,
                    stored_event_id=1,
                    start_row=0,
                    stop_row=8,
                    start_ns=int(timestamps[0]),
                    stop_ns=int(timestamps[-1]),
                )

                fig = None
                baseline_counts = None
                watched_events = (
                    "button_press_event",
                    "button_release_event",
                    "motion_notify_event",
                    "key_press_event",
                    "key_release_event",
                )

                for _ in range(5):
                    fig, _ = mp.plot_event(
                        h5,
                        event,
                        1,
                        mp.gqrx_colormap(),
                        None,
                        None,
                        None,
                        mp.PlotConfig(),
                        None,
                        reuse_fig=fig,
                    )
                    counts = {
                        name: len(fig.canvas.callbacks.callbacks.get(name, {}))
                        for name in watched_events
                    }
                    if baseline_counts is None:
                        baseline_counts = counts
                    else:
                        self.assertEqual(counts, baseline_counts)

                self.assertEqual(len(fig._meteoris_widgets), 6)

                # The fifth plot's Max PSD button must still invoke its callback.
                max_psd_button = fig._meteoris_widgets[2]
                max_psd_axis = fig.axes[0]
                self.assertTrue(max_psd_axis.get_visible())
                max_psd_button._observers.process("clicked", None)
                self.assertFalse(max_psd_axis.get_visible())
                max_psd_button._observers.process("clicked", None)
                self.assertTrue(max_psd_axis.get_visible())

    def test_button_defaults_from_plot_config_survive_reuse(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            cfg_path = tmp_path / "meteoris_plot.toml"
            cfg_path.write_text(
                "[buttons]\n"
                "max_psd = false\n"
                "trigger = false\n"
                "grid = true\n",
                encoding="utf-8",
            )
            cfg = mp.load_plot_config(cfg_path)
            self.assertFalse(cfg.button_max_psd)
            self.assertFalse(cfg.button_trigger)
            self.assertTrue(cfg.button_grid)

            h5_path = tmp_path / "plot-test.h5"
            with h5py.File(h5_path, "w") as h5:
                psd = h5.create_group("psd")
                psd.create_dataset("frequency_hz", data=np.linspace(-1000.0, 1000.0, 8))
                psd.create_dataset("power_density", data=np.ones((8, 8), dtype=np.float32))
                psd.create_dataset(
                    "timestamp_ns",
                    data=(
                        np.arange(8, dtype=np.uint64) * np.uint64(100_000_000)
                        + np.uint64(1_700_000_000_000_000_000)
                    ),
                )
                psd.create_dataset(
                    "detector_state",
                    data=np.array([0, 1, 1, 2, 0, 1, 2, 0], dtype=np.uint8),
                )
                h5.create_group("metadata").attrs["software_version"] = mp.__version__

            with h5py.File(h5_path, "r") as h5:
                timestamps = h5["/psd/timestamp_ns"][:]
                event = mp.EventSegment(
                    ordinal=1, stored_event_id=1, start_row=0, stop_row=8,
                    start_ns=int(timestamps[0]), stop_ns=int(timestamps[-1]),
                )
                fig = None
                for _ in range(2):
                    fig, _ = mp.plot_event(
                        h5, event, 1, mp.gqrx_colormap(), None, None, None,
                        cfg, None, reuse_fig=fig,
                    )
                    max_axis = fig.axes[0]
                    waterfall_axis = fig.axes[1]
                    self.assertFalse(max_axis.get_visible())
                    self.assertTrue(any(line.get_visible() for line in waterfall_axis.get_xgridlines()))

                    widgets = fig._meteoris_widgets
                    self.assertEqual(widgets[2].label.get_text(), "Max PSD: OFF")
                    self.assertEqual(widgets[3].label.get_text(), "Trigger: OFF")
                    self.assertEqual(widgets[4].label.get_text(), "Grid: ON")
                    self.assertEqual(widgets[5].label.get_text(), "Band: HALF")

                    trigger_lines = [
                        line for line in waterfall_axis.lines
                        if line.get_label() in ("trigger ON", "trigger OFF", "_child0", "_child1")
                    ]
                    # Trigger marker lines exist but all are hidden by the configured default.
                    marker_lines = [line for line in waterfall_axis.lines if line.get_linestyle() in ("--", ":")]
                    self.assertTrue(marker_lines)
                    self.assertTrue(all(not line.get_visible() for line in marker_lines))


    def test_bandwidth_default_toggle_and_summary_follow_visible_band(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            h5_path = Path(tmp) / "band-test.h5"
            freq = np.linspace(-4000.0, 4000.0, 9)
            # Outer bins deliberately dominate so Full and Half summaries differ.
            db_rows = np.array([
                [-30, -80, -70, -60, -50, -60, -70, -80, -20],
                [-25, -85, -75, -65, -55, -65, -75, -85, -15],
            ], dtype=np.float32)
            power = np.power(10.0, db_rows / 10.0).astype(np.float32)
            with h5py.File(h5_path, "w") as h5:
                psd = h5.create_group("psd")
                psd.create_dataset("frequency_hz", data=freq)
                psd.create_dataset("power_density", data=power)
                psd.create_dataset(
                    "timestamp_ns",
                    data=np.array([1_700_000_000_000_000_000, 1_700_000_000_100_000_000], dtype=np.uint64),
                )
                h5.create_group("metadata").attrs["software_version"] = mp.__version__

            with h5py.File(h5_path, "r") as h5:
                ts = h5["/psd/timestamp_ns"][:]
                event = mp.EventSegment(
                    ordinal=1, stored_event_id=1, start_row=0, stop_row=2,
                    start_ns=int(ts[0]), stop_ns=int(ts[-1]),
                )
                cfg = mp.PlotConfig(button_bandwidth="half")
                fig, _ = mp.plot_event(
                    h5, event, 1, mp.gqrx_colormap(), None, None, None, cfg, None
                )
                waterfall_axis = fig.axes[1]
                max_axis = fig.axes[0]
                band_button = fig._meteoris_widgets[5]

                self.assertEqual(band_button.label.get_text(), "Band: HALF")
                lo, hi = waterfall_axis.get_ylim()
                self.assertAlmostEqual(lo, -2.0)
                self.assertAlmostEqual(hi, 2.0)
                half_mask = (freq >= -2000.0) & (freq <= 2000.0)
                np.testing.assert_allclose(
                    max_axis.lines[0].get_ydata(), np.max(db_rows[:, half_mask], axis=1), atol=1e-4
                )
                np.testing.assert_allclose(
                    max_axis.lines[1].get_ydata(), np.median(db_rows[:, half_mask], axis=1), atol=1e-4
                )

                band_button._observers.process("clicked", None)
                self.assertEqual(band_button.label.get_text(), "Band: FULL")
                lo, hi = waterfall_axis.get_ylim()
                self.assertAlmostEqual(lo, -4.0)
                self.assertAlmostEqual(hi, 4.0)
                np.testing.assert_allclose(
                    max_axis.lines[0].get_ydata(), np.max(db_rows, axis=1), atol=1e-4
                )
                np.testing.assert_allclose(
                    max_axis.lines[1].get_ydata(), np.median(db_rows, axis=1), atol=1e-4
                )

    def test_bandwidth_config_validation(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "plot.toml"
            cfg_path.write_text('[buttons]\nbandwidth = "full"\n', encoding="utf-8")
            self.assertEqual(mp.load_plot_config(cfg_path).button_bandwidth, "full")
            cfg_path.write_text('[buttons]\nbandwidth = "quarter"\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "buttons.bandwidth"):
                mp.load_plot_config(cfg_path)


    def test_window_mode_config_and_validation(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "plot.toml"
            cfg_path.write_text(
                '[plot]\nwindow_mode = "normal"\nfigure_width = 9.5\nfigure_height = 6.0\n',
                encoding="utf-8",
            )
            cfg = mp.load_plot_config(cfg_path)
            self.assertEqual(cfg.window_mode, "normal")
            self.assertEqual(cfg.figure_width, 9.5)
            self.assertEqual(cfg.figure_height, 6.0)

            cfg_path.write_text('[plot]\nwindow_mode = "fullscreen"\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "plot.window_mode"):
                mp.load_plot_config(cfg_path)

    def test_apply_window_mode_reapplies_native_state(self):
        mp = load_plot_module()

        class FakeWindow:
            def __init__(self):
                self.maximized_calls = 0
                self.normal_calls = 0

            def showMaximized(self):
                self.maximized_calls += 1

            def showNormal(self):
                self.normal_calls += 1

        class FakeManager:
            def __init__(self, window):
                self.window = window

        class FakeCanvas:
            def __init__(self, manager):
                self.manager = manager

        class FakeFigure:
            def __init__(self, window):
                self.canvas = FakeCanvas(FakeManager(window))
                self.size_calls = []

            def set_size_inches(self, width, height, forward=False):
                self.size_calls.append((width, height, forward))

        window = FakeWindow()
        fig = FakeFigure(window)

        max_cfg = mp.PlotConfig(window_mode="maximized")
        mp.apply_window_mode(fig, max_cfg)
        mp.apply_window_mode(fig, max_cfg)
        self.assertEqual(window.maximized_calls, 2)
        self.assertEqual(fig.size_calls, [])

        normal_cfg = mp.PlotConfig(
            window_mode="normal", figure_width=10.0, figure_height=6.5
        )
        mp.apply_window_mode(fig, normal_cfg)
        self.assertEqual(window.normal_calls, 1)
        self.assertEqual(fig.size_calls[-1], (10.0, 6.5, True))

    def test_maximized_reuse_does_not_resize_live_figure(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            h5_path = Path(tmp) / "plot-test.h5"
            with h5py.File(h5_path, "w") as h5:
                psd = h5.create_group("psd")
                psd.create_dataset("frequency_hz", data=np.linspace(-1000.0, 1000.0, 8))
                psd.create_dataset("power_density", data=np.ones((8, 8), dtype=np.float32))
                psd.create_dataset(
                    "timestamp_ns",
                    data=(
                        np.arange(8, dtype=np.uint64) * np.uint64(100_000_000)
                        + np.uint64(1_700_000_000_000_000_000)
                    ),
                )
                psd.create_dataset(
                    "detector_state",
                    data=np.array([0, 1, 1, 2, 0, 1, 2, 0], dtype=np.uint8),
                )
                h5.create_group("metadata").attrs["software_version"] = mp.__version__

            with h5py.File(h5_path, "r") as h5:
                timestamps = h5["/psd/timestamp_ns"][:]
                event = mp.EventSegment(
                    ordinal=1, stored_event_id=1, start_row=0, stop_row=8,
                    start_ns=int(timestamps[0]), stop_ns=int(timestamps[-1]),
                )
                cfg = mp.PlotConfig(window_mode="maximized")
                fig, _ = mp.plot_event(
                    h5, event, 1, mp.gqrx_colormap(), None, None, None,
                    cfg, None,
                )
                calls = []
                real_set_size = fig.set_size_inches

                def tracked_set_size(*args, **kwargs):
                    calls.append((args, kwargs))
                    return real_set_size(*args, **kwargs)

                fig.set_size_inches = tracked_set_size
                fig, _ = mp.plot_event(
                    h5, event, 1, mp.gqrx_colormap(), None, None, None,
                    cfg, None, reuse_fig=fig,
                )
                self.assertEqual(calls, [])

    def test_button_config_requires_boolean_values(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "bad.toml"
            cfg_path.write_text('[buttons]\ngrid = "true"\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "buttons.grid must be true or false"):
                mp.load_plot_config(cfg_path)


if __name__ == "__main__":
    unittest.main()
