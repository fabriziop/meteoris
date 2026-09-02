# Meteoris configuration reference

This document describes the `meteoris.toml` configuration file used by
Meteoris 0.1.0.

Meteoris reads configuration values from TOML sections and then applies any
command-line overrides. The precedence is:

```text
compiled defaults < meteoris.toml < command-line options
```

If no explicit `--config` option is supplied, Meteoris looks for
`./meteoris.toml` in the current working directory. Unknown TOML keys are
treated as configuration errors rather than silently ignored.

The examples and values shown below correspond to the current reference
`config/meteoris.toml`. They are example operating values, not necessarily the
only valid settings.

---

## Overall signal-processing chain

The principal signal path is:

```text
SoapySDR native CS8 I/Q
        |
        v
complex NCO frequency translation
        |
        v
FIR decimator stage 1
        |
        v
FIR decimator stage 2
        |
        v
Welch PSD
        |
        v
symmetric-band detector
        |
        v
triggered HDF5 recording
```

For the reference configuration:

```text
input sample rate          10,000,000 samples/s
stage-1 decimation        /8
stage-1 output rate        1,250,000 samples/s
stage-2 decimation        /5
final output rate            250,000 samples/s
PSD observation bandwidth    100,000 Hz
```

The total decimation is:

```text
8 x 5 = 40
```

and therefore:

```text
10,000,000 / 40 = 250,000 samples/s
```

---

# `[sdr]` — SDR receiver configuration

This section controls the SoapySDR receiver and its raw input stream.

Reference configuration:

```toml
[sdr]
driver = "hackrf-htime"
sample_rate = 10000000
center_frequency = 142151200
gain = 110
```

## `driver`

```toml
driver = "hackrf-htime"
```

**Type:** string

SoapySDR driver name used to locate and open the receiver.

Meteoris calls SoapySDR using this driver identifier, so the value must match
an installed SoapySDR device driver.

Examples could include a custom HackRF driver such as:

```toml
driver = "hackrf-htime"
```

or the Meteoris simulator:

```toml
driver = "meteoris_sim"
```

The available devices can normally be inspected with `SoapySDRUtil`.

## `sample_rate`

```toml
sample_rate = 10000000
```

**Type:** floating-point/integer number  
**Unit:** samples per second (Hz)

Requested SDR input sample rate.

The reference DSP design is optimized and benchmarked around:

```text
10 Msps
```

This value participates directly in the final output sample rate:

```text
output_rate =
    sample_rate /
    (decimation_stage1 * decimation_stage2)
```

The value must be greater than zero.

Changing the sample rate also changes:

- NCO normalized frequency;
- FIR design;
- output sample rate;
- PSD frame rate;
- frequency-bin spacing;
- CPU load.

The selected SDR/driver must support the requested sample rate.

## `center_frequency`

```toml
center_frequency = 142151200
```

**Type:** floating-point/integer number  
**Unit:** Hz

Requested SDR RF tuning frequency.

A value of:

```toml
center_frequency = 0
```

means that Meteoris does not change the receiver frequency and leaves the
device's existing tuning unchanged.

The frequency later selected by the NCO is determined jointly by
`center_frequency` and `dsp.shift_hz`.

For the normal complex-baseband convention, a received RF signal at:

```text
f_signal
```

appears before the NCO at approximately:

```text
f_signal - center_frequency
```

and after the NCO at:

```text
f_signal - center_frequency + shift_hz
```

For example:

```text
center_frequency = 142.1512 MHz
shift_hz         = -1.1000 MHz

desired RF frequency =
142.1512 MHz + 1.1000 MHz
= 143.2512 MHz
```

would be translated close to 0 Hz in the final complex baseband.

Actual frequency orientation ultimately depends on the SDR driver's I/Q
convention; verify a new receiver/driver using a known RF test signal.

## `gain`

```toml
gain = 110
```

**Type:** floating-point/integer number  
**Unit:** dB, interpreted by the SoapySDR driver

Initial receiver gain.

When AGC is disabled, this remains the requested operating gain.

When AGC is enabled, this value is the **starting gain**, after which Meteoris
adjusts gain through the SoapySDR API.

Valid gain values and interpretation depend on the selected SDR driver.

---

# `[simulator]` — synthetic-event source

This section is used only when:

```toml
[sdr]
driver = "meteoris_sim"
```

It configures the Soapy simulator waveform. Real SDR drivers ignore these
parameters. The simulator repeats one finite stationary event and one finite
chirp inside `period_s`.

