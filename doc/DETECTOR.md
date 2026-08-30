> **Architecture note:** the algorithm described here is now the built-in
> `peak_tracker` detector plugin. The plugin boundary, structured diagnostics,
> and future threading provisions are documented in
> [`DETECTOR_PLUGIN_API.md`](DETECTOR_PLUGIN_API.md).
>
# Meteoris PSD peak/track detector

This document describes the first implementation of the PSD peak/track
detector used by Meteoris.

The detector works only on the final Welch PSD stream. It does not inspect raw
I/Q samples.

Its processing chain is:

```text
PSD in linear power density
        |
        v
centered frequency moving mean
        |
        v
causal time moving mean
        |
        v
whole-band median background
        |
        v
local spectral maxima above background
        |
        v
minimum-separation consolidation
        |
        v
bounded list of SpectralPeak objects
        |
        v
generic multi-target PeakTracker
        |
        +--> stationary classification
        |
        +--> chirp classification
        |
        v
tentative -> active tracks
        |
        v
active-track count > 0
        |
        v
existing Meteoris event recorder
```

The design intentionally separates four concepts:

1. PSD smoothing;
2. spectral peak extraction;
3. peak-to-track association;
4. event recording.

The HDF5 event recorder is not specific to stationary or chirped tracks.

---

## 1. Input PSD cadence

With the common Meteoris configuration:

```text
final complex rate = 250,000 sample/s
FFT size           = 4096
FFT hop             = 2048 samples
```

the PSD interval is approximately:

```text
2048 / 250000 = 8.192 ms
```

or about:

```text
122.07 PSD/s
```

The detector derives this interval from the actual output rate and FFT size.

---

## 2. Frequency smoothing

For every PSD, Meteoris first applies a centered square mean along frequency.

For an odd window of `Nf` bins:

```text
frequency_mean[k] =
    mean(PSD[k-r ... k+r])

r = (Nf-1)/2
```

The operation is performed in **linear PSD power**, not dB.

The implementation uses a prefix sum, so its cost is O(number of PSD bins)
rather than O(number of bins x averaging width).

At PSD edges, the averaging window is truncated to the available bins.

Configuration:

```toml
frequency_mean_bins = 5
```

The value must be an odd positive integer.

At approximately 61 Hz/bin:

```text
3 bins ~= 183 Hz
5 bins ~= 305 Hz
7 bins ~= 427 Hz
9 bins ~= 549 Hz
```

---

## 3. Time smoothing

The frequency-smoothed PSD is then averaged causally along time:

```text
time_mean_t[k] =
    mean(freq_mean_t[k],
         freq_mean_(t-1)[k],
         ...
         freq_mean_(t-Nt+1)[k])
```

Again, averaging is in linear power.

The implementation keeps a per-bin rolling sum, so increasing
`time_mean_psds` does not multiply the per-frame computational cost.

Configuration:

```toml
time_mean_psds = 3
```

At the standard PSD cadence:

```text
2 PSDs ~= 16.4 ms
3 PSDs ~= 24.6 ms
4 PSDs ~= 32.8 ms
5 PSDs ~= 41.0 ms
```

Peak extraction starts only after the causal time window has filled.

---

## 4. Adaptive instantaneous background

The detector does not use a fixed absolute dB/Hz trigger threshold.

For each doubly-smoothed PSD it calculates the **median** across the complete
observation band:

```text
background = median(smoothed_PSD)
```

The median is robust against a limited number of strong narrowband signals.

A peak is compared with this instantaneous background using:

```text
excess_db =
    10 log10(peak_power / background_power)
```

Configuration:

```toml
peak_threshold_db = 5.0
```

therefore means:

> a candidate local maximum must be at least 5 dB above the current blurred
> PSD median.

This follows changes in receiver noise level without a long-term adaptive
noise state.

---

## 5. Local peak extraction

A candidate spectral peak must satisfy both:

```text
power[k] > power[k-1]
power[k] >= power[k+1]
```

and:

```text
power[k] >= background * 10^(peak_threshold_db/10)
```

Thus only local maxima above the adaptive background are converted into
`SpectralPeak` objects.

Each peak contains:

```text
frequency_hz
psd_db_hz
excess_db
```

---

## 6. Sub-bin peak frequency

