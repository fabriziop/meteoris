# Simulator

Typical defaults are:

```toml
[simulator]
period_s = 8.0
cycle_count = 3
amplitude_reduction = 1.0
noise_amplitude = 5.0

stationary_enabled = true
stationary_start_s = 0.8
stationary_rise_s = 0.35
stationary_hold_s = 1.0
stationary_fall_s = 0.65
stationary_offset_hz = 0
stationary_amplitude = 3.0

chirp_enabled = true
chirp_start_s = 4.0
chirp_duration_s = 1.2
chirp_start_offset_hz = 38000
chirp_end_offset_hz = 12000
chirp_amplitude = 6.0
```

`cycle_count` controls how many complete TOML-defined periods contain signals.
Cycle 1 uses `stationary_amplitude` and `chirp_amplitude` unchanged. For cycle
number `n` (starting at 1), each effective signal amplitude is:

```text
max(0, configured_amplitude - (n - 1) * amplitude_reduction)
```

The same absolute reduction is applied independently to the stationary echo
and chirp. After `cycle_count` cycles the simulator continues streaming
background noise, but generates no more signals. Set `cycle_count = 0` for the
previous unlimited-repeat behavior. `amplitude_reduction` must be non-negative;
it does not change `noise_amplitude`.

With the normal `shift_hz = -1000000`, the stationary offset and chirp offsets
appear directly on the final Meteoris PSD frequency axis. Stochastic CS8
rounding is retained to suppress coherent quantization/intermodulation replicas.

The simulator is built as a SoapySDR plugin. During development, run it with:

```bash
./run_sim.sh
```

This forces SoapySDR to use the freshly built plugin from this repository, so
an older installed `meteoris_sim` module cannot silently mask simulator source
changes. Normal `install.sh --local` / `--system` now installs the simulator
plugin too; use `--no-sim` to exclude it.

Select it with:

```toml
[sdr]
driver = "meteoris_sim"
```

At startup the plugin logs `Meteoris simulator finite-cycles-3` together with
the effective cycle count, amplitude reduction, event timing, and amplitudes.
If that line is absent, the process is not using this simulator build.

The simulated meteor-scatter sequence contains one stationary-frequency event
at `f = 0` and one chirp event with a rapidly decreasing Doppler frequency,
representing a meteor undergoing strong deceleration.

# References and Internals

[Configuration File Reference](./doc/CONFIG_FILE_REFERENCE.md)

[Detailed Features](./doc/DETAILED_FEATURES.md)

[Detection Algorithm](./doc/DETECTOR.md)
Note: the detector algorithm is still under development and may require
further tuning to reduce false triggers caused by noise.

[Echoes-style Automatic Detector](./doc/ECHOES_AUTOMATIC_DETECTOR.md)

[DSP Pipeline](./doc/DSP_PIPELINE.md)

[HDF5 Recording Format](./doc/RECORDING_FORMAT.md)

[Recover HDF5 Output Files](./doc/RECOVER_HDF5.md)

[BUG20260902: one-PSD spectrogram dropout after trigger ON](./doc/BUG20260902.md)

**Live browser/network architecture:** see [NETWORK_WEB.md](doc/NETWORK_WEB.md).

### Notes

Meteoris currently requires native `CS8` input from the selected SoapySDR
device/driver. The main HackRF acquisition path uses direct buffers when
available.

The Release build enables `-ffast-math` and `-fno-math-errno` for the DSP
executable. `-march=native` is enabled by default and should be disabled for
cross-machine binary distribution.

When available, the optional FFTW backend is recommended for FFT processing.
On the tested x86-64 system, FFTW makes the FFT kernel approximately 3x faster
than the embedded radix-2 implementation. With the current 4096-point PSD
pipeline, this corresponds to roughly a 35--40% reduction in post-FIR1
processing time and an 8--10% reduction in overall DSP processing time. The
end-to-end gain is smaller because FIR/NCO, decimation, windowing, PSD
calculation, detector processing, and I/O are unaffected by the FFT backend.

### Tentative-track diagnostics

When no track is active but tentative candidates exist, the `DET` diagnostic
line now includes one representative `tentative{...}` record with class,
frequency, velocity, excess, age, occupancy, hit count and missed time. This
is useful for tuning activation parameters against real stationary echoes and
chirps.