```toml
[simulator]
period_s = 8.0
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

`stationary_offset_hz` and the chirp offsets are relative to the simulator's
+1 MHz translated center; with the standard `shift_hz = -1000000` they appear
at the same offsets in the final PSD. Stationary rise/hold/fall durations are
independent. The chirp is linear in frequency versus time. All event times must
fit inside `period_s`. Amplitudes are approximately native CS8-count amplitudes
at receiver gain 20 dB and scale with the simulated gain setting.

# `[dsp]` — frequency translation and decimation

Reference configuration:

```toml
[dsp]
shift_hz = -1100000
decimation_stage1 = 8
decimation_stage2 = 5
bandwidth_hz = 100000
attenuation_db = 60
```

## `shift_hz`

```toml
shift_hz = -1100000
```

**Type:** floating-point/integer number  
**Unit:** Hz

Complex digital frequency shift performed by the NCO.

Meteoris multiplies the incoming complex samples by a complex oscillator. A
negative value shifts positive-frequency input components downward; a positive
value shifts them upward.

If a desired signal is approximately `+1.1 MHz` from the SDR tuning frequency,
then:

```toml
shift_hz = -1100000
```

moves that signal to approximately DC.

The NCO implementation uses an efficient periodic lookup table when the shift
and sample-rate ratio allows a short repeating oscillator sequence.

## `decimation_stage1`

```toml
decimation_stage1 = 8
```

**Type:** positive integer

First FIR decimation factor.

With a 10 Msps input:

```text
10,000,000 / 8 = 1,250,000 samples/s
```

This is the computationally expensive FIR stage and is implemented as an
optimized block/polyphase-equivalent kernel with SIMD support.

The value must be greater than zero.

## `decimation_stage2`

```toml
decimation_stage2 = 5
```

**Type:** positive integer

Second FIR decimation factor.

With the reference first-stage rate:

```text
1,250,000 / 5 = 250,000 samples/s
```

The total decimation factor is:

```text
decimation_stage1 * decimation_stage2
```

For the reference configuration:

```text
8 * 5 = 40
```

The value must be greater than zero.

## `bandwidth_hz`

```toml
bandwidth_hz = 100000
```

**Type:** positive number  
**Unit:** Hz

Width of the final PSD observation band.

For:

```toml
bandwidth_hz = 100000
```

the stored/displayed frequency span is approximately:

```text
-50 kHz ... +50 kHz
```

around the translated center frequency.

The configured bandwidth must be **smaller than the final output sample rate**.

With the reference settings:

```text
final sample rate = 250 kHz
bandwidth          = 100 kHz
```

which satisfies that requirement.

This is the useful signal band; the final complex sample stream itself still
runs at the decimated sample rate.

## `attenuation_db`

```toml
attenuation_db = 60
```

**Type:** positive number  
**Unit:** dB

Requested **out-of-band attenuation** of the FIR decimation filters.

A larger value generally gives better rejection of signals that could alias
into the final band, but requires more FIR taps and therefore more CPU work.

Typical interpretation:

```text
higher attenuation_db
    -> more taps
    -> better alias rejection
    -> more computation
