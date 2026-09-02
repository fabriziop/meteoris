#!/usr/bin/env python3
"""
.+
.context    : Meteoris triggered PSD recorder
.title      : HDF5 triggered-PSD event viewer and waterfall plotter
.kind       : Python analysis/plot program
.author     : Fabrizio Pollastri <mxgbot@gmail.com>
.site       : Revello - Italy
.creation   : 2026-08-23
.license    : MIT (see LICENSE file)
.description
Inspect HDF5 files produced by Meteoris and display recorded detector events.

The program:
  - displays acquisition metadata and the embedded Meteoris TOML configuration;
  - reads active Meteoris HDF5 files through HDF5 SWMR mode;
  - reports the number and properties of detected events;
  - displays one selected event at a time as a time-frequency waterfall;
    - advances to the next event with SPACE/ENTER;
    - jumps to a typed event number when SPACE/ENTER is pressed;
    - refreshes a live SWMR file with the R key;
    - skips ahead by 10/20/50 events with Z/X/C;
    - terminate the interactive browser with Q or by closing the window;
  - provides interactive PSD color-scale minimum/maximum sliders;
  - provides show/hide controls for the X/Y grid and trigger markers;
  - marks trigger ON/OFF positions;
  - can save selected event plots as PNG files.

The waterfall uses a Matplotlib reproduction of the classic Gqrx palette:
black -> blue -> cyan/green -> yellow -> red -> white.

Examples:
  python3 meteoris_plot.py psd_data/meteoris_20260823.h5
  python3 meteoris_plot.py psd_data/meteoris_20260823.h5 --events 1-3
  python3 meteoris_plot.py psd_data/meteoris_20260823.h5 --events 2,5,8-10
  python3 meteoris_plot.py psd_data/meteoris_20260823.h5 --list-events
.-
"""

from __future__ import annotations

__version__ = "0.2.0"
__author__ = "Fabrizio Pollastri <mxgbot@gmail.com>"


import argparse
import datetime as dt
import sys

try:
    import tomllib
except ImportError:
    tomllib = None
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import h5py
    import numpy as np
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    from matplotlib.colors import ListedColormap
    from matplotlib.widgets import Slider, Button
except ImportError as exc:
    missing = getattr(exc, "name", "required Python package")
    raise SystemExit(
        f"Missing Python package: {missing}\n"
        "Install dependencies on Kubuntu with, for example:\n"
        "  sudo apt install python3-numpy python3-matplotlib python3-h5py"
    ) from exc


@dataclass
class PlotConfig:
    db_min: float | None = None
    db_max: float | None = None
    slider_min_db: float = -180.0
    slider_max_db: float = 20.0
    slider_step_db: float = 1.0
    figure_width: float = 11.5
    figure_height: float = 7.2


def load_plot_config(path: Path) -> PlotConfig:
    cfg = PlotConfig()
    if not path.exists():
        return cfg
    if tomllib is None:
        raise ValueError("TOML configuration requires Python 3.11+ (tomllib)")
    with path.open("rb") as f:
        data = tomllib.load(f)
    plot = data.get("plot", {})
    scale = data.get("color_scale", {})
    if "db_min" in scale:
        cfg.db_min = float(scale["db_min"])
    if "db_max" in scale:
        cfg.db_max = float(scale["db_max"])
    if "slider_min_db" in scale:
        cfg.slider_min_db = float(scale["slider_min_db"])
    if "slider_max_db" in scale:
        cfg.slider_max_db = float(scale["slider_max_db"])
    if "slider_step_db" in scale:
        cfg.slider_step_db = float(scale["slider_step_db"])
    if "figure_width" in plot:
        cfg.figure_width = float(plot["figure_width"])
    if "figure_height" in plot:
        cfg.figure_height = float(plot["figure_height"])
    if cfg.slider_max_db <= cfg.slider_min_db:
        raise ValueError("color_scale.slider_max_db must be greater than slider_min_db")
    if cfg.slider_step_db <= 0:
        raise ValueError("color_scale.slider_step_db must be > 0")
    if cfg.db_min is not None and cfg.db_max is not None and cfg.db_max <= cfg.db_min:
        raise ValueError("color_scale.db_max must be greater than db_min")
    return cfg


