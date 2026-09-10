Here is a concise list of the main **Meteoris 0.5.0** features as developed so far:

* **Real-time SDR acquisition**

  * SoapySDR interface.
  * Native `CS8` I/Q input.
  * Direct-buffer access when supported.
  * Designed and benchmarked for continuous **10 Msps** input.
  * Configurable sample rate, center frequency, gain and streaming buffers.

* **Optimized DSP chain**

  * Complex NCO frequency translation.
  * Optimized block/polyphase first FIR decimator.
  * SIMD acceleration: **AVX2 on x86** and **NEON on ARM** when available.
  * Optional 1/2-thread FIR1 processing.
  * Two-stage configurable decimation, typically **8 × 5 = 40:1**.
  * Typical conversion:
    `10 Msps → 1.25 Msps → 250 ksps`.
  * Configurable FIR out-of-band attenuation.
  * Final selectable **100 kHz observation band**.

* **Spectrum processing**

  * Welch PSD calculation.
  * Configurable FFT size, typically 4096 points.
  * 50% FFT overlap.
  * PSD density expressed in dB/Hz.
  * Integrated band-power and peak measurements.

* **PSD peak/track meteor detector**

  * Centered moving mean across frequency bins, performed in linear PSD power.
  * Causal rolling mean across consecutive PSDs.
  * Adaptive whole-band median background for each blurred PSD.
  * Local spectral peaks selected by dB excess above the current background.
  * Sub-bin parabolic frequency interpolation.
  * Minimum-frequency-separation consolidation of nearby peaks.
  * Configurable maximum retained peaks per PSD with rate-limited warnings.
  * Generic one-to-one peak/track association.
  * Tentative tracks classified as near-zero stationary signals or fast chirps.
  * Stationary tracks use a narrow frequency association window.
  * Chirps use frequency-velocity prediction and a wider matching window.
  * Track activation requires both minimum age and minimum occupancy.
  * Configurable dropout/loss time for each signal class.
  * Recording starts when at least one track is active and stops when the last
    active track is lost.
  * Existing pre/post context, retrigger merging, maximum event duration and
    forced-cutoff rearm remain common to all tracks.

* **Low-overhead AGC**

  * Uses native CS8 ADC clipping rather than PSD values.
  * Configurable clipping target.
  * Configurable response time constant.
  * Gain limits and maximum adjustment step.
  * Receiver gain is stored with every recorded PSD.

* **HDF5 event recording**

  * Only interesting triggered PSD intervals are stored.
  * PSD, timestamps, detector state, event ID and gain are recorded.
  * Embedded original/effective TOML configuration.
  * Acquisition start time, SDR/DSP parameters, hostname and other metadata.
  * HDF5 format identifier: **`meteoris v0`**.
  * Author metadata: **Fabrizio Pollastri**.
  * Software version metadata.

* **Live HDF5 access**

  * HDF5 **SWMR (Single Writer Multiple Reader)** support.
  * `meteoris_plot` can inspect today's HDF5 file while Meteoris is still recording.
  * Configurable SWMR flush interval.
  * No need to copy the active HDF5 file before inspecting it.

* **Daily file management**

  * One HDF5 file per logical day.
  * Configurable UTC rotation time (`HH:MM`).
  * File rotation is delayed if an event crosses the boundary.
  * The complete event, including post-trigger context, remains in the previous file.

* **Storage protection**

  * Monitors HDF5 file growth in MB/min.
  * Configurable maximum growth rate.
  * Monitors available filesystem space.
  * Configurable minimum free-space threshold.
  * Controlled shutdown when storage safety limits are exceeded.

* **Graceful operation**

  * Continuous operation until `SIGINT`/`SIGTERM`.
  * First Ctrl-C waits for the current event and its post-trigger context to finish.
  * Second Ctrl-C forces immediate termination.
  * HDF5 is flushed and closed cleanly.
  * Optional daemon operation.
  * Plain text-file logging by default in both foreground and daemon mode; no syslog.