```

The reference value of `60 dB` is a compromise between rejection and
real-time CPU load.

---

# `[psd]` — power spectral density calculation

Reference configuration:

```toml
[psd]
fft_size = 4096
```

## `fft_size`

```toml
fft_size = 4096
```

**Type:** positive integer, power of two  
**Unit:** FFT samples

Number of complex samples in each FFT used for the Welch PSD.

Meteoris requires this value to be a power of two.

The PSD currently uses 50% overlap, so with:

```text
final sample rate = 250,000 samples/s
FFT size          = 4096
hop size           = 2048 samples
```

the approximate PSD-frame rate is:

```text
250000 / 2048 ~= 122.07 PSD/s
```

and the time between consecutive PSD frames is approximately:

```text
2048 / 250000 ~= 8.192 ms
```

FFT-bin spacing is:

```text
250000 / 4096 ~= 61.0 Hz/bin
```

Increasing `fft_size` gives finer frequency resolution but:

- increases FFT computation;
- increases latency;
- decreases the number of new PSD frames per second for a fixed overlap;
- increases the amount of PSD data stored per event.

---

# `[runtime]` — process and streaming behavior

Reference configuration:

```toml
[runtime]
daemon = false
run_seconds = 0
stream_buffers = 16
direct_buffer = true
fir1_threads = 1
```

## `daemon`

```toml
daemon = false
```

**Type:** boolean

Controls whether Meteoris detaches from the terminal and runs as a daemon.

```toml
daemon = false
```

runs in the foreground.

```toml
daemon = true
```

requests daemon/background operation.

The command-line options:

```text
--daemon
--no-daemon
```

override the TOML setting.

When daemonized, operational messages are written to the configured text log file. Syslog is not used.

## `run_seconds`

```toml
run_seconds = 0
```

**Type:** non-negative number  
**Unit:** seconds

Optional automatic run-time limit.

```toml
run_seconds = 0
```

means:

```text
run continuously until shutdown is requested
```

A positive value causes automatic termination after approximately that many
seconds of input-stream duration.

Negative values are invalid.

For production meteor monitoring, `0` is normally appropriate.

## `stream_buffers`

```toml
stream_buffers = 16
```

**Type:** positive integer

Number of streaming buffers requested/configured for the SDR path.

More buffers can give additional tolerance to scheduling jitter, but increase
buffered latency and memory use.

The value must be at least 1.

This is mainly a real-time robustness parameter rather than a DSP parameter.

## `direct_buffer`

```toml
direct_buffer = true
```

**Type:** boolean

Requests SoapySDR direct-buffer access.

When supported, Meteoris can consume native `CS8` samples directly from the
driver's streaming buffers, reducing unnecessary memory copies.

```toml
direct_buffer = true
```

is preferred for maximum streaming efficiency.

If the selected SoapySDR driver does not support the direct-buffer API,
Meteoris can use its conventional stream-read fallback path.

## `fir1_threads`

```toml
fir1_threads = 1
```

**Type:** integer  
**Allowed values:** `1` or `2`

Controls parallel execution of the first, most expensive FIR decimator.

### `1`

Uses the block FIR1 SIMD kernel on a single processing thread.

This is the safest default and can be fastest on CPUs where SIMD execution is
already efficient and thread synchronization would add more overhead than it
saves.

### `2`

Splits FIR1 output work between:

- the main DSP thread;
- one persistent worker thread.

This can help on slower multi-core processors when FIR1 is CPU-bound.

It should be benchmarked on the target system rather than assumed to be
faster.

The code supports SIMD implementations such as:

- AVX2 on suitable x86 CPUs;
- NEON on suitable ARM CPUs;
- scalar/compiler-vectorized fallback otherwise.

---

# `[logging]` — runtime logging

Meteoris uses `spdlog` for runtime messages.

The default line format is:

```text
<unix-seconds.ms> [level] message
```

Reference configuration:

```toml
[logging]
level = "info"
color = true
file_enabled = true
file = "meteoris.log"
file_truncate = false
daily_rotation = true
daily_rotate_time = "00:00"
```

## `level`

Minimum emitted logging level:

```text
trace
debug
info
warn
error
critical
off
```

Detector periodic diagnostics use `debug`, so select `debug` when tuning the
detector.

## `color`

Enables level colors on the foreground terminal sink. The text-file sink is
always plain text and contains no ANSI color escape sequences.

The default terminal colors are approximately:

```text
trace/debug  white
info         green
warn         yellow
error        red
critical     bold red
```

This parameter has no visible effect in daemon mode because daemon mode has no
terminal sink.

## `file_enabled`

Enables the text-file logging sink. It defaults to `true` in both foreground
and daemon mode.

When `false`, foreground mode still has its terminal sink. Daemon mode requires
a file sink, so disabling it while daemonized leaves no useful logging
destination and is rejected.

## `file`

Text log path. Default:

```toml
file = "meteoris.log"
```

A relative path is interpreted relative to the working directory from which
Meteoris was launched. Daemonization intentionally preserves that working
directory.

## `file_truncate`

Controls startup behavior for the current logical-day file.

```toml
file_truncate = false
```

appends if that day's log already exists.

```toml
file_truncate = true
```

truncates the current logical-day file when Meteoris starts. Subsequent daily
rotations during the same run create/append the next day's file normally.

## `daily_rotation`

```toml
daily_rotation = true
```

Enables UTC daily text-log rotation.

When enabled, the configured base filename is converted to a dated filename.
For example:

```text
meteoris.log
```

becomes:

```text
meteoris_20260829.log
```

The date is the same **logical UTC day** concept used by the HDF5 output
rotation.

When disabled, Meteoris writes directly to the configured `file` path without
adding a date.

## `daily_rotate_time`

```toml
daily_rotate_time = "00:00"
```

UTC daily boundary in `HH:MM` format.

For example:

```toml
daily_rotate_time = "06:00"
```

means that:

```text
2026-08-29 05:59 UTC -> logical day 20260828
2026-08-29 06:00 UTC -> logical day 20260829
```

Unlike HDF5 event rotation, text-log rotation does not wait for a detector
event to finish. It rotates on the first log message belonging to the new
logical day.

## Foreground and daemon destinations

Foreground:

```text
colored terminal
+
plain text log file
```

Daemon:

```text
plain text log file only
```

Meteoris does **not** use syslog.

CLI overrides are available:

```text
--log-level LEVEL
--log-color
--no-log-color
--log-file PATH
--log-file-enabled
--no-log-file
--log-file-truncate
--log-file-append
--log-daily-rotation
--no-log-daily-rotation
--log-daily-rotate-time HH:MM
```


# `[agc]` — clipping-based automatic gain control

Reference configuration:

```toml
[agc]
enabled = false
target_saturation_percent = 1.0
time_constant_s = 2.0
clip_level = 126
min_gain_db = -1
max_gain_db = -1
max_step_db = 1.0
```

The Meteoris AGC is deliberately low-cost. It measures clipping directly from
the native `CS8` I/Q input while that input is already being processed, so it
does not require an additional scan of the 10 Msps stream.

It controls SDR gain through SoapySDR.

## `enabled`

```toml
enabled = false
```

**Type:** boolean

Enables or disables the clipping-based AGC.

When disabled:

- `sdr.gain` remains the requested fixed receiver gain;
- clipping statistics do not cause gain changes.

When enabled:

- `sdr.gain` is the starting value;
- Meteoris periodically adjusts receiver gain.

## `target_saturation_percent`

```toml
target_saturation_percent = 1.0
```

**Type:** number  
**Unit:** percent  
**Valid range:** greater than 0 and less than or equal to 100

Target maximum fraction of complex input samples considered clipped or
near-clipped.

For example:

```toml
target_saturation_percent = 1.0
```

means that the AGC controller tries to keep the exponentially smoothed clipping
fraction around or below roughly 1%.

For weak-signal detection, a smaller value such as `0.1` may provide more
headroom against strong transients, at the cost of lower average gain.

## `time_constant_s`

```toml
time_constant_s = 2.0
```

**Type:** positive number  
**Unit:** seconds

Time constant of the exponential smoothing/control response.

A larger value:

- reacts more slowly;
- reduces gain pumping;
- gives a more stable noise floor.

A smaller value:

- reacts more quickly;
- protects against rapid clipping sooner;
- can make gain changes more noticeable.

The value must be greater than zero when AGC is enabled.

## `clip_level`

```toml
clip_level = 126
```

**Type:** integer  
**Native units:** signed CS8 magnitude  
**Valid range:** 1 to 128

I or Q values whose magnitude reaches this level are treated as saturated or
near-saturated.

Native CS8 approximately spans:

```text
-128 ... +127
```

Using `126` intentionally detects samples close to the rails rather than
waiting only for the exact maximum code.

A complex sample is considered clipped if either I or Q reaches the configured
threshold.

## `min_gain_db`

```toml
min_gain_db = -1
```

**Type:** number  
**Unit:** dB

Minimum gain the AGC may request.

A negative value means:

```text
use the minimum gain reported by the SoapySDR device
```

A non-negative value imposes an application-level lower limit.

## `max_gain_db`

```toml
max_gain_db = -1
```

**Type:** number  
**Unit:** dB

Maximum gain the AGC may request.

A negative value means:

```text
use the maximum gain reported by the SoapySDR device
```

If both `min_gain_db` and `max_gain_db` are non-negative, the minimum must not
be greater than the maximum.

## `max_step_db`

```toml
max_step_db = 1.0
```

**Type:** positive number  
**Unit:** dB per AGC update

Maximum gain correction permitted in one AGC update.

This limits sudden gain jumps and reduces pumping.

A smaller value gives smoother, slower corrections. A larger value allows the
AGC to react more aggressively.

---

# `[recording]` — recorder operating mode

```toml
[recording]
mode = "triggered"
segment_seconds = 15.0
segment_count = 0
```

## `mode`

`"triggered"` uses the selected detector to control event recording.

`"continuous"` ignores detector decisions and records every PSD. The detector
need not be enabled in continuous mode.

## `segment_seconds`

Maximum continuous-recording sequence duration in seconds. It is ignored in
triggered mode.

A continuous sequence is never deliberately split by daily HDF5 rotation.

## `segment_count`

Number of complete continuous sequences to record before exiting cleanly.

`0` means unlimited recording until another stop condition or Ctrl-C.

CLI shorthand:

```text
--record N
--record-mode triggered|continuous
--record-segment-seconds S
```

`--record N` selects continuous mode and sets `segment_count=N`.

# `[detector]` — PSD peak/track detector

The detector works on the final PSD stream. It smooths PSD power in frequency
and time, extracts local spectral peaks above the instantaneous median
background, associates those peaks into tracks, and triggers recording when at
least one track becomes active.

Reference configuration:

```toml
[detector]
enabled = true
plugin = "peak_tracker"
threads = 1
frequency_mean_bins = 5
time_mean_psds = 3
peak_threshold_db = 5.0
min_peak_separation_hz = 300
max_peaks_per_psd = 20
diagnostic_interval_s = 1.0
pre_context_s = 0.5
post_context_s = 1.0
max_event_seconds = 15
rearm_seconds = 1.0