@dataclass
class EventSegment:
    ordinal: int          # file-local event number, 1-based
    stored_event_id: int  # event_id written by Meteoris
    start_row: int
    stop_row: int         # exclusive
    start_ns: int
    stop_ns: int

    @property
    def rows(self) -> int:
        return self.stop_row - self.start_row

    @property
    def duration_s(self) -> float:
        return max(0.0, (self.stop_ns - self.start_ns) * 1e-9)


def gqrx_colormap() -> ListedColormap:
    """Return the classic 256-entry Gqrx-like waterfall palette."""
    rgb = np.zeros((256, 3), dtype=np.float64)
    for i in range(256):
        if i < 20:
            r, g, b = 0, 0, 0
        elif i < 70:
            r, g, b = 0, 0, 140 * (i - 20) / 50.0
        elif i < 100:
            r = 60 * (i - 70) / 30.0
            g = 125 * (i - 70) / 30.0
            b = 115 * (i - 70) / 30.0 + 140
        elif i < 150:
            r = 195 * (i - 100) / 50.0 + 60
            g = 130 * (i - 100) / 50.0 + 125
            b = 255 - 255 * (i - 100) / 50.0
        elif i < 250:
            r = 255
            g = 255 - 255 * (i - 150) / 100.0
            b = 0
        else:
            r = 255
            g = 255 * (i - 250) / 5.0
            b = 255 * (i - 250) / 5.0
        rgb[i] = np.clip((r, g, b), 0, 255) / 255.0
    return ListedColormap(rgb, name="gqrx_classic")


def decode_value(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, np.generic):
        return value.item()
    return value


def ns_to_utc(ns: int) -> dt.datetime:
    return dt.datetime.fromtimestamp(int(ns) * 1e-9, tz=dt.timezone.utc)


def print_metadata(h5: h5py.File, show_toml: bool = False) -> None:
    print(f"File: {h5.filename}")
    if "metadata" not in h5:
        print("Metadata: <missing /metadata group>")
        return

    meta = h5["metadata"]
    print("\n=== Metadata ===")
    for key in sorted(meta.attrs.keys()):
        print(f"{key} = {decode_value(meta.attrs[key])}")

    if "toml_config" in meta:
        raw = meta["toml_config"][()]
        if isinstance(raw, np.ndarray):
            if raw.dtype.kind in ("S", "U"):
                raw = b"".join(raw.tolist()) if raw.dtype.kind == "S" else "".join(raw.tolist())
            else:
                raw = bytes(np.asarray(raw, dtype=np.uint8).tolist())
        if isinstance(raw, bytes):
            toml = raw.decode("utf-8", errors="replace")
        else:
            toml = str(raw)
        print(f"toml_config_bytes = {len(toml.encode('utf-8'))}")
        if show_toml:
            print("\n=== Embedded TOML ===")
            print(toml.rstrip())


def refresh_live_datasets(h5: h5py.File) -> None:
    """Refresh datasets that can grow while Meteoris is the SWMR writer."""
    if not getattr(h5, "swmr_mode", False):
        return
    for name in (
        "/psd/power_density",
        "/psd/timestamp_ns",
        "/psd/frame_index",
        "/psd/event_id",
        "/psd/detector_state",
        "/psd/detector_max_db_hz",
        "/psd/gain_db",
    ):
        if name in h5:
            h5[name].refresh()


def open_meteoris_h5(path: Path) -> tuple[h5py.File, bool]:
    """Prefer SWMR live-reader mode, with fallback for older closed files."""
    swmr_error = None
    try:
        h5 = h5py.File(path, "r", libver="latest", swmr=True)
        return h5, True
    except OSError as exc:
        swmr_error = exc

    try:
        h5 = h5py.File(path, "r")
        return h5, False
    except OSError as exc:
        raise OSError(
            f"cannot open {path}. SWMR open failed: {swmr_error}; "
            f"normal read open failed: {exc}. If Meteoris is currently writing "
            "this file, it must be produced with [output] swmr = true."
        ) from exc


