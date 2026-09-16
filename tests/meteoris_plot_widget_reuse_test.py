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


if __name__ == "__main__":
    unittest.main()