[detector.stationary]
enabled = true
min_hz = -5000
max_hz = 5000
max_df_hz = 250
activation_time_s = 0.5
activation_fraction = 0.50
lost_s = 0.30
max_drift_hz_s = 1000

[detector.chirp]
enabled = true
min_hz = -50000
max_hz = 50000
max_df_hz = 1500
activation_time_s = 0.05
activation_fraction = 0.70
lost_s = 0.08
min_drift_hz_s = 3000
max_drift_hz_s = 150000
```

## `plugin`

Selects the compile-time registered detector implementation.

Available plugins are:

```toml
plugin = "peak_tracker"
plugin = "echoes_automatic"
```

Changing this parameter is the application-level detector replacement
mechanism. Unknown plugin names are rejected at startup.

## `threads`

Requested detector worker-thread budget:

```toml
threads = 1
```

`0` means a future detector may choose automatically.

The current `peak_tracker` implementation remains serial, but the phase-1
detector API carries both requested and hardware thread counts so future
plugins can parallelize internally without changing Meteoris event/recording
code.

See [`DETECTOR_PLUGIN_API.md`](DETECTOR_PLUGIN_API.md).
See [`ECHOES_AUTOMATIC_DETECTOR.md`](ECHOES_AUTOMATIC_DETECTOR.md) for the
Echoes-style threshold detector and its `[detector.echoes]` parameters.

## `enabled`

Enables/disables the complete detector and triggered HDF5 recorder.

## `frequency_mean_bins`

Odd positive integer. Width of the centered frequency-domain moving mean.

The mean is calculated in linear PSD power. With ~61 Hz FFT-bin spacing, a
5-bin mean covers approximately 305 Hz.

## `time_mean_psds`

Positive integer. Number of frequency-smoothed PSDs in the causal time moving
mean. The current PSD is included.

At the reference ~122 PSD/s cadence, 3 PSDs correspond to ~24.6 ms.

## `peak_threshold_db`

Minimum local-maximum level, in dB above the median of the current
frequency/time-smoothed PSD.

This is a relative adaptive threshold, not an absolute dB/Hz value.

## `min_peak_separation_hz`

Minimum spacing between retained spectral peaks. Nearby candidates are
consolidated by keeping the strongest.

## `max_peaks_per_psd`

Maximum number of consolidated peaks passed to the tracker in one PSD. The
strongest peaks are kept; excess peaks are dropped and counted.

## `diagnostic_interval_s`

Periodic detector diagnostic interval in seconds. `0` disables periodic
diagnostics.

## `pre_context_s`

Amount of PSD history saved before detector trigger ON.

## `post_context_s`

Amount of PSD data saved after the last active track is lost and trigger OFF
occurs. A new active track during this interval merges into the same event.

## `max_event_seconds`

Maximum duration of one merged recorded event. `0` disables the forced cutoff.

## `rearm_seconds`

Detector inhibit interval after a forced maximum-duration cutoff.

# `[detector.stationary]`

Parameters for near-zero, slowly drifting tracks.

## `stationary.enabled`

Enables stationary-track classification and activation.

## `stationary.min_hz`, `stationary.max_hz`

Allowed frequency interval for stationary tracks. Both values are signed
offset frequencies relative to the PSD center.

## `stationary.max_df_hz`

Maximum frequency difference used when associating the next peak with an
existing stationary track.

## `stationary.activation_time_s`

Minimum tentative-track age before a stationary track may become active.

## `stationary.activation_fraction`

Required fraction of PSD opportunities on which that track was actually
matched to a peak.

## `stationary.lost_s`

Maximum gap since the last matched peak before the stationary track is dropped.

## `stationary.max_drift_hz_s`

Maximum absolute average frequency drift used to classify an unknown track as
stationary.

# `[detector.chirp]`

Parameters for rapidly moving tracks.

## `chirp.enabled`

Enables chirp-track classification and activation.

## `chirp.min_hz`, `chirp.max_hz`

Allowed chirp search interval. The reference values cover the whole 100 kHz
observation band.

## `chirp.max_df_hz`

Maximum frequency error around the predicted chirp frequency used for
track/peak association.

## `chirp.activation_time_s`

Minimum tentative-track age before a chirp may become active.

## `chirp.activation_fraction`

Required hit/PSD-opportunity fraction before chirp activation.

## `chirp.lost_s`

Maximum gap since the last matched chirp peak before the track is dropped.

## `chirp.min_drift_hz_s`, `chirp.max_drift_hz_s`

Absolute average frequency-velocity interval used to classify an unknown track
as a chirp.

See [`DETECTOR.md`](DETECTOR.md) for the full processing and tracking
algorithm.

# `[output]` — HDF5 recording, rotation, and storage protection

Reference configuration:

```toml
[output]
directory = "./data"
file_prefix = "meteoris"
hdf5_compression = 1
swmr = true
swmr_flush_seconds = 1.0
daily_rotate_time = "00:00"
max_growth_mb_per_min = 100
min_free_space_gb = 2
```

## `directory`

```toml
directory = "./data"
```

**Type:** string

Directory in which HDF5 files are stored.

Meteoris creates the directory hierarchy automatically when needed.

The current compiled default is also:

```text
./data
```

Relative paths are interpreted relative to the process's current working
directory.

For daemon installations, an absolute path may be preferable so the storage
location does not depend on the launch directory.

## `file_prefix`

```toml
file_prefix = "meteoris"
```

**Type:** string

Prefix used to construct daily HDF5 filenames.

The general form is:

```text
<directory>/<file_prefix>_YYYYMMDD.h5
```

For example:

```text
./data/meteoris_20260826.h5
```

The date corresponds to the configured logical daily rotation interval.

## `hdf5_compression`

```toml
hdf5_compression = 1
```

**Type:** integer  
**Valid range:** 0 through 9

HDF5/gzip compression level applied to the PSD dataset.

```text
0 = compression disabled
1 = low-CPU compression
...
9 = highest gzip compression effort
```

For a real-time recorder, level `1` is a good low-CPU default.

Higher values may save some disk space but increase CPU load and write latency.

## `swmr`

```toml
swmr = true
```

**Type:** boolean

Enables HDF5 **Single Writer / Multiple Reader (SWMR)** mode.

With SWMR enabled, `meteoris_plot` can safely open the active daily HDF5 file
while Meteoris is still recording it.

This avoids the need to make a potentially inconsistent copy of an open HDF5
file.

New SWMR files use the HDF5 latest-format bounds required by SWMR.

For normal operation this should remain enabled.

## `swmr_flush_seconds`

```toml
swmr_flush_seconds = 1.0
```

**Type:** non-negative number  
**Unit:** seconds

Maximum interval between publication of dirty HDF5 data to SWMR readers.

With:

```toml
swmr_flush_seconds = 1.0
```

a live reader typically sees newly recorded data within about one second.

A value of:

```toml
swmr_flush_seconds = 0
```

flushes after every saved PSD row.

That provides the smallest reader latency but can cause substantially more
filesystem I/O.

Meteoris only needs to publish when recorded data are dirty, so idle detector
periods do not continuously write HDF5 data.

## `daily_rotate_time`

```toml
daily_rotate_time = "00:00"
```

**Type:** string  
**Format:** `HH:MM`  
**Time zone:** UTC  
**Valid range:** `00:00` through `23:59`

Defines the logical daily HDF5 file boundary.

At this UTC time, Meteoris closes the old daily file and starts the new
logical-day file.

Example:

```toml
daily_rotate_time = "06:00"
```

means the logical day changes at 06:00 UTC instead of midnight.

### Event-safe delayed rotation

Meteoris never splits an event merely because the daily boundary is reached.

If an event is active at the configured time:

```text
daily boundary reached
        |
        v