def infer_frame_period_ns(h5: h5py.File, timestamps: np.ndarray) -> int | None:
    """Estimate PSD frame spacing for restart/gap detection."""
    if len(timestamps) >= 2:
        d = np.diff(timestamps.astype(np.int64))
        good = d[(d > 0) & (d < 10_000_000_000)]
        if len(good):
            # Lower half avoids long inter-event gaps biasing the estimate.
            q50 = np.percentile(good, 50)
            near = good[good <= q50 * 1.5]
            if len(near):
                return int(np.median(near))

    try:
        out_rate = float(h5["metadata"].attrs["output_rate_hz"])
        nfft = int(h5["metadata"].attrs["fft_size"])
        # Meteoris currently uses 50% FFT overlap.
        return int(round((nfft / 2.0) / out_rate * 1e9))
    except Exception:
        return None


def find_events(h5: h5py.File) -> list[EventSegment]:
    required = ["/psd/event_id", "/psd/timestamp_ns"]
    for name in required:
        if name not in h5:
            raise ValueError(f"Missing required dataset: {name}")

    refresh_live_datasets(h5)
    event_ids = np.asarray(h5["/psd/event_id"], dtype=np.uint64)
    timestamps = np.asarray(h5["/psd/timestamp_ns"], dtype=np.uint64)
    if len(event_ids) != len(timestamps):
        if getattr(h5, "swmr_mode", False):
            # A reader can catch the writer between two dataset flushes. Only
            # consume the mutually published prefix and retry on next refresh.
            n = min(len(event_ids), len(timestamps))
            event_ids = event_ids[:n]
            timestamps = timestamps[:n]
        else:
            raise ValueError("event_id and timestamp_ns dataset lengths differ")
    if len(event_ids) == 0:
        return []

    period_ns = infer_frame_period_ns(h5, timestamps)
    # A gap well beyond normal PSD cadence means a new recorded event/session,
    # even if event_id restarted and reused the same number.
    max_same_event_gap = period_ns * 4 if period_ns else 2_000_000_000

    starts = [0]
    for i in range(1, len(event_ids)):
        id_changed = event_ids[i] != event_ids[i - 1]
        gap = int(timestamps[i]) - int(timestamps[i - 1])
        time_discontinuity = gap <= 0 or gap > max_same_event_gap
        if id_changed or time_discontinuity:
            starts.append(i)
    starts.append(len(event_ids))

    events: list[EventSegment] = []
    for ordinal, (a, b) in enumerate(zip(starts[:-1], starts[1:]), start=1):
        if b <= a:
            continue
        events.append(
            EventSegment(
                ordinal=ordinal,
                stored_event_id=int(event_ids[a]),
                start_row=a,
                stop_row=b,
                start_ns=int(timestamps[a]),
                stop_ns=int(timestamps[b - 1]),
            )
        )
    return events


def print_event_table(h5: h5py.File, events: list[EventSegment]) -> None:
    print(f"\nDetected events in file: {len(events)}")
    if not events:
        return

    states = h5["/psd/detector_state"] if "/psd/detector_state" in h5 else None
    gains = h5["/psd/gain_db"] if "/psd/gain_db" in h5 else None

    print("\n#   stored_id  PSDs   pre  active  post  status       duration(s)  start UTC                         gain dB")
    print("--  ---------  -----  ---  ------  ----  -----------  -----------  --------------------------------  ----------------")
    for ev in events:
        n_pre = n_active = n_post = "-"
        status = "UNKNOWN"
        if states is not None:
            s = np.asarray(states[ev.start_row:ev.stop_row], dtype=np.uint8)
            n_pre = str(int(np.count_nonzero(s == 0)))
            n_active = str(int(np.count_nonzero(s == 1)))
            n_post = str(int(np.count_nonzero(s == 2)))

            # A normal completed event ends after its configured post-trigger
            # context. The HDF5 v0 stream has no explicit "event complete"
            # marker, so classify conservatively from the visible state tail.
            if s.size and s[-1] == 2:
                status = "COMPLETE"
            elif s.size and s[-1] == 1:
                status = "ACTIVE?"
            else:
                status = "INCOMPLETE"

        gain_txt = "-"
        if gains is not None:
            g = np.asarray(gains[ev.start_row:ev.stop_row], dtype=float)
            if len(g):
                gain_txt = f"{np.nanmin(g):.1f}..{np.nanmax(g):.1f}"
        start = ns_to_utc(ev.start_ns).isoformat(timespec="milliseconds")
        print(
            f"{ev.ordinal:<3d} {ev.stored_event_id:<10d} {ev.rows:<6d} "
            f"{n_pre:<4s} {n_active:<7s} {n_post:<5s} {status:<12s} "
            f"{ev.duration_s:<12.3f} {start:<33s} {gain_txt}"
        )


