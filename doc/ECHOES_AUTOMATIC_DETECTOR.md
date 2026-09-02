# Echoes-style automatic detector

The `echoes_automatic` detector adapts the automatic-capture behavior described
by [Echoes](https://www.gabb.it/echoes/051_automatic.html) to Meteoris's
headless PSD/event recorder.

Select it with:

```toml
[detector]
plugin = "echoes_automatic"
```

## Scan measurements

Within the configured detection interval, every PSD frame produces:

- **S**: maximum PSD density, expressed in dB/Hz;
- **N**: mean linear PSD density converted to dB/Hz;
- **S-N**: their difference in dB;
- peak frequency: the frequency of S.

Echoes describes absolute thresholds in uncalibrated dBFS. Meteoris receives
calibrated/scaled PSD density instead, so this implementation expresses
absolute thresholds in **dB/Hz**. Differential and automatic offsets remain dB
ratios and transfer directly.

## Threshold modes

### Absolute

The event begins when S reaches `absolute_upper_db_hz` and reaches its falling
edge when S falls below `absolute_lower_db_hz`.

### Differential

The same hysteresis is applied to S-N using `differential_upper_db` and
`differential_lower_db`. This follows changes in absolute receiver noise better
than absolute mode.

### Automatic

While idle, an exponential moving average learns the mean S-N difference. No
events are emitted during `automatic_warmup_s`.

```text
lower = mean(S-N) + automatic_lower_offset_db
upper = lower + automatic_upper_delta_db
```

The two thresholds freeze at the initial upper crossing and remain frozen
through the event and its join interval. This prevents a strong echo from
raising its own baseline.

Echoes also requires signal oscillation to settle before ending an automatic
event. The public description does not define the exact statistic, so Meteoris
uses an explicit reproducible interpretation:

1. compute the temporal standard deviation of S over
   `automatic_stddev_window_s`;
2. learn its idle exponential average;
3. freeze `automatic_end_stddev_factor` times that average at event start;
4. require both S-N below the frozen lower threshold and current deviation
   below the frozen limit.

## Detection interval

`detection_center_hz` and `detection_width_hz` are offsets within the final PSD
band. A width of zero selects the whole PSD. Restricting this interval can
reject interference but can also exclude Doppler-shifted head echoes.

## Delayed trigger

`delay_before_trigger_s` delays the Active transition after an upper crossing.
Set `detector.pre_context_s` to at least the same duration so the initial peak
is retained when the buffered event is written. Unlike the Echoes GUI control,
which is a percentage of the displayed waterfall, Meteoris uses seconds because
it has no fixed screenshot height.

## Joining interrupted events

After the lower/end conditions are met, the detector remains Active until they
have remained satisfied for `join_events_closer_than_s`. A signal recovery in
that interval cancels the pending end, joining rotating-beam illumination gaps
into one event.

`detector.max_event_seconds` remains the recorder-level protection against a
persistent signal, and `detector.rearm_seconds` applies after such a forced
cutoff.

## Performance and diagnostics

The detector is synchronous and allocation-free on ordinary frames after its
small rolling history reaches capacity. Named metrics are materialized only at
`detector.diagnostic_interval_s`. `PreprocessOnly` continues measurement and
automatic-baseline learning during recorder rearm without activating an event.