keep previous file open
        |
        v
finish active event
        |
        v
finish post-trigger context
        |
        v
rotate immediately
```

Thus an event spanning 05:59:55 to 06:00:08 with a `06:00` boundary remains
entirely in the previous file.

## `max_growth_mb_per_min`

```toml
max_growth_mb_per_min = 100
```

**Type:** non-negative number  
**Unit:** MB/minute

Storage safety limit for sustained HDF5 file growth.

If the current HDF5 file grows faster than this configured rate, Meteoris
performs a controlled shutdown.

The rate is measured over a longer window rather than reacting to individual
HDF5 allocation bursts.

A value of:

```toml
max_growth_mb_per_min = 0
```

disables this safety check.

This protects against situations such as:

- persistent false triggering;
- unexpectedly high event duty cycle;
- configuration errors producing excessive recording volume.

## `min_free_space_gb`

```toml
min_free_space_gb = 2
```

**Type:** non-negative number  
**Unit:** GiB/GB-scale filesystem free-space threshold

Minimum permitted available space on the filesystem containing the output
directory.

If available space drops below this threshold, Meteoris stops in a controlled
manner rather than continuing until the filesystem is full.

A value of:

```toml
min_free_space_gb = 0
```

disables the free-space check.

---

# Parameter interaction examples

## Example 1 — resulting sample rates

For:

```toml
[sdr]
sample_rate = 10000000

