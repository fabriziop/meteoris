# Meteor Logger 3f detector

The `meteor_logger_3f` plugin adapts Wolfgang Kaufmann's frequency-signature
method from *New radio meteor detecting and logging software*, WGN 45:4
(2017), 67-72:

<http://www.ars-electromagnetica.de/robs/Media/WGN45-4_MeteorLogger_add.pdf>

The original software analyzed overlapping audio FFTs. Meteoris already
produces a calibrated final PSD stream, so this plugin applies the published
detection principle to those PSD frames rather than adding another audio FFT.

## Selecting the plugin

```toml
[detector]
plugin = "meteor_logger_3f"
pre_context_s = 0.5
post_context_s = 1.0

[detector.meteor_logger]
detection_min_hz = -49000
detection_max_hz = 49000
cluster_width_hz = 250
sequence_frames = 6
allowed_gaps = 1
max_drift_hz_s = 11000
release_s = 0.15
auto_notch_enabled = true
auto_notch_after_s = 10
auto_notch_width_hz = 200
max_auto_notches = 4
```

The normal `pre_context_s = 0.5` retains the frames consumed by temporal
confirmation at the standard Meteoris PSD cadence.

## Processing chain

For every PSD, the detector:

1. restricts analysis to `detection_min_hz .. detection_max_hz`;
2. excludes any learned interference notches;
3. selects the three bins with greatest spectral power;
4. accepts a candidate when their total frequency span is no greater than
   `cluster_width_hz`;
5. checks candidates over `sequence_frames` PSDs, allowing at most
   `allowed_gaps` missing candidates;
6. requires successive peak frequencies to stay within
   `max_drift_hz_s * elapsed_time`;
7. activates the normal Meteoris event recorder after the sequence qualifies.

There is deliberately no absolute or background-relative amplitude threshold.
The signature is concentration of the three strongest frequencies followed by
frequency continuity in time. A mathematically flat PSD is explicitly rejected
because it has no spectral signature.

## Sensitive and robust modes

The paper describes two empirical sequence lengths:

```toml
# Sensitive
sequence_frames = 5
allowed_gaps = 1

# Robust
sequence_frames = 6
allowed_gaps = 1
```

The defaults use robust mode. The paper reports about 1.5 and 0.7 false
registrations per hour respectively for its 2.5 kHz audio setup and white-noise
test. Those rates do not transfer directly to a different PSD bandwidth,
resolution, receiver, or interference environment.

The paper's 117 Hz frame-to-frame limit corresponded to roughly 11 kHz/s at
its 10.7 ms update interval. Meteoris expresses this as `max_drift_hz_s` and
multiplies it by the actual elapsed time, so the rule remains meaningful when
FFT size, decimation, or frame cadence changes.

## Persistent-line auto-notch

A line broad enough to make the three strongest frequencies cluster can hold a
signature detector active and obscure weaker echoes. With auto-notch enabled,
a clustered peak that remains within half of `auto_notch_width_hz` for
`auto_notch_after_s` is learned as interference. Up to `max_auto_notches`
persistent lines are excluded from later top-three selection.

Learned notches survive detector recorder-rearm resets but are cleared when
Meteoris restarts. Disable this adaptation when intentionally observing echoes
that can remain at one frequency longer than `auto_notch_after_s`:

```toml
auto_notch_enabled = false
```

## Configuration reference

| Parameter | Meaning |
|---|---|
| `detection_min_hz`, `detection_max_hz` | Signed PSD-offset interval searched by the detector. |
| `cluster_width_hz` | Maximum span from the lowest to highest of the three strongest frequencies. |
| `sequence_frames` | Sliding confirmation length; use 5 for sensitive or 6 for robust operation. |
| `allowed_gaps` | Maximum non-clustered frames in one confirmation sequence. |
| `max_drift_hz_s` | Maximum change of peak frequency per second. |
| `release_s` | Time without a compatible peak before trigger OFF. |
| `auto_notch_enabled` | Enables persistent-line learning. |
| `auto_notch_after_s` | Required persistence before a line is learned. |
| `auto_notch_width_hz` | Width excluded around each learned line. |
| `max_auto_notches` | Maximum learned lines retained until restart. |

## Applicability

This method assumes a continuous-wave transmitter. As the paper notes, an AM
carrier may work when it dominates its sidebands, while FM and multifrequency
or digital transmissions do not satisfy the single spectral-ridge assumption.
Tune `cluster_width_hz` for the final PSD bin spacing and the expected signal
width, and validate settings against local noise and interference recordings.
