# Meteoris detector plugin interface — phase 1

Meteoris phase 1 separates detector algorithms from acquisition, event
recording, HDF5, logging, and plotting.

The current implementation is a **compile-time plugin architecture**. Detector
implementations are normal C++ source files linked into `meteoris` and selected
at runtime through TOML:

```toml
[detector]
enabled = true
plugin = "peak_tracker"
threads = 1
```

Phase 1 deliberately does **not** use `dlopen()` or an external C++ shared
library ABI. This keeps deployment simple while establishing the boundary
needed for future detector replacement.

## Source layout

```text
src/
  meteoris.cpp
  detector/
    detector.hpp
    detector_registry.cpp
    peak_tracker_detector.cpp
    echoes_automatic_detector.cpp
```

`meteoris.cpp` no longer contains detector-specific configuration structs, defaults,
parsing, or validation. It retains only common detector selection and recorder/event
settings.

## Core interface

`detector.hpp` defines:

```text
IDetector
Detector Frame
Detector Result
Detector DebugView
Detector Metric
Detector Object
Detector Environment
Detector Info
```

The detector API version is currently:

```text
DETECTOR_API_VERSION = 3
```

## Frame ownership

`DetectorFrame.psd` is a borrowed pointer to the already-computed Meteoris PSD.
No PSD copy is made at the interface.

The memory is valid only during:

```cpp
detector->process(frame, options)
```

A detector must not retain the pointer after `process()` returns.

## Synchronous execution

Phase 1 uses:

```cpp
DetectorResult process(const DetectorFrame &frame,
                       const ProcessOptions &options)
```

The call is synchronous.

`ProcessOptions.mode` selects either `ProcessingMode::Full` or
`ProcessingMode::PreprocessOnly`. Meteoris uses `PreprocessOnly` during rearm
and forced cutoff: detector preprocessing and smoothing history advance, while
association and tracking are suppressed. This lets the first post-rearm frame
use current smoothing history without inheriting an earlier track.

A future detector may internally use worker threads/SIMD, but all work for the
frame must finish before `process()` returns.

`DetectorEnvironment` already provides:

```text
requestedThreads
hardwareThreads
```

so multicore implementations can be added without changing the Meteoris main
loop.

## Detector state

Plugins return one of:

```text
WarmingUp
Idle
Active
```

Meteoris, not the plugin, owns recording/event semantics.

The plugin does not know about:

```text
event IDs
pre-trigger context
post-trigger context
HDF5
daily rotation
maximum event duration
forced-cutoff rearm
disk-space protection
```

Meteoris starts/stops recording from transitions in detector `Active` state.

## Structured diagnostics

Plugins return borrowed structured diagnostic views only when the caller sets
`ProcessOptions.collectDebug=true`. Meteoris requests them at
`detector.diagnostic_interval_s`; ordinary frames return the lightweight
decision and peak counters without constructing metric or object vectors.

Scalar values are exported as `DetectorMetric`:

```text
name
value
unit
```

The current `peak_tracker` exports metrics including:

```text
background_db_hz
raw_candidates
close_suppressed
dropped_by_max_peaks
retained_peaks
track_count
active_tracks
stationary_active
chirp_active
strongest_peak_frequency_hz
strongest_peak_level_db_hz
strongest_peak_excess_db
```

Tracked physical candidates are exported as `DetectorObject`:

```text
id
type
active
frequency_hz
frequency_rate_hz_s
strength_db_hz
excess_db
age_s
occupancy
hits
opportunities
missed_s
```

The object types currently standardized are:

```text
Unknown
Stationary
Chirp
```

The debug arrays are owned by the plugin and remain valid until its next
`process()` or `reset()` call.

This zero-copy arrangement is intended for multiple consumers:

```text
plugin
  |
  +--> spdlog diagnostics
  +--> future HDF5 detector diagnostics
  +--> meteoris_plot overlays
  +--> future offline detector replay/comparison tools
```

## Configuration schema

Detector-specific TOML values are stored by Meteoris as raw scalar strings in
`detector::Config`. The core parses TOML structure, but it does not interpret the
types or semantics of plugin-owned keys.

Each compile-time detector implementation owns:

- its private typed configuration struct;
- a `ConfigField` schema containing relative TOML key, default TOML value, and
  description;
- conversion from the generic `Config` view into its private configuration;
- semantic and PSD-geometry validation;
- optional generic recorder requirements, currently `minPreContextSeconds`.

`meteoris.cpp` owns only common detector keys such as `enabled`, `plugin`, `threads`,
`diagnostic_interval_s`, `pre_context_s`, `post_context_s`, `max_event_seconds`, and
`rearm_seconds`. Plugin schemas are also used to render the effective/default TOML,
so adding a detector does not require adding its settings to `meteoris.cpp`.

Configuration files may contain settings for multiple compiled-in detectors. The
registry accepts keys declared by any compiled-in schema and rejects unknown detector
keys, preserving typo detection without coupling the core to a concrete plugin.

## Registry/factory

`detector_registry.cpp` contains the compile-time registry.

Currently the registered implementations are:

```text
peak_tracker
echoes_automatic
```

Adding a second detector requires:

1. implement `IDetector` in a separate source file;
2. add a factory function;
3. register its string name in `detector_registry.cpp`;
4. add the source to CMake;
5. select it with `detector.plugin`.

No acquisition, event-recorder, HDF5, or plotting code should need detector
algorithm changes.

## Threading provision

`[detector].threads` is carried through `DetectorEnvironment`.

```toml
threads = 1
```

requests one detector thread.

```toml
threads = 0
```

means that a future plugin may choose automatically.

The present `peak_tracker` reports:

```text
mayUseMultipleThreads = false
```

because its cost is currently small. This provision exists so a heavier future
detector can tile frequency work or use persistent worker threads while keeping
the same synchronous API.

## Performance measurement

Meteoris measures wall-clock time around every `IDetector::process()` call.

Periodic debug diagnostics include:

```text
process_us=...
```

and the final detector summary includes the last measured detector processing
time.

This permits direct comparison of detector implementations against the PSD
frame period.

## Future phase 2

Once this interface has proven stable, an external dynamically loaded detector
can be added through a small C ABI adapter.

The phase-1 C++ interface should not itself be exposed directly as a binary
plugin ABI because independent C++ shared libraries may depend on compiler,
standard-library, and build ABI details.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