[dsp]
decimation_stage1 = 8
decimation_stage2 = 5
```

the rates are:

```text
input                  10.000 Msps
after stage 1           1.250 Msps
after stage 2           0.250 Msps
```

## Example 2 — PSD timing and detector time smoothing

With a 250 ksps final complex rate, 4096-point FFT, and 50% overlap:

```text
FFT hop           2048 samples
frame interval    8.192 ms
frame rate        ~122.07 PSD/s
frequency bins    ~61.0 Hz apart
```

Therefore:

```toml
time_mean_psds = 3
```

averages about 24.6 ms of recent PSD history.

## Example 3 — frequency smoothing

At ~61 Hz/bin:

```toml
frequency_mean_bins = 5
```

corresponds to approximately 305 Hz of frequency smoothing.

## Example 4 — stationary versus chirp activation

A central peak that remains near one frequency can become stationary after:

```toml
activation_time_s = 0.5
activation_fraction = 0.50
```

while a fast-moving track can activate much sooner:

```toml
[detector.chirp]
activation_time_s = 0.05
activation_fraction = 0.70
```

The chirp must nevertheless remain consistently associated with predicted
frequency positions.

## Example 5 — frequency translation

For:

```toml
center_frequency = 142151200
shift_hz = -1100000
```

a signal near 143251200 Hz is translated close to 0 Hz before final PSD
processing.

# Current reference configuration

```toml
# Meteoris configuration.
# Command-line options override values in this file.

