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
    - goes back one event with SHIFT+SPACE/SHIFT+ENTER;
    - jumps to a typed event number when SPACE/ENTER is pressed;
    - refreshes a live SWMR file with the R key;
    - skips by -20/-10/+10/+20 events with Z/X/C/V;
    - selects an HDF5 save file with N using a file chooser;
    - saves the currently displayed event data to that file with W;
    - terminate the interactive browser with Q or by closing the window;
  - provides interactive PSD color-scale minimum/maximum sliders;
  - shows the 0..1 kHz positive-frequency PSD spectrum at the time of the Max-PSD peak;
  - provides controls for full/half PSD bandwidth, max/median-PSD traces, X/Y grid, and trigger markers;
  - marks trigger ON/OFF positions;
  - can save selected event plots as PNG files.

Old PSD v0 files remain supported by meteoris_plot_legacy.py.  The temporary
PSD view is generated in chunks and discarded when the plotter exits.

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

__author__ = "Fabrizio Pollastri <mxgbot@gmail.com>"
import importlib.machinery
import importlib.util
import sys
from pathlib import Path

import tempfile

import h5py
import numpy as np

def _load_legacy_module():
    """Load the sibling legacy plot module/script from this installation."""
    here = Path(__file__).resolve().parent
    candidates = (
        here / "meteoris_plot_legacy",
        here / "meteoris_plot_legacy.py",
    )
    for candidate in candidates:
        if not candidate.exists():
            continue
        if candidate.suffix == ".py":
            spec = importlib.util.spec_from_file_location(
                "meteoris_plot_legacy", candidate
            )
        else:
            loader = importlib.machinery.SourceFileLoader(
                "meteoris_plot_legacy", str(candidate)
            )
            spec = importlib.util.spec_from_loader("meteoris_plot_legacy", loader)
        if spec is None or spec.loader is None:
            continue
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            spec.loader.exec_module(module)
        except Exception:
            sys.modules.pop(spec.name, None)
            raise
        return module
    raise SystemExit(
        "Cannot load meteoris_plot_legacy beside meteoris_plot. "
        "Expected one of: meteoris_plot_legacy.py or meteoris_plot_legacy."
    )


legacy = _load_legacy_module()
__version__ = legacy.__version__
# Retain plotting API for existing integrations that import from meteoris_plot.
for _name, _value in legacy.__dict__.items():
    if _name.startswith("_"):
        continue
    globals().setdefault(_name, _value)


def convert_iq_to_psd(source: Path, target: Path, block_size: int = 64) -> None:
    with h5py.File(source, 'r', libver='latest', swmr=True) as src, h5py.File(target, 'w') as dst:
        if '/iq/samples' not in src:
            raise ValueError('Not a Meteoris IQ v1 recording')
        src.copy('/metadata', dst)
        iq = src['/iq']
        out = dst.create_group('psd')
        hop = int(iq['samples'].shape[1])
        packed = 'segment_prefix' in iq and 'prefix_index' in iq
        nfft = hop * 2 if packed else hop
        if iq['samples'].shape[2] != 2:
            raise ValueError('IQ data must have real and imaginary channels')
        fs = float(src['/metadata'].attrs['output_rate_hz'])
        if fs <= 0:
            raise ValueError('invalid output sample rate')
        freqs = np.fft.fftfreq(nfft, 1 / fs)
        selected = np.flatnonzero(np.abs(freqs) <= float(src['/metadata'].attrs['psd_bandwidth_hz']) / 2)
        selected = selected[np.argsort(freqs[selected])]
        out.create_dataset('frequency_hz', data=freqs[selected])
        for name in ('timestamp_ns', 'frame_index', 'event_id', 'detector_state',
                     'detector_max_db_hz', 'gain_db'):
            if name in iq:
                iq.copy(name, out)
        rows = min([int(iq['samples'].shape[0])] + [int(out[name].shape[0]) for name in out if name not in ('frequency_hz',)])
        power = out.create_dataset('power_density', (rows, len(selected)), dtype='f4',
                                   chunks=(min(max(1, rows), 64), len(selected)))
        window = np.hanning(nfft).astype(np.float64)
        scale = 1.0 / (fs * np.sum(window * window))
        for start in range(0, rows, block_size):
            stop = min(rows, start + block_size)
            samples = np.asarray(iq['samples'][start:stop], dtype=np.float64)
            if packed:
                indices = np.asarray(iq['prefix_index'][start:stop], dtype=np.int64)
                frame_ids = np.asarray(iq['frame_index'][start:stop], dtype=np.int64)
                previous = np.empty_like(samples)
                for j, row in enumerate(range(start, stop)):
                    if row > 0 and frame_ids[j] == int(iq['frame_index'][row - 1]) + 1:
                        previous[j] = iq['samples'][row - 1]
                    else:
                        previous[j] = iq['segment_prefix'][indices[j]]
                samples = np.concatenate((previous, samples), axis=1)
            complex_samples = samples[..., 0] + 1j * samples[..., 1]
            fft = np.fft.fft(complex_samples * window, axis=1)
            power[start:stop] = (np.abs(fft[:, selected]) ** 2 * scale).astype('f4')


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    args = legacy.build_arg_parser().parse_args(argv)
    if not args.file.exists():
        return legacy.main(argv)
    with h5py.File(args.file, 'r', libver='latest', swmr=True) as src:
        if '/iq/samples' not in src:
            raise SystemExit('Old PSD files require meteoris_plot_legacy.py')
    with tempfile.TemporaryDirectory(prefix='meteoris_psd_view_') as tmp:
        converted = Path(tmp) / args.file.name
        convert_iq_to_psd(args.file, converted)
        argv[argv.index(str(args.file))] = str(converted)
        return legacy.main(argv)


if __name__ == '__main__':
    raise SystemExit(main())