Meteoris refines the FFT-bin frequency using three-point parabolic
interpolation in log-power space.

The interpolated offset is limited to:

```text
-0.5 ... +0.5 FFT bin
```

This reduces frequency quantization in the tracker and helps distinguish
slowly moving tracks from bin-to-bin jitter.

---

## 7. Minimum peak separation

Frequency smoothing can produce multiple nearby local maxima belonging to one
physical spectral feature.

Meteoris therefore applies one-dimensional non-maximum suppression.

Candidates are sorted strongest first. A weaker candidate is discarded if it
is closer than:

```toml
min_peak_separation_hz = 300
```

to a stronger already-retained peak.

The number of such suppressions is counted and included in diagnostics and
rate-limited warnings.

---

## 8. Maximum peaks per PSD

After consolidation, Meteoris limits the number of retained peaks:

```toml
max_peaks_per_psd = 20
```

If more peaks survive:

- the strongest are retained;
- weaker excess peaks are dropped;
- a rate-limited warning is emitted;
- the total number of dropped peaks is included in the final detector summary.

This bounds peak-tracker work in a badly polluted RF environment.

---

## 9. Generic peak tracks

The tracker maintains one common list of `PeakTrack` objects.

A track contains approximately:

```text
track id
classification
first seen time
last seen time
first frequency
current frequency
average frequency velocity
current strength
current excess above background
number of opportunities
number of successful peak matches
active flag
```

New unassigned peaks create **tentative unknown tracks**.

They are not immediately considered detections.

---

## 10. Peak-to-track association

At every PSD, Meteoris builds all valid `(track, peak)` associations.

For each track it predicts a frequency and defines an allowed matching window.

### Unknown track

Before classification:

```text
predicted frequency = last observed frequency
```

The matching window is the largest applicable stationary/chirp `max_df_hz`.

### Stationary track

```text
predicted frequency = last observed frequency
```

with the narrow stationary matching window.

### Chirp track

```text
predicted frequency =
    last_frequency + velocity * elapsed_time
```

with the wider chirp matching window.

All valid track/peak pairs are sorted by normalized frequency error. A greedy
one-to-one assignment is then performed.

Therefore:

- one peak cannot update two tracks;
- one track cannot consume two peaks in the same PSD.

---

## 11. Track velocity

For a matched track, frequency velocity is estimated from the complete track
baseline:

```text
velocity =
    (current_frequency - first_frequency) /
    (current_time - first_time)
```

This average drift is intentionally less sensitive to one-frame FFT-bin jitter
than a derivative calculated from only two adjacent PSDs.

Classification is delayed until the track has at least three hits and enough
time history to make the drift meaningful.

---

## 12. Stationary classification

An unknown track may become stationary if:

```text
track frequency is inside [stationary.min_hz, stationary.max_hz]
```

and:

```text
abs(average velocity) <= stationary.max_drift_hz_s
```

Reference values:

```toml
[detector.stationary]
min_hz = -5000
max_hz = 5000
max_drift_hz_s = 1000
```

Once classified stationary, matching uses:

```toml
max_df_hz = 250
```

around the last observed frequency.

---

## 13. Chirp classification

An unknown track may become a chirp if:

```text
track frequency is inside [chirp.min_hz, chirp.max_hz]
```

and its average drift satisfies:

```text
chirp.min_drift_hz_s
    <= abs(velocity)
    <= chirp.max_drift_hz_s
```

Reference values:

```toml
[detector.chirp]
min_hz = -50000
max_hz = 50000
min_drift_hz_s = 3000
max_drift_hz_s = 150000
```

After classification, the next frequency is predicted using the measured
velocity.

---

## 14. Tentative-track activation

A track becomes ACTIVE only after satisfying both an age requirement and an
occupancy requirement.

Occupancy is:

```text
hits / opportunities
```

A hit means a peak was assigned to that track on that PSD opportunity.

### Stationary

Reference settings:

```toml
activation_time_s = 0.5
activation_fraction = 0.50
```

A stationary track therefore needs:

```text
age >= 0.5 s
AND
occupancy >= 50%
```

before it can trigger recording.

### Chirp

Reference settings:

```toml
activation_time_s = 0.05
activation_fraction = 0.70
```

A fast track can therefore activate much sooner, but it must be observed on a
larger fraction of the available PSDs.

---