[sdr]
driver = "hackrf-htime"
sample_rate = 10000000
# Set to 0 to leave frequency unchanged by Meteoris.
center_frequency = 142151200
# Initial gain. When AGC is enabled this is the starting value.
gain = 110

[dsp]
shift_hz = -1100000
decimation_stage1 = 8
decimation_stage2 = 5
bandwidth_hz = 100000
# Out-of-band attenuation of the FIR decimation filters, in dB.
attenuation_db = 60

[psd]
fft_size = 4096

[runtime]
# Detach from the terminal and run in the background.
# Command-line --daemon / --no-daemon overrides this setting.
daemon = false

# 0 = run continuously until SIGINT/SIGTERM.
# A positive value stops automatically after this many seconds.
run_seconds = 0

# Number of SoapySDR streaming buffers.
stream_buffers = 16

# Use SoapySDR direct-buffer access when supported.
direct_buffer = true

# FIR1 block-kernel worker count.
# 1 uses SIMD on one core and is the safest default.
# 2 splits FIR1 outputs across the caller plus one persistent worker thread.
# Benchmark both values on the target CPU; two threads help only when FIR1 is CPU-bound.
fir1_threads = 1

[logging]
# Runtime logging level: trace, debug, info, warn, error, critical, or off.
# Detector periodic diagnostics are DEBUG messages.
level = "info"
# ANSI terminal colors by level in foreground mode.
color = true
# Plain text logging is enabled by default in foreground and daemon mode.
file_enabled = true
file = "meteoris.log"
file_truncate = false

[agc]
# Low-cost AGC based on clipping of native CS8 ADC samples. The clipping count
# is collected during the existing 10 Msps DSP pass, so no extra input scan is done.
enabled = false
# Keep the exponentially-smoothed fraction of clipped complex samples near/below this value.
target_saturation_percent = 1.0
# Response/smoothing time constant. Larger values reduce gain pumping.
time_constant_s = 2.0
# Treat I or Q magnitudes at/above this native CS8 level as saturated/near-saturated.
clip_level = 126
# -1 uses the gain range reported by the Soapy driver.
min_gain_db = -1
max_gain_db = -1
# Maximum gain change per AGC update.
max_step_db = 1.0