Close-peak consolidation is treated as normal detector behavior. A rate-limited
warning is emitted only when `max_peaks_per_psd` is actually exceeded and peaks
must be dropped.


### Finite stationary and chirp simulator events

The `meteoris_sim` source generates a finite near-zero rise/hold/fade echo and
a separate clean linear chirp instead of a permanent carrier. Both event
amplitudes and timing are independently configurable. CS8 conversion continues
to use stochastic rounding and generous headroom to prevent coherent 2x/3x
intermodulation chirps.


### Daily text-log rotation

Text logging is rotated by UTC logical day by default:

```toml
[logging]
file = "meteoris.log"
daily_rotation = true
daily_rotate_time = "00:00"
```

With rotation enabled, `meteoris.log` becomes files such as
`meteoris_20260829.log`. The `HH:MM` boundary follows the same UTC logical-day
convention as `output.daily_rotate_time`. Log rotation is independent of HDF5
events and occurs on the first log message after the configured boundary.


### SDR overflow handling

SoapySDR stream overflows (`SOAPY_SDR_OVERFLOW`) are treated as recoverable
acquisition errors. Meteoris logs an `error` message, increments the overflow
counter, discards the affected read, and immediately continues acquisition.
Other negative SoapySDR stream errors remain fatal.


### Detector plugin architecture

Detector algorithms are now separated from `meteoris.cpp` behind a versioned
synchronous `IDetector` interface. The detector is selected with:

```toml
[detector]
plugin = "peak_tracker"
threads = 1
```

Available detector plugins are `peak_tracker`, `echoes_automatic`, and
`meteor_logger_3f`. Their implementations live in separate files under
`src/detector/`. Structured metrics and detector objects cross the interface
for logging and future HDF5/plot/replay tools. See
[`doc/DETECTOR_PLUGIN_API.md`](doc/DETECTOR_PLUGIN_API.md).


### Recording modes

Meteoris has two application-level PSD recording modes:

```toml
[recording]
mode = "triggered"
segment_seconds = 15.0
segment_count = 0
```

`triggered` uses the configured detector and preserves the normal pre/post
context and event semantics.

`continuous` ignores detector state and writes every PSD. Continuous data is
divided into bounded sequences of `segment_seconds`. Each sequence receives a
new HDF5 `event_id`; `detector_state` remains neutral (`0`). `segment_count=0`
records indefinitely, while a positive count stops cleanly after that many
complete segments.

The convenience CLI:

```text
--record N
```

selects continuous mode and records `N` segments (`N=0` means unlimited).
`--record-segment-seconds S` overrides segment length.

Daily HDF5 rotation occurs only between continuous segments, so a segment that
crosses the configured daily boundary remains whole in the previous file.


### Logging

Meteoris runtime output is handled by `spdlog`. The default log format is:

```text
<unix-seconds.ms> [level] message
```

Example:

```text
1787983200.137 [info] detector=ON peak_tracker ...
1787983201.044 [debug] DET ready=YES event=IDLE bg=-70.53dB/Hz peaks=0 ...
```

Configure logging with:

```toml
[logging]
level = "info"
color = true
```

Foreground levels are colored when supported: info is green, warnings are
yellow, errors are red, debug/trace messages are white, and critical messages
are bold red. Detector periodic diagnostics
are emitted at `debug`, so use `level = "debug"` while tuning the detector.
File logging is enabled by default in both foreground and daemon mode.
Foreground mode also logs to the terminal; daemon mode logs to the text file
only. Syslog is not used. The file log never contains ANSI colors. `info` and
higher-severity records are flushed to the file immediately, so daemon startup
and state-change messages are visible without waiting for a warning, shutdown,
or an output-buffer fill. Periodic detector diagnostics remain `debug` messages;
use `level = "debug"` when those recurring details are wanted. In triggered
recording, each retained event also emits an `event_recorded` DEBUG line. If a
`peak_tracker` event reaches `max_event_seconds`, it emits `event_discarded`
instead; the final INFO summary reports both `recorded_events` and
`discarded_events`.

CLI overrides include `--log-level`, `--log-color`, and `--no-log-color`.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