## 15. Temporary signal loss

A track is not deleted on the first missed PSD.

Its last successful observation time is retained.

The track is deleted only when:

```text
current_time - last_seen > lost_s
```

### Stationary

```toml
lost_s = 0.30
```

allows relatively long fading/dropouts.

### Chirp

```toml
lost_s = 0.08
```

uses a shorter interval because a rapidly moving track becomes difficult to
predict after a long gap.

If the peak is found again before this limit, normal tracking continues.

---

## 16. Detector active state

The detector itself is active when:

```text
number of ACTIVE tracks > 0
```

Tentative/unknown tracks do not trigger recording.

Therefore:

```text
0 active tracks -> 1 or more active tracks
```

produces detector trigger ON.

The loss of the last ACTIVE track produces detector trigger OFF.

Multiple simultaneous tracks are naturally supported.

---

## 17. Event recording

The existing `PsdDetectorRecorder` remains responsible for:

- pre-trigger PSD context;
- active PSD recording;
- post-trigger context;
- retrigger/event merging;
- maximum event duration;
- rearm after forced cutoff;
- daily file rotation;
- HDF5 SWMR publication;
- graceful shutdown.

The detector only supplies:

```text
active / inactive
```

plus diagnostics.

Configuration:

```toml
pre_context_s = 0.5
post_context_s = 1.0
max_event_seconds = 15
rearm_seconds = 1.0
```

---

## 18. Retrigger merging

If the active-track count reaches zero, the recorder begins the configured
post-trigger tail.

If another track becomes active before that tail expires, Meteoris returns to
active recording using the **same event ID**.

Thus short separations between related detections can remain one HDF5 event.

---

## 19. Peak-density warnings

Two conditions are monitored:

1. nearby peaks suppressed by `min_peak_separation_hz`;
2. peaks dropped because `max_peaks_per_psd` was exceeded.

Close-peak consolidation is counted for statistics/diagnostics but is
considered normal operation and does **not** by itself emit a warning.

A warning is emitted only when the hard `max_peaks_per_psd` limit actually
drops peaks. Warnings are rate limited to avoid flooding the terminal or
log file.

Final detector statistics include:

```text
peak_limit_drops
close_peak_suppressions
```

---

## 20. Runtime diagnostics

`detector.diagnostic_interval_s` controls periodic output.

For example:

```toml
diagnostic_interval_s = 1.0
```

may produce:

```text
DET ready=YES event=ACTIVE bg=-73.42dB/Hz
peaks=2 raw=3 tracks=4 active_tracks=1 stationary=0 chirp=1
strongest{freq=12450Hz level=-65.8dB/Hz excess=7.6dB}
track{id=17 class=chirp freq=12450Hz velocity=-31200Hz/s
      excess=7.6dB age=0.18s occupancy=0.83 missed=0.00s}
```

The diagnostic values are intended to have direct physical meaning.

---

## 21. Computational cost

For approximately 1600 PSD bins and ~122 PSD/s:

- frequency smoothing: O(F);
- time smoothing: O(F);
- median calculation: O(F) average using `nth_element`;
- local-maximum scan: O(F);
- peak consolidation: small candidate list;
- track association: approximately O(number_of_tracks x number_of_peaks).

With a typical peak limit of 20, detector cost is expected to be small
relative to the 10 Msps NCO/FIR DSP path.

---

## 22. Current first-version limitations

This first implementation intentionally stays simple.

Not yet implemented:

- local peak-prominence criterion;
- optimal Hungarian track/peak assignment;
- widening prediction uncertainty during chirp gaps;
- HDF5 storage of individual track trajectories;
- explicit track overlays in `meteoris_plot`.

These can be added after real waterfall data establish which refinements are
actually useful.


### Tentative-track diagnostics

If there is no active track but tentative tracks exist, periodic diagnostics
print the most representative tentative candidate:

```text
tentative{id=24 class=unknown freq=-43820Hz velocity=-520Hz/s
          excess=5.4dB age=0.032s occupancy=0.50 hits=2/4 missed=0.008s}
```

The diagnostic prefers the oldest tentative track and then higher occupancy,
which makes it easier to follow a real candidate through:

```text
peak -> tentative unknown -> stationary/chirp classification -> ACTIVE
```

instead of reporting only the latest noise peak.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
