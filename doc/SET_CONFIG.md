## Essential parameters

### The SDR Section Parameters

Set the proper SoapySDR driver for the SDR that is connected to the computer.
The driver identifier strings are in the Soapy documentation.

Set the appropriate sample rate. Meteoris is designed to operate with input
sample rates up to 10 MHz. Lower sample rates are possible; check compatibility
with your SDR and the configured DSP chain.

Set the center frequency, i.e. the SDR tuning frequency. This parameter depends
on the transmitter used for meteor-scatter reception. Some examples of
transmitters used for this purpose are:

- GRAVES 143.050 MHz (Dijon, France)
- BRAMS   49.970 MHz (Dourbes, Belgium)
- GB3MBA  50.408 MHz (Sutton-in-Ashfield, UK)

Set the SDR gain. In general, a relatively high value can be used initially
because meteor-scatter signals are quite weak. However, excessive gain can
overload the receiver. If a strong recorded signal shows symmetric sidebands at
approximately ±1.22 kHz from the carrier, reduce the gain until the sidebands
disappear. 

The following is an example configuration for a HackRF One SDR at a 10 MHz
sample rate using the GRAVES transmitter:

```
[sdr]
driver = "hackrf"
sample_rate = 10000000
center_frequency = 143050000
gain = 80
```


### Center Frequency May Be Tricky

Many SDRs, especially low-cost devices, do not completely suppress the DC
component (frequency = 0 Hz) caused by small leakage from the receiver's local
oscillator. As a result, a strong spur may appear at the center of the received
band and hide signals close to zero frequency. This problem can be avoided by
using a tuning offset so that the unwanted DC component is moved away from the
frequency region of interest.

A second tuning issue affects SDRs that are not synchronized to an external
reference clock, such as a GPS-disciplined reference. Low-cost crystal or
ceramic oscillators can have frequency errors that drift with temperature,
sometimes by 1 kHz or more at the received RF frequency.

These problems can be handled with the **`shift_hz`** parameter in the
**`[dsp]`** section. This parameter digitally translates the received complex
baseband and can therefore compensate for the intentional tuning offset and
receiver frequency error.

For example, to receive the GRAVES transmitter at 143.050 MHz, move the DC
spur approximately 1 MHz up away from the signal and compensate for a receiver
frequency error of about -1.35 kHz, the center frequency is raised by 1 MHz and
the shift frequency is set to the sum of the raised quantity (+1000000) and
the -1.35 kHz compensation (+998650 Hz). See the configuration below.

``` 
[sdr]
...
center_frequency = 144050000
...

[dsp]
...
shift_hz = +998650
...
```

### The DSP Section Parameters

This section defines the parameters of the Meteoris digital signal-processing
chain. Apart from the **`shift_hz`** parameter discussed above, the supplied
configuration provides suitable defaults for the other DSP parameters.

### The Detector Section Parameters

Meteoris provides compile-time detector plugins selected with
`detector.plugin`.

The default `peak_tracker` detector detects **spectral peaks and their motion
through time**.

Each PSD is first averaged in linear power across a small centered frequency
window and then across a short causal time window. Local maxima are extracted
when they rise by `peak_threshold_db` above the median of the blurred PSD.
Nearby maxima are consolidated into one peak.

The resulting peaks are associated from PSD to PSD into tentative tracks.
Tracks can become:

- **stationary**, for slowly moving peaks in a configurable region near zero
  frequency;
- **chirp**, for rapidly moving peaks across the observation band.

A track becomes active only after satisfying both a minimum age and a minimum
fraction of successful peak observations. Short missing intervals are
tolerated through `lost_s`.

Recording starts when the number of active tracks changes from zero to one or
more, and trigger OFF occurs when the last active track is lost. Existing
pre/post context, HDF5 recording, event merging, daily rotation and graceful
shutdown behavior remain common to both signal types. `max_event_seconds` bounds
a merged peak-tracker event; if that duration is reached, the complete event is
discarded from the HDF5 datasets and the detector rearms after `rearm_seconds`.

A useful starting configuration is:

```toml
[detector]
plugin = "peak_tracker"
frequency_mean_bins = 5
time_mean_psds = 3
peak_threshold_db = 5.0
min_peak_separation_hz = 300
max_peaks_per_psd = 20
max_event_seconds = 15
rearm_seconds = 1

[detector.stationary]
min_hz = -5000
max_hz = 5000
max_df_hz = 250
activation_time_s = 0.5
activation_fraction = 0.50
lost_s = 0.30

[detector.chirp]
min_hz = -50000
max_hz = 50000
max_df_hz = 1500
activation_time_s = 0.05
activation_fraction = 0.70
lost_s = 0.08
min_drift_hz_s = 3000
max_drift_hz_s = 150000
```

The `echoes_automatic` detector is also available. It adapts the automatic
capture logic used by Echoes to Meteoris's headless PSD recorder. Each PSD is
scanned inside a configurable frequency interval to measure:

- **S**: maximum PSD density in dB/Hz;
- **N**: mean PSD density in dB/Hz;
- **S-N**: peak excess above the mean noise level.

It can trigger with absolute thresholds on S, differential thresholds on S-N,
or automatic thresholds learned from the idle S-N baseline. It also supports a
delayed trigger and a join interval for merging short interruptions into one
event. Select it with:

```toml
[detector]
plugin = "echoes_automatic"
pre_context_s = 0.5
post_context_s = 1.0

[detector.echoes]
threshold_mode = "automatic"
detection_center_hz = 0
detection_width_hz = 0
automatic_lower_offset_db = 4
automatic_upper_delta_db = 3
automatic_warmup_s = 5
automatic_baseline_time_constant_s = 30
automatic_stddev_window_s = 1
automatic_end_stddev_factor = 2
delay_before_trigger_s = 0
join_events_closer_than_s = 1
```

The detailed detector algorithms and all tuning parameters are documented in
[`doc/DETECTOR.md`](doc/DETECTOR.md),
[`doc/ECHOES_AUTOMATIC_DETECTOR.md`](doc/ECHOES_AUTOMATIC_DETECTOR.md), and
[`doc/CONFIG_FILE_REFERENCE.md`](doc/CONFIG_FILE_REFERENCE.md).

### The Output Section Parameters

By default, Meteoris saves data in the **`data`** subdirectory. Files use the
**`meteoris`** prefix followed by the daily date and the **`.h5`** suffix. If
these defaults are suitable, no changes are required.

### Parameters in All Other Sections

For a detailed description of every configuration section and parameter, see
the [Configuration File Reference](./doc/CONFIG_FILE_REFERENCE.md).

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