def parse_event_selection(spec: str | None, count: int) -> list[int]:
    if count == 0:
        return []
    if spec is None or spec.strip().lower() in {"all", "*"}:
        return list(range(1, count + 1))

    selected: set[int] = set()
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if "-" in token:
            a_s, b_s = token.split("-", 1)
            a, b = int(a_s), int(b_s)
            if b < a:
                a, b = b, a
            selected.update(range(a, b + 1))
        else:
            selected.add(int(token))

    bad = sorted(i for i in selected if i < 1 or i > count)
    if bad:
        raise ValueError(f"Event number(s) outside 1..{count}: {bad}")
    return sorted(selected)


def auto_color_limits(db: np.ndarray) -> tuple[float, float]:
    finite = db[np.isfinite(db)]
    if not finite.size:
        return -140.0, -60.0
    vmin = float(np.percentile(finite, 2.0))
    vmax = float(np.percentile(finite, 99.7))
    if vmax - vmin < 10.0:
        mid = 0.5 * (vmin + vmax)
        vmin, vmax = mid - 5.0, mid + 5.0
    return vmin, vmax


def plot_event(
    h5: h5py.File,
    ev: EventSegment,
    cmap: ListedColormap,
    db_min: float | None,
    db_max: float | None,
    save_dir: Path | None,
    plot_cfg: PlotConfig,
) -> tuple[plt.Figure, dict[str, bool]]:
    refresh_live_datasets(h5)
    freq_hz = np.asarray(h5["/psd/frequency_hz"], dtype=float)
    visible_stop = min(
        ev.stop_row,
        int(h5["/psd/power_density"].shape[0]),
        int(h5["/psd/timestamp_ns"].shape[0]),
    )
    if visible_stop <= ev.start_row:
        raise ValueError("Selected event has no fully published PSD rows yet; refresh and retry")
    power = np.asarray(h5["/psd/power_density"][ev.start_row:visible_stop, :], dtype=float)
    ts_ns = np.asarray(h5["/psd/timestamp_ns"][ev.start_row:visible_stop], dtype=np.uint64)

    if power.ndim != 2 or power.shape[1] != len(freq_hz):
        raise ValueError(f"PSD shape {power.shape} does not match frequency axis {len(freq_hz)}")

    db = 10.0 * np.log10(np.maximum(power, np.finfo(np.float32).tiny))
    auto_min, auto_max = auto_color_limits(db)
    vmin = auto_min if db_min is None else db_min
    vmax = auto_max if db_max is None else db_max
    if vmax <= vmin:
        raise ValueError("--db-max must be greater than --db-min")

    # Matplotlib dates are days. pcolormesh handles actual PSD timestamps and
    # therefore also shows small irregularities/gaps faithfully.
    t_dt = [ns_to_utc(int(x)) for x in ts_ns]
    t_num = mdates.date2num(t_dt)

    fig, ax = plt.subplots(figsize=(plot_cfg.figure_width, plot_cfg.figure_height))
    fig.subplots_adjust(bottom=0.24)
    mesh = ax.pcolormesh(
        t_num,
        freq_hz / 1000.0,
        db.T,
        shading="nearest",
        cmap=cmap,
        vmin=vmin,
        vmax=vmax,
        rasterized=True,
    )
    ax.xaxis_date()
    locator = mdates.AutoDateLocator(minticks=3, maxticks=9)
    ax.xaxis.set_major_locator(locator)
    ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(locator, tz=dt.timezone.utc))
    ax.set_xlabel("Time (UTC)")
    ax.set_ylabel("Frequency offset (kHz)")

    # X/Y grid, disabled by default. A button below the waterfall toggles the
    # major grid without changing the PSD data or color normalization.
    grid_state = {"visible": False}
    # Draw the grid above the waterfall QuadMesh; otherwise it can be hidden
    # underneath the coloured PSD map.
    ax.set_axisbelow(False)
    ax.grid(False, which="major", axis="both")

    title = f"Meteoris event {ev.ordinal} (stored event_id={ev.stored_event_id})"
    ax.set_title(title)
    cb = fig.colorbar(mesh, ax=ax, pad=0.02)
    cb.set_label("PSD density (dB/Hz)")

    # Mark every trigger ON/OFF transition. Retriggers that occur during the
    # post-trigger tail are intentionally merged into the same Meteoris event,
    # so an event may contain several active/post-active intervals.
    trigger_lines = []
    trigger_state = {"visible": True}
    trigger_legend = None
    if "/psd/detector_state" in h5:
        state = np.asarray(h5["/psd/detector_state"][ev.start_row:visible_stop], dtype=np.uint8)
        if state.size:
            # ON = first active frame after any non-active state.
            on_idx = np.flatnonzero(
                (state == 1) &
                np.r_[True, state[:-1] != 1]
            )
            # OFF = first post-trigger frame after an active state.
            off_idx = np.flatnonzero(
                (state == 2) &
                np.r_[False, state[:-1] == 1]
            )

            for j, idx in enumerate(on_idx):
                trigger_lines.append(
                    ax.axvline(
                        t_num[int(idx)],
                        linestyle="--",
                        linewidth=1.6,
                        color="magenta",
                        alpha=1.0,
                        zorder=14,
                        label="trigger ON" if j == 0 else None,
                    )
                )

            for j, idx in enumerate(off_idx):
                trigger_lines.append(
                    ax.axvline(
                        t_num[int(idx)],
                        linestyle=":",
                        linewidth=1.6,
                        color="lime",
                        alpha=1.0,
                        zorder=14,
                        label="trigger OFF" if j == 0 else None,
                    )
                )

            if trigger_lines:
                trigger_legend = ax.legend(loc="upper right")

    # Interactive color-scale controls. The slider bounds are configurable
    # and automatically expanded if the initial limits fall outside them.
    slider_lo = min(plot_cfg.slider_min_db, vmin - 1.0)
    slider_hi = max(plot_cfg.slider_max_db, vmax + 1.0)
    ax_min = fig.add_axes([0.14, 0.115, 0.70, 0.025])
    ax_max = fig.add_axes([0.14, 0.070, 0.70, 0.025])
    ax_trigger = fig.add_axes([0.72, 0.020, 0.12, 0.035])
    ax_grid = fig.add_axes([0.86, 0.020, 0.10, 0.035])
    s_min = Slider(ax_min, "Color min (dB/Hz)", slider_lo, slider_hi,
                   valinit=vmin, valstep=plot_cfg.slider_step_db)
    s_max = Slider(ax_max, "Color max (dB/Hz)", slider_lo, slider_hi,
                   valinit=vmax, valstep=plot_cfg.slider_step_db)
    b_trigger = Button(ax_trigger, "Trigger: ON")
    b_grid = Button(ax_grid, "Grid: OFF")

    def update_scale(_value=None):
        lo = float(s_min.val)
        hi = float(s_max.val)
        # Keep a valid normalization without making the sliders fight the user.
        if hi <= lo:
            return
        mesh.set_clim(lo, hi)
        fig.canvas.draw_idle()

    def toggle_grid(_event=None):
        grid_state["visible"] = not grid_state["visible"]
        if grid_state["visible"]:
            # Style arguments are supplied only when enabling the grid.
            ax.set_axisbelow(False)
            ax.grid(True, which="major", axis="both",
                    linewidth=1.15, alpha=0.85, zorder=10)
        else:
            # Passing style properties together with visible=False makes
            # Matplotlib re-enable the grid and emits a warning.
            ax.grid(False, which="major", axis="both")
        b_grid.label.set_text("Grid: ON" if grid_state["visible"] else "Grid: OFF")
        fig.canvas.draw_idle()

    def toggle_triggers(_event=None):
        trigger_state["visible"] = not trigger_state["visible"]
        for line in trigger_lines:
            line.set_visible(trigger_state["visible"])
        if trigger_legend is not None:
            trigger_legend.set_visible(trigger_state["visible"])
        b_trigger.label.set_text(
            "Trigger: ON" if trigger_state["visible"] else "Trigger: OFF"
        )
        fig.canvas.draw_idle()

    s_min.on_changed(update_scale)
    s_max.on_changed(update_scale)
    b_trigger.on_clicked(toggle_triggers)
    b_grid.on_clicked(toggle_grid)
    # Keep widget references alive for the lifetime of the figure.
    fig._meteoris_widgets = (s_min, s_max, b_trigger, b_grid)

    # Keyboard navigation:
    #   SPACE/ENTER          -> next event
    #   digits + SPACE/ENTER -> jump to that file-local event number
    #   Z / X / C            -> skip ahead by 10 / 20 / 50 events
    #   R              -> refresh a live SWMR file
    #   Q              -> quit interactive browsing
    # Closing the window normally also stops interactive browsing.
    navigation = {
        "advance_by": None,
        "jump_to": None,
        "digits": "",
        "refresh": False,
        "quit": False,
    }

    def on_key(event):
        key = event.key or ""
        if len(key) == 1 and key.isdigit():
            navigation["digits"] += key
            try:
                fig.canvas.manager.set_window_title(
                    f"Meteoris event {ev.ordinal} | jump: {navigation['digits']}"
                )
            except Exception:
                pass
            return

        if key in ("backspace", "delete"):
            navigation["digits"] = navigation["digits"][:-1]
            try:
                suffix = f" | jump: {navigation['digits']}" if navigation["digits"] else ""
                fig.canvas.manager.set_window_title(
                    f"Meteoris event {ev.ordinal}{suffix}"
                )
            except Exception:
                pass
            return

        if key == "escape":
            navigation["digits"] = ""
            try:
                fig.canvas.manager.set_window_title(f"Meteoris event {ev.ordinal}")
            except Exception:
                pass
            return

        if key.lower() == "z":
            navigation["advance_by"] = 10
            navigation["digits"] = ""
            plt.close(fig)
            return

        if key.lower() == "x":
            navigation["advance_by"] = 20
            navigation["digits"] = ""
            plt.close(fig)
            return

        if key.lower() == "c":
            navigation["advance_by"] = 50
            navigation["digits"] = ""
            plt.close(fig)
            return

        if key.lower() == "q":
            navigation["quit"] = True
            navigation["digits"] = ""
            plt.close(fig)
            return

        if key.lower() == "r":
            navigation["refresh"] = True
            navigation["digits"] = ""
            plt.close(fig)
            return

        if key in (" ", "space", "enter", "return"):
            if navigation["digits"]:
                navigation["jump_to"] = int(navigation["digits"])
            else:
                navigation["advance_by"] = 1
            plt.close(fig)

    fig.canvas.mpl_connect("key_press_event", on_key)

    if save_dir is not None:
        save_dir.mkdir(parents=True, exist_ok=True)
        stamp = ns_to_utc(ev.start_ns).strftime("%Y%m%dT%H%M%S")
        path = save_dir / f"event_{ev.ordinal:04d}_{stamp}Z.png"
        fig.savefig(path, dpi=150, bbox_inches="tight")
        print(f"Saved: {path}")

    return fig, navigation


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Inspect Meteoris HDF5 PSD recordings and display event waterfalls."
    )
    p.add_argument("--version", action="version",
                   version=f"%(prog)s {__version__} — {__author__}")
    p.add_argument("file", type=Path, help="Meteoris daily .h5 file")
    p.add_argument("--config", type=Path, default=Path("meteoris_plot.toml"),
                   help="plot configuration TOML (default: meteoris_plot.toml)")
    p.add_argument(
        "--events",
        metavar="RANGE",
        help="event selection, e.g. 1-5 or 1,3,7-9 (default: all)",
    )
    p.add_argument(
        "--list-events",
        action="store_true",
        help="display metadata/event summary only; do not open plots",
    )
    p.add_argument(
        "--show-toml",
        action="store_true",
        help="also print the embedded Meteoris TOML configuration",
    )
    p.add_argument("--db-min", type=float, help="fixed waterfall lower level in dB/Hz")
    p.add_argument("--db-max", type=float, help="fixed waterfall upper level in dB/Hz")
    p.add_argument(
        "--save-dir",
        type=Path,
        help="also save each selected event as PNG in this directory",
    )
    p.add_argument(
        "--no-show",
        action="store_true",
        help="do not open GUI windows (useful together with --save-dir)",
    )
    return p