* **TOML configuration**

  * Separate sections for `[sdr]`, `[dsp]`, `[psd]`, `[runtime]`, `[agc]`, `[detector]`, and `[output]`.
  * CLI options override TOML.
  * Sensible built-in defaults when `meteoris.toml` is absent.
  * Detector enabled by default.
  * Default output directory is `./data`, created automatically.

* **Performance monitoring**

  * DSP CPU budget.
  * DSP block execution times.
  * p50/p95/p99/p99.9 latency statistics.
  * Deadline misses.
  * SDR timeouts and overflows.
  * FIR1 versus post-FIR1 processing breakdown.
  * Real-time sustainability reporting.

* **`meteoris_plot` analysis tool**

  * Reads both closed and active SWMR HDF5 recordings.
  * Displays metadata and embedded TOML.
  * Lists detected events.
  * Diagnostic event columns for total, pre-trigger, active and post-trigger PSD counts.
  * Event completeness/status indication.
  * Gqrx-like waterfall color map.
  * Time on X axis, frequency on Y axis, PSD density by color.
  * Interactive minimum/maximum color-scale sliders.
  * Toggleable X/Y grid.
  * Toggleable high-visibility trigger markers.
  * Displays **all ON/OFF/retrigger transitions**.
  * One event displayed at a time.
  * `Space` advances to the next event.
  * `number + Space` jumps directly to an event.
  * `R` refreshes an active SWMR recording.
  * PNG export.

* **SoapyMeteorisSim**

  * Synthetic SoapySDR receiver for end-to-end testing.
  * Native CS8 generation.
  * Direct-buffer interface.
  * Generates finite near-zero stationary echoes plus separate meteor-like chirped events.
  * Real-time pacing.
  * Allows testing DSP, detector, HDF5 recording and plotting without an actual receiver.

* **Build/install infrastructure**

  * CMake-based build.
  * Out-of-source `build/` directory.
  * Convenience `build.sh`.
  * Local-user or system-wide installation.
  * Consistent repository organization for application, simulator, tools and configuration.
  * `meteoris --version` and `meteoris_plot --version`.
  * Current software version: **0.5.0**.



* **Configurable finite-event SDR simulator**

  * Finite near-zero stationary echo with configurable rise, hold and fade.
  * Separate clean linear chirp with configurable start/end offsets and duration.
  * Independent stationary/chirp amplitudes and event timing.
  * No permanent carrier outside the stationary event.
  * Configurable broadband synthetic-noise amplitude.
  * Moderate amplitudes with wide CS8 headroom.
  * Stochastic CS8 quantization turns quantization error into noise instead of
    coherent carrier/chirp intermodulation replicas.


## Structured runtime logging

Meteoris uses `spdlog` for runtime output with Unix-epoch millisecond timestamps
and severity levels. Foreground output supports automatic per-level ANSI
colors; daemon mode uses the text-file sink only. Logging level, file and color behavior are
configurable from TOML or CLI, and detector tuning diagnostics are classified
as `debug` messages.

* **Daily UTC text-log rotation** with configurable `HH:MM` boundary and dated filenames.

* **Recoverable SDR overflow handling**: `SOAPY_SDR_OVERFLOW` is logged at error level and counted, but does not terminate acquisition.


* **Acquisition/DSP block diagnostics**
  * Logs the SoapySDR stream MTU at startup.
  * Logs the first actual sample count returned by the SDR and passed directly
    to `processCs8()` as the effective acquisition/DSP block size.
  * Final statistics report first/minimum/maximum block sample counts. This
    distinguishes the driver's advertised MTU from the actual block cadence.

* **Low-risk execution optimizations**
  * PSD output storage is reused across Welch frames instead of allocating a
    new `powerDensity` vector for every PSD.
  * PSD normalization scale is precomputed once.
  * Band-bin power calculation avoids `std::norm` temporary overhead.
  * FIR2 input scaling avoids copying and mutating each FIR1 complex sample.
  * FIR2 caches its tap count in the hot `push()` path.
  * Zero-frequency NCO translation has a direct CS8-to-float fast path while
    preserving clipping/AGC accounting.


---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