[detector]
# Shared PSD preprocessing. Means are computed in LINEAR PSD power.
# Frequency mean is centered; use an odd number of bins.
frequency_mean_bins = 5
# Causal time mean: current PSD plus previous PSDs.
time_mean_psds = 3

# A spectral local maximum becomes a candidate when the doubly-smoothed PSD is
# this many dB above the current whole-band MEDIAN background.
peak_threshold_db = 5.0

# Nearby local maxima are consolidated by keeping the strongest peak.
min_peak_separation_hz = 300

# Safety limit after consolidation. Strongest peaks are retained; excess peaks
# are dropped and a rate-limited warning is emitted.
max_peaks_per_psd = 20

# 0 disables periodic detector diagnostics.
diagnostic_interval_s = 1.0

# Common recording context around the transition of active-track count 0 <-> >0.
pre_context_s = 0.5
post_context_s = 1.0

# Maximum event duration; 0 disables forced cutoff.
max_event_seconds = 15
# Trigger inhibit after a forced cutoff.
rearm_seconds = 1.0

[detector.stationary]
enabled = true
# Only tracks inside this near-zero region can become stationary detections.
min_hz = -5000
max_hz = 5000
# Maximum frequency error for association to the last stationary position.
max_df_hz = 250
# Tentative track must reach this age and occupancy before becoming active.
activation_time_s = 0.5
activation_fraction = 0.50
# Active/tentative stationary track survives gaps up to this time.
lost_s = 0.30
# Average |df/dt| at or below this value classifies an unknown track stationary.
max_drift_hz_s = 1000

[detector.chirp]
enabled = true
# Chirps may occur across the complete observation band.
min_hz = -50000
max_hz = 50000
# Matching window around the predicted frequency.
max_df_hz = 1500
activation_time_s = 0.05
activation_fraction = 0.70
# Chirp tracks are allowed only short gaps because prediction uncertainty grows fast.
lost_s = 0.08
# Average drift range used to classify a generic tentative track as a chirp.
min_drift_hz_s = 3000
max_drift_hz_s = 150000

[output]
# One HDF5 file is created/appended per UTC day.
directory = "./data"
file_prefix = "meteoris"
# 0 disables gzip; 1 is a good low-CPU default for a real-time recorder.
hdf5_compression = 1

# HDF5 Single-Writer/Multiple-Reader mode. Keep enabled to allow
# meteoris_plot to open the active daily file while Meteoris is recording.
swmr = true

# Publish newly written rows to SWMR readers at this interval.
# 0 flushes after every saved PSD; 1 second is a low-overhead default.
swmr_flush_seconds = 1.0

# UTC time when a new logical daily HDF5 file should begin. Format: HH:MM.
# If an event is active at the boundary, rotation is delayed until the complete
# event, including post-trigger context, has been saved.
daily_rotate_time = "00:00"

# Stop Meteoris if the HDF5 file grows faster than this sustained rate.
# The rate is checked over 10-second windows to ignore short HDF5 allocation bursts.
# 0 disables the growth-rate safety check.
max_growth_mb_per_min = 100

# Stop Meteoris when available space on the output filesystem falls below this value.
# 0 disables the free-space safety check.
min_free_space_gb = 2
```

# Recommended tuning workflow

1. Set SDR tuning, gain, sample rate and DSP shift first.
2. Verify frequency placement with a known RF signal.
3. Start with `frequency_mean_bins = 5` and `time_mean_psds = 3`.
4. Observe diagnostics and tune `peak_threshold_db` so ordinary blurred noise
   produces a manageable number of peaks.
5. Set `min_peak_separation_hz` near the effective frequency-smoothing width.
6. Tune stationary `max_df_hz`, `activation_time_s`, `activation_fraction` and
   `lost_s` using representative near-zero echoes.
7. Tune chirp drift limits, `max_df_hz`, activation and loss time using
   representative moving signals.
8. Keep `max_peaks_per_psd` as a safety bound rather than using it as the main
   noise-rejection mechanism.
9. Tune pre/post context and event-duration protection after track behavior is
   satisfactory.
10. Keep SWMR and disk-growth/free-space protections enabled for unattended
    operation.

For performance-sensitive targets such as Raspberry Pi, benchmark the complete
application on the target hardware; the peak/track detector itself is expected
to be much cheaper than the high-rate NCO/FIR pipeline.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