def main(argv: Iterable[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    try:
        plot_cfg = load_plot_config(args.config)
    except (OSError, ValueError) as exc:
        print(f"Error loading plot config {args.config}: {exc}", file=sys.stderr)
        return 2

    # CLI color limits override TOML values.
    effective_db_min = args.db_min if args.db_min is not None else plot_cfg.db_min
    effective_db_max = args.db_max if args.db_max is not None else plot_cfg.db_max
    if effective_db_min is not None and effective_db_max is not None and effective_db_max <= effective_db_min:
        print("Error: db-max must be greater than db-min", file=sys.stderr)
        return 2

    if args.config.exists():
        print(f"Plot config: {args.config}")
    else:
        print(f"Plot config: {args.config} (not found; using defaults)")

    if not args.file.exists():
        print(f"Error: file not found: {args.file}", file=sys.stderr)
        return 2

    try:
        h5, swmr_live = open_meteoris_h5(args.file)
        with h5:
            print(f"HDF5 mode: {'SWMR reader (live-safe)' if swmr_live else 'normal read (legacy/closed file)'}")
            for required in ("/psd/frequency_hz", "/psd/power_density"):
                if required not in h5:
                    raise ValueError(f"Not a compatible Meteoris file: missing {required}")

            print_metadata(h5, show_toml=args.show_toml)
            events = find_events(h5)
            print_event_table(h5, events)

            if args.list_events or not events:
                return 0

            selected = parse_event_selection(args.events, len(events))
            print("\nBrowsing file-local event(s): " + ", ".join(map(str, selected)))
            cmap = gqrx_colormap()

            if args.no_show:
                # Headless mode still renders/saves every selected event.
                for n in selected:
                    fig, _navigation = plot_event(
                        h5, events[n - 1], cmap,
                        effective_db_min, effective_db_max,
                        args.save_dir, plot_cfg
                    )
                    plt.close(fig)
            else:
                print(
                    "Press SPACE or ENTER for next event. Type an event number then SPACE/ENTER to jump; "
                    "Z/X/C skip by 10/20/50 events; R refreshes a live SWMR file; "
                    "Q quits; Backspace edits, Esc clears. "
                    "Close the window to stop."
                )
                pos = 0
                selected_pos = {event_no: i for i, event_no in enumerate(selected)}
                while 0 <= pos < len(selected):
                    n = selected[pos]
                    fig, navigation = plot_event(
                        h5, events[n - 1], cmap,
                        effective_db_min, effective_db_max,
                        args.save_dir, plot_cfg
                    )
                    try:
                        fig.canvas.manager.set_window_title(
                            f"Meteoris event {n} ({pos + 1}/{len(selected)})"
                        )
                    except Exception:
                        pass
                    plt.show(block=True)

                    if navigation.get("quit"):
                        break

                    if navigation.get("refresh"):
                        current_event_no = n
                        events = find_events(h5)
                        print(f"SWMR refresh: visible_events={len(events)}")
                        if args.events is None:
                            selected = list(range(1, len(events) + 1))
                        else:
                            selected = parse_event_selection(args.events, len(events))
                        selected_pos = {event_no: i for i, event_no in enumerate(selected)}
                        if current_event_no in selected_pos:
                            pos = selected_pos[current_event_no]
                        elif selected:
                            pos = min(pos, len(selected) - 1)
                        else:
                            break
                        continue

                    jump_to = navigation.get("jump_to")
                    if jump_to is not None:
                        # Jumps use file-local event numbers. Honor the current
                        # --events selection; an unselected/nonexistent target
                        # leaves the browser on the current event.
                        if jump_to in selected_pos:
                            pos = selected_pos[jump_to]
                            continue
                        print(
                            f"Event {jump_to} is not in the current selection "
                            f"(valid: {selected[0]}..{selected[-1]} or explicit --events set)."
                        )
                        continue

                    advance_by = navigation.get("advance_by")
                    if advance_by is not None:
                        pos += int(advance_by)
                        continue

                    break

    except (OSError, ValueError, KeyError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
