# Meteoris raw-IQ recording v1

The output sample rate remains **250,000 complex samples/s**. The receiver's
last FIR decimator now supplies unwindowed complex float32 samples to the
recording and live-stream paths. The existing PSD and detector calculation
still runs inside the receiver for real-time detection and diagnostics.

## HDF5

`/metadata/format` is `meteoris v1`. The new `/iq/samples` dataset has
shape `(frames, fft_size, 2)`, float32 little-endian, with last-axis order
`[real, imaginary]`. The complex samples are normalized by 128, as in the
receiver's DSP pipeline. Each frame is a complete **unwindowed FFT input
window**, and adjacent frames overlap by 50%. Therefore, this is raw I/Q,
not a contiguous, deduplicated sample stream: overlapping samples are stored
twice. The first window begins at acquisition sample zero; successive windows
advance by `fft_size / 2` samples. `frame_index` and `timestamp_ns` refer to
the window's PSD frame index and center timestamp respectively.

Per-frame metadata is in `/iq/{timestamp_ns,frame_index,event_id,
detector_state,detector_max_db_hz,gain_db}`. `/iq/frequency_hz` contains the
frequency coordinates for reproducing the former band-limited PSD.
Metadata includes `output_rate_hz`, `psd_bandwidth_hz`, `fft_size`,
`acquisition_start_ns`, and `iq_layout`. Event recording/selection is still
triggered by the in-process detector, so not all acquisition windows need be
saved. To reconstruct a PSD, apply a periodic-symmetric Hann window
(`numpy.hanning`), complex FFT, and divide squared magnitude by
`output_rate_hz * sum(window**2)`; select bins within the saved bandwidth.

The old PSD format `meteoris v0` is documented in `RECORDING_FORMAT.md` and
can be viewed using `meteoris_plot_legacy.py`. Old files must not be appended
to as v1 files. Start a new recording directory or filename prefix when
migrating.

## Plotter and web bridge

`meteoris_plot.py` converts v1 IQ to a temporary PSD-compatible view in
bounded chunks, then delegates to the established plotting UI. The temporary
view is a snapshot; refresh within the UI does **not** reread new IQ windows.
The temporary view also requires disk space. The old plotter is preserved as
`meteoris_plot_legacy.py`.

The DSP TCP stream sends `MIQ1` version 2 packets with the existing 68-byte
header layout, and a float32 interleaved IQ payload of `fft_size * 8` bytes.
`meteoris_web.py` receives IQ, computes the Hann-windowed PSD with NumPy, and
forwards the existing `MPSD` version 1 packets to the browser. The browser
protocol and UI are unchanged. The gateway now requires NumPy in addition to
aiohttp. Each transmitted IQ frame is a complete overlapping FFT window.
