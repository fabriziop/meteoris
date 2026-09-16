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

                self.assertEqual(len(fig._meteoris_widgets), 5)

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

                    trigger_lines = [
                        line for line in waterfall_axis.lines
                        if line.get_label() in ("trigger ON", "trigger OFF", "_child0", "_child1")
                    ]
                    # Trigger marker lines exist but all are hidden by the configured default.
                    marker_lines = [line for line in waterfall_axis.lines if line.get_linestyle() in ("--", ":")]
                    self.assertTrue(marker_lines)
                    self.assertTrue(all(not line.get_visible() for line in marker_lines))

    def test_button_config_requires_boolean_values(self):
        mp = load_plot_module()
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / "bad.toml"
            cfg_path.write_text('[buttons]\ngrid = "true"\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "buttons.grid must be true or false"):
                mp.load_plot_config(cfg_path)


if __name__ == "__main__":
    unittest.main()
