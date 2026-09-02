# Meteoris HDF5 recording format

This document describes the HDF5 recording format currently written by
**Meteoris 0.2.0**.

The format identifier stored in each new file is:

```text
meteoris v0
```

The HDF5 file contains two main groups:

```text
/
├── metadata/
└── psd/
```

The format is designed for:

- triggered PSD recording rather than continuous raw-I/Q recording;
- efficient append-only writes during detected events;
- one logical file per configured UTC day;
- live read access through HDF5 SWMR;
- reconstruction of event timing, trigger state, PSD level, and receiver gain;
- preservation of acquisition and configuration metadata.

---

# 1. What is recorded

Meteoris does **not** store the original 10 Msps raw I/Q stream.

The recorded data are selected Welch PSD frames produced after the DSP chain:

```text
native SDR CS8 I/Q
        |
        v
frequency translation
        |
        v
FIR decimation
        |
        v
final complex stream
        |
        v
Welch PSD
        |
        v
detector
        |
        +--> selected event-related PSD frames
                |
                v
              HDF5
```

A recorded event can contain:

```text
pre-trigger PSD frames
active trigger PSD frames
post-trigger PSD frames
```

Short retriggers during the post-trigger interval remain part of the same
recorded event.

---

# 2. File naming

The normal filename is constructed as:

```text
<output.directory>/<output.file_prefix>_YYYYMMDD.h5
```

For example:

```toml
[output]
directory = "./data"
file_prefix = "meteoris"
```

produces files such as:

```text
./data/meteoris_20260827.h5
```

The `YYYYMMDD` date is the **logical recording day**, not necessarily midnight
UTC if a non-zero daily rotation time is configured.

---

# 3. Logical daily boundary

The file day is controlled by:

```toml
daily_rotate_time = "HH:MM"
```

in UTC.

Internally Meteoris subtracts the configured rotation offset from the PSD
timestamp and then derives `YYYYMMDD`.

For example:

```toml
daily_rotate_time = "06:00"
```

means:

```text
2026-08-27 05:59 UTC -> logical file day 20260826
2026-08-27 06:00 UTC -> logical file day 20260827
```

## Event-safe rotation

A file boundary never intentionally splits a detector event.

If the daily boundary occurs while an event is active or while its
post-trigger context is still being written, Meteoris keeps the old file open.

The sequence is:

```text
daily rotation time reached
        |
        v
event still in progress?
        |
       yes
        |
        v
continue writing old file
        |
        v
event and post-context complete
        |
        v
close old HDF5 file
        |
        v
open new logical-day file
```

Therefore a complete event belongs to the file in which it started, even when
the event extends beyond the nominal daily boundary.

---

# 4. High-level HDF5 tree

A newly created Meteoris file has the following logical structure:

```text
/
├── metadata/
│   └── toml_config
│
└── psd/
    ├── frequency_hz
    ├── power_density
    ├── timestamp_ns
    ├── frame_index
    ├── event_id
    ├── detector_state
    ├── detector_max_db_hz
    └── gain_db
```

The `/metadata` group also carries scalar/string HDF5 attributes described
below.

---

# 5. `/metadata` group

The `/metadata` group records information that applies to the file/acquisition
rather than to one individual PSD row.

A new file stores the following attributes.

| Attribute | Type | Meaning |
|---|---|---|
| `format` | string | Meteoris recording-format identifier |
| `author` | string | Format/application author |
| `software_version` | string | Meteoris software version |
| `hostname` | string | Hostname of the recording computer |
| `driver` | string | SoapySDR driver identifier |
| `hardware_info` | string | SDR/device information reported by the application |
| `acquisition_start_utc` | string | Human-readable acquisition start in UTC |
| `acquisition_start_ns` | uint64 | Acquisition start timestamp in nanoseconds |
| `sample_rate_hz` | double | SDR input sample rate |
| `center_frequency_hz` | double | SDR center/tuning frequency |
| `sdr_bandwidth_hz` | double | SDR bandwidth reported/configured for acquisition |
| `gain_db` | double | Initial receiver gain |
| `shift_hz` | double | DSP NCO frequency shift |
| `output_rate_hz` | double | Final complex sample rate after decimation |
| `psd_bandwidth_hz` | double | Selected PSD observation bandwidth |
| `decimation_stage1` | uint64 | First decimation factor |
| `decimation_stage2` | uint64 | Second decimation factor |
| `fft_size` | uint64 | PSD FFT length |
| `day_boundary` | string | UTC logical daily rotation boundary |
| `swmr` | string | Whether SWMR was enabled for the file |

---

## 5.1 `format`

Example:

```text
format = "meteoris v0"
```

This is the current HDF5 format identifier.

Readers should prefer checking this attribute rather than assuming that every
`.h5` file with similar datasets is a Meteoris file.

---

## 5.2 `author`

Current value:

```text
Fabrizio Pollastri
```

This attribute describes the Meteoris format/application author.

---

## 5.3 `software_version`

For the current software:

```text
0.2.0
```

This records the Meteoris version that created the new HDF5 file.

It is separate from the recording-format identifier so software can evolve
without necessarily changing the HDF5 format version.

---

## 5.4 `hostname`

Example:

```text
hostname = "meteor-station-01"
```

This is obtained from the operating system hostname.

It is useful when files from multiple receiving stations are collected into a
common archive.

---

## 5.5 `driver`

Example:

```text
driver = "hackrf-htime"
```

or:

```text
driver = "meteoris_sim"
```

This records the SoapySDR driver selected for the acquisition.

---

## 5.6 `hardware_info`

A string describing receiver/device information available to Meteoris.

The exact content depends on the SDR and SoapySDR driver.

Readers should treat this as descriptive metadata rather than a fixed-schema
field.

---

## 5.7 `acquisition_start_utc`

A human-readable UTC representation of acquisition start.

Example:

```text
2026-08-27T14:32:18.125Z
```

The current formatter records milliseconds in this string.

For machine processing, `acquisition_start_ns` is the preferred timestamp.

---

## 5.8 `acquisition_start_ns`

Type:

```text
uint64
```

Unit:

```text
nanoseconds since Unix epoch
```

This identifies the start time associated with the acquisition session.

PSD rows have their own independent `timestamp_ns` values.

---

## 5.9 `sample_rate_hz`

The original SDR input rate.

Typical value:

```text
10000000
```

for a 10 Msps acquisition.

---

## 5.10 `center_frequency_hz`

The SDR RF center frequency requested by Meteoris.

Example:

```text
142151200
```

The final PSD frequency axis is an **offset-frequency axis**, not absolute RF
frequency.

To reconstruct an approximate absolute RF frequency from a PSD bin, the
receiver center frequency and DSP `shift_hz` must both be considered.

---

## 5.11 `sdr_bandwidth_hz`

Records the SDR bandwidth associated with the receiver acquisition.

Its meaning and availability can depend on the SoapySDR driver.

It is distinct from `psd_bandwidth_hz`, which describes the final DSP/PSD
observation band.

---

## 5.12 `gain_db`

Initial receiver gain at acquisition setup.

Because AGC may subsequently change the gain, the gain actually associated
with each saved PSD row is stored separately in:

```text
/psd/gain_db
```

Therefore this metadata value should not be assumed to describe every row when
AGC is enabled.

---

## 5.13 `shift_hz`

Digital NCO translation applied before decimation.

Example:

```text
-1100000
```

This value is required when translating the final PSD offset frequencies back
toward absolute RF frequency.

---

## 5.14 `output_rate_hz`

Final complex sample rate after both FIR decimation stages.

For:

```text
input rate = 10 MHz
D1 = 8
D2 = 5
```

the value is:

```text
250000 Hz
```

---

## 5.15 `psd_bandwidth_hz`

Width of the selected stored PSD frequency band.

For:

```text
100000 Hz
```

the normal stored frequency axis spans approximately:

```text
-50 kHz ... +50 kHz
```

---

## 5.16 `decimation_stage1`

First DSP decimation factor.

Typical value:

```text
8
```

---

## 5.17 `decimation_stage2`

Second DSP decimation factor.

Typical value:

```text
5
```

Total decimation is therefore:

```text
decimation_stage1 * decimation_stage2
```

---

## 5.18 `fft_size`

Welch PSD FFT length.

Typical value:

```text
4096
```

This can be combined with `output_rate_hz` to determine nominal FFT bin
spacing:

```text
bin_spacing_hz = output_rate_hz / fft_size
```

For 250 kHz and 4096 points:

```text
~61.035 Hz/bin
```

---

## 5.19 `day_boundary`

Example:

```text
UTC daily at 06:00
```

Records the logical file-rotation boundary used when the file was created.

---

## 5.20 `swmr`

Current values are textual:

```text
enabled
```

or:

```text
disabled
```

This indicates whether the HDF5 writer was configured to use Single
Writer/Multiple Reader operation.

---

# 6. `/metadata/toml_config`

Path:

```text
/metadata/toml_config
```

This dataset stores the complete configuration text associated with file
creation.

If Meteoris was launched with a configuration file, the stored data represent
the configuration text used by the application.

If no external configuration file was supplied, Meteoris can store a generated
effective TOML configuration based on compiled/default/current values.

The dataset is a one-dimensional character array whose length equals the
number of bytes/characters written.

Conceptually:

```text
shape = [number_of_TOML_bytes]
```

The configuration is stored only once when the HDF5 file is created.

This is particularly valuable for later reproducibility because the file
carries the acquisition/DSP/detector/output configuration that produced it.

---

# 7. `/psd/frequency_hz`

Path:

```text
/psd/frequency_hz
```

Type:

```text
double
```

Shape:

```text
[number_of_frequency_bins]
```

This is a fixed-size dataset created once with the file.

It contains the frequency coordinate corresponding to columns of
`/psd/power_density`.

The values are:

- in Hz;
- relative to the translated DSP center;
- sorted from negative to positive frequency.

Typical span for a 100 kHz PSD band:

```text
approximately -50000 ... +50000 Hz
```

Every PSD row uses the same frequency axis.

If:

```text
F = len(/psd/frequency_hz)
```

then:

```text
/psd/power_density.shape[1] == F
```

must hold.

---

# 8. `/psd/power_density`

Path:

```text
/psd/power_density
```

On-disk datatype:

```text
IEEE 32-bit floating point, little-endian
```

Logical shape:

```text
[number_of_saved_PSD_rows, number_of_frequency_bins]
```

The first dimension is unlimited and grows while Meteoris records events.

The second dimension is fixed for the life of the file.

Each row is one selected-band Welch PSD frame.

The values are stored in **linear power-density units**, not dB/Hz.

The DSP computes approximately:

```text
power_density[k] =
    |FFT[k]|^2 /
    (window_power * final_sample_rate)
```

The detector and plotter convert these values to dB/Hz with:

```text
10 * log10(power_density)
```

when needed.

Therefore applications reading the file should not apply `20*log10`; these are
power values, so the appropriate logarithmic conversion is `10*log10`.

---

## 8.1 PSD chunking

The PSD dataset is chunked as:

```text
[128, number_of_frequency_bins]
```

This means one HDF5 chunk contains up to 128 consecutive PSD rows spanning the
full stored frequency axis.

The design favors:

- append operations;
- event-range reads;
- waterfall visualization.

---

## 8.2 PSD compression

If:

```toml
hdf5_compression > 0
```

gzip/DEFLATE compression is applied to `/psd/power_density`.

The configured compression level is used directly.

For example:

```toml
hdf5_compression = 1
```

selects low-effort gzip compression.

The scalar companion datasets are chunked but are not explicitly configured
for gzip compression in the current writer.

---

# 9. Row-aligned companion datasets

The following datasets have exactly one element for every PSD row:

```text
/psd/timestamp_ns
/psd/frame_index
/psd/event_id
/psd/detector_state
/psd/detector_max_db_hz
/psd/gain_db
```

Conceptually, if row `r` in `/psd/power_density` is:

```text
PSD[r, :]
```

then all of the following describe that same frame:

```text
timestamp_ns[r]
frame_index[r]
event_id[r]
detector_state[r]
detector_max_db_hz[r]
gain_db[r]
```

A reader should therefore treat these datasets as a row-aligned table.

---

# 10. `/psd/timestamp_ns`

Path:

```text
/psd/timestamp_ns
```

On-disk datatype:

```text
unsigned 64-bit integer, little-endian
```

Shape:

```text
[number_of_saved_PSD_rows]
```

Unit:

```text
nanoseconds since Unix epoch
```

The timestamp represents approximately the **center time of the FFT window**
used to construct that PSD frame.

It is not simply the time at which the HDF5 row was written.

This makes the time axis meaningful for waterfall/event analysis.

Conversion to seconds is:

```text
seconds = timestamp_ns / 1e9
```

---

# 11. `/psd/frame_index`

Path:

```text
/psd/frame_index
```

Datatype:

```text
uint64
```

Shape:

```text
[number_of_saved_PSD_rows]
```

This is the sequential PSD-frame index produced by the PSD engine during the
running acquisition.

Because Meteoris records only event-related frames, consecutive HDF5 rows do
not necessarily have consecutive `frame_index` values between different
events.

Within a continuously saved event, frame indices should normally progress with
the PSD cadence.

The field is useful for:

- detecting gaps;
- correlating saved frames with PSD-generation order;
- diagnostics.

---

# 12. `/psd/event_id`

Path:

```text
/psd/event_id
```

Datatype:

```text
uint64
```

Shape:

```text
[number_of_saved_PSD_rows]
```

Each saved row is associated with a detector event.

All rows belonging to a single event share the same stored `event_id`.

The same ID covers:

- pre-trigger rows;
- active rows;
- post-trigger rows;
- retriggered active periods merged into that event.

## Session/restart consideration

The current detector event counter is an in-memory counter. If Meteoris
restarts and appends to an existing same-day file, the numeric event IDs can
restart from their initial sequence.

For this reason, `meteoris_plot` does **not** rely only on event-ID changes to
separate file-local events. It also detects sufficiently large or invalid
timestamp discontinuities.

A consumer that needs a globally unique event identity should therefore not
assume that `event_id` alone is unique across process restarts.

A robust external identity can combine information such as:

```text
file identity + event start timestamp + stored event_id
```

---

# 13. `/psd/detector_state`

Path:

```text
/psd/detector_state
```

On-disk datatype:

```text
uint8
```

Shape:

```text
[number_of_saved_PSD_rows]
```

The current state values are:

| Value | Meaning |
|---:|---|
| `0` | Pre-trigger/history frame |
| `1` | Trigger-active frame |
| `2` | Post-trigger frame |

These state codes describe how each row was classified when it was stored.

---

## 13.1 State 0 — pre-trigger

Rows saved from the rolling pre-trigger buffer.

This includes the requested context before the trigger qualification sequence
and may include early qualifying frames that precede the final trigger
assertion.

---

## 13.2 State 1 — active

Rows recorded while the event is in its active trigger state.

The final row written before leaving active state is also state 1 because the
writer stores the frame before the detector transitions into post-trigger
mode.

---

## 13.3 State 2 — post-trigger

Rows recorded during the configured post-trigger context.

If the signal requalifies during post-trigger, the same event can return to
state 1.

Therefore an event may contain a sequence such as:

```text
0 0 0 1 1 1 2 2 2 1 1 1 2 2 2 2 ...
```

This is intentional retrigger merging.

---

## 13.4 No explicit complete-state marker

The current `meteoris v0` schema does not contain a separate state value such
as:

```text
3 = event complete
```

Consequently a live SWMR reader cannot determine event completion solely from
one explicit terminal marker.

`meteoris_plot` therefore interprets the visible state sequence
conservatively, especially for the most recent event in an active file.

---

# 14. `/psd/detector_max_db_hz`

Path:

```text
/psd/detector_max_db_hz
```

On-disk datatype:

```text
IEEE 32-bit floating point, little-endian
```

Shape:

```text
[number_of_saved_PSD_rows]
```

This stores a compact PSD-level metric associated with the recorded frame.

With the peak/track detector, the value is the maximum **raw final-band PSD
density** in dB/Hz:

```text
detector_max_db_hz =
    max(10*log10(power_density[k]))
```

over the complete stored observation band.

Trigger decisions are no longer made by comparing this field with one absolute
threshold. Detection uses frequency/time-smoothed PSDs, median-relative local
peaks, and peak tracks. The dataset name is retained for HDF5 `meteoris v0`
compatibility.

---

# 15. `/psd/gain_db`

Path:

```text
/psd/gain_db
```

On-disk datatype:

```text
IEEE 32-bit floating point, little-endian
```

Shape:

```text
[number_of_saved_PSD_rows]
```

This records the SDR gain associated with each PSD frame.

It is especially important when Meteoris AGC is enabled because gain may vary
during the acquisition.

A waterfall or offline analysis can therefore correlate PSD-level changes with
receiver gain changes.

---

# 16. One HDF5 row as a logical record

A convenient way to interpret the file is to view every row as:

```text
PSDRecord {
    timestamp_ns
    frame_index
    event_id
    detector_state
    detector_max_db_hz
    gain_db
    power_density[frequency_bin]
}
```

where the frequency coordinate is shared globally through:

```text
/psd/frequency_hz
```

The file is therefore effectively an append-only table of variable-time PSD
records with one large array-valued field.

---

# 17. Event representation

Events are **not** represented as separate HDF5 groups.

There is no structure such as:

```text
/events/event_0001/
```

Instead all event rows are appended to the same `/psd` datasets and event
membership is represented by:

```text
event_id
```

plus temporal continuity.

Advantages of this design include:

- simple append operations;
- no HDF5 object creation during normal SWMR writing;
- compact schema;
- efficient sequential reads;
- easy construction of waterfall ranges.

This is particularly suitable for HDF5 SWMR because the complete schema can be
created before SWMR writing starts.

---

# 18. Typical event row sequence

For an event with pre-trigger, active, and post-trigger context:

```text
row      event_id   state
----     --------   -----
100         7         0
101         7         0
102         7         0
...
110         7         1   <- trigger active
111         7         1
112         7         1
...
125         7         2   <- post-trigger
126         7         2
...
134         7         2
```

The next event may begin much later:

```text
200         8         0
201         8         0
...
```

Rows corresponding to uninteresting idle periods are not present in the HDF5
file.

---

# 19. Retriggered event example

Because Meteoris merges retriggers occurring during the post-trigger tail, an
event can look like:

```text
event_id = 12

state:
0 0 0 0
1 1 1 1
2 2
1 1 1
2 2 2 2 2 ...
```

The transition:

```text
2 -> 1
```

means the signal satisfied the consecutive-frame qualification again before
the post-trigger context expired.

The same event ID is retained.

`meteoris_plot` displays all such ON/OFF transitions.

---

# 20. Maximum-event cutoff representation

If `max_event_seconds` is reached, Meteoris stops the event immediately.

Unlike a normal trigger OFF, a forced maximum-duration cutoff does not append a
new normal post-trigger tail.

The current HDF5 schema does not include a dedicated per-row flag explicitly
saying:

```text
event ended because max_event_seconds was reached
```

That information is available in the running program summary through its
forced-cutoff counter, but it is not represented as a separate HDF5 event-end
record in `meteoris v0`.

Therefore an offline reader should not infer a normal post-tail requirement
for every event solely from the row count.

---

# 21. Graceful shutdown and recording completeness

On the first Ctrl-C/SIGTERM request, Meteoris now waits when a detector event
is in progress.

It continues acquisition until the active event and its post-trigger context
have completed, then flushes/closes the HDF5 writer.

This reduces the chance that the final event in a normally stopped file is
truncated.

A second Ctrl-C can still request a forced immediate shutdown; in that case
the final event can be incomplete.

External failures such as power loss can also leave the final event incomplete.

---

# 22. HDF5 SWMR

Meteoris supports HDF5 **Single Writer / Multiple Reader** mode.

Relevant configuration:

```toml
[output]
swmr = true
swmr_flush_seconds = 1.0
```

When SWMR is enabled, a live reader can open the active file while Meteoris is
still writing.

New SWMR files are created using HDF5's latest-format bounds and the complete
groups/datasets are created before:

```text
H5Fstart_swmr_write()
```

is called.

This is why the schema is fixed and append-oriented.

---

# 23. SWMR publication interval

After rows are appended, the writer marks the file as dirty.

At the configured publication interval it flushes:

```text
/psd/power_density
/psd/timestamp_ns
/psd/frame_index
/psd/event_id
/psd/detector_state
/psd/detector_max_db_hz
/psd/gain_db
```

and then performs a file-level flush.

For:

```toml
swmr_flush_seconds = 1.0
```

live readers typically see newly published rows with about one second or less
of publication latency.

A value of:

```toml
swmr_flush_seconds = 0
```

requests publishing after every saved PSD append, at greater I/O cost.

---

# 24. Reading a live file

With `h5py`, the intended live-reader pattern is:

```python
import h5py

h5 = h5py.File(
    "data/meteoris_20260827.h5",
    "r",
    libver="latest",
    swmr=True,
)
```

Before looking for newly appended rows, refresh growing datasets:

```python
h5["/psd/power_density"].refresh()
h5["/psd/timestamp_ns"].refresh()
h5["/psd/event_id"].refresh()
```

and similarly for other row-aligned datasets as needed.

`meteoris_plot` performs this automatically when using its live refresh
function.

---

# 25. Mutually visible SWMR prefix

The row-aligned datasets are extended/written individually by the writer.

A live reader can theoretically observe a brief publication point at which one
dataset reports a slightly different visible length from another.

For this reason, `meteoris_plot` uses a conservative **mutually visible
prefix** where necessary.

For example:

```text
visible_rows =
min(
    len(event_id),
    len(timestamp_ns),
    ...
)
```

A custom live reader should follow the same principle rather than assuming
that every growing dataset is visible at exactly the same instant.

Closed files should have matching row counts across all row-aligned datasets.

---

# 26. HDF5 schema creation and append

At creation time Meteoris creates:

```text
/metadata
/psd
```

then:

- metadata attributes;
- `/metadata/toml_config`;
- `/psd/frequency_hz`;
- every extendible `/psd/*` dataset.

Only after the complete schema exists does SWMR writing start.

During normal recording, Meteoris does not create a new group or dataset for
each event.

It only extends the existing datasets.

This is a deliberate SWMR-friendly architecture.

---

# 27. 1-D dataset chunking

The row-aligned scalar datasets are created as unlimited one-dimensional
datasets with chunks of:

```text
256 rows
```

This applies to:

```text
timestamp_ns
frame_index
event_id
detector_state
detector_max_db_hz
gain_db
```

The chunk size is independent of event boundaries.

---

# 28. Existing-file append behavior

When the target logical-day file already exists, Meteoris opens it read/write,
opens the expected datasets, verifies the PSD frequency-bin count, obtains the
current number of PSD rows, and continues appending.

The existing file must have a compatible schema.

If SWMR is requested, the existing file must also be compatible with starting
SWMR writer mode.

Older files created with incompatible HDF5 format/library bounds can fail to
reopen as a live SWMR writer.

---

# 29. Legacy `gain_db` handling

The writer contains limited compatibility handling for an older file lacking:

```text
/psd/gain_db
```

If that dataset is absent, Meteoris creates it, extends it to the existing row
count, and fills existing rows with the acquisition's metadata gain value.

This gives legacy rows a reasonable fixed-gain fallback.

Other required missing datasets are considered an incompatible schema.

---

# 30. File-format validation on append

For an existing file, Meteoris requires these datasets:

```text
/psd/power_density
/psd/timestamp_ns
/psd/frame_index
/psd/event_id
/psd/detector_state
/psd/detector_max_db_hz
```

It also verifies that:

```text
power_density.shape[1]
```

matches the current frequency-axis size expected by the running DSP.

A mismatch causes the file to be rejected rather than appending rows with a
different PSD geometry.

---

# 31. Storage growth protection

HDF5 recording is monitored for excessive file growth.

Configuration:

```toml
max_growth_mb_per_min = 100
```

The writer periodically checks the file size.

The growth rate is evaluated over windows of at least approximately 10 seconds
to reduce false alarms from HDF5 chunk allocation/compression bursts.

If the sustained growth rate exceeds the configured limit, Meteoris raises an
error and stops rather than allowing uncontrolled storage consumption.

Setting:

```toml
max_growth_mb_per_min = 0
```

disables this check.

---

# 32. Free-space protection

Configuration:

```toml
min_free_space_gb = 2
```

The writer queries available space on the filesystem containing the output
directory.

If free space falls below the configured threshold, Meteoris stops recording
instead of filling the filesystem completely.

Setting:

```toml
min_free_space_gb = 0
```

disables this protection.

---

# 33. File flush and close

On normal close, Meteoris:

1. forces publication of any dirty SWMR data;
2. closes each dataset;
3. performs a global HDF5 file flush;
4. closes the file.

This occurs during:

- normal program shutdown;
- daily file rotation;
- recorder destruction.

A normal graceful shutdown therefore leaves the HDF5 metadata and datasets in
a closed, synchronized state.

---

# 34. Units summary

| Field | Unit |
|---|---|
| `frequency_hz` | Hz offset from translated center |
| `power_density` | linear normalized power density |
| `timestamp_ns` | ns since Unix epoch |
| `frame_index` | dimensionless sequential PSD index |
| `event_id` | dimensionless detector event ID |
| `detector_state` | enumerated state code |
| `detector_max_db_hz` | dB/Hz |
| `gain_db` | dB |
| `sample_rate_hz` | Hz |
| `center_frequency_hz` | Hz |
| `sdr_bandwidth_hz` | Hz |
| `shift_hz` | Hz |
| `output_rate_hz` | Hz |
| `psd_bandwidth_hz` | Hz |

---

# 35. Datatype summary

The principal dataset datatypes are:

| Dataset | HDF5 datatype |
|---|---|
| `/metadata/toml_config` | native character array |
| `/psd/frequency_hz` | native double |
| `/psd/power_density` | IEEE F32 little-endian |
| `/psd/timestamp_ns` | U64 little-endian |
| `/psd/frame_index` | U64 little-endian |
| `/psd/event_id` | U64 little-endian |
| `/psd/detector_state` | U8 |
| `/psd/detector_max_db_hz` | IEEE F32 little-endian |
| `/psd/gain_db` | IEEE F32 little-endian |

HDF5 itself records datatype metadata and performs conversions when a reader
requests compatible native types.

---

# 36. Dimension summary

Let:

```text
R = number of saved PSD rows
F = number of stored frequency bins
C = length of embedded TOML configuration
```

Then the logical dimensions are:

| Dataset | Shape |
|---|---|
| `/metadata/toml_config` | `[C]` |
| `/psd/frequency_hz` | `[F]` |
| `/psd/power_density` | `[R, F]` |
| `/psd/timestamp_ns` | `[R]` |
| `/psd/frame_index` | `[R]` |
| `/psd/event_id` | `[R]` |
| `/psd/detector_state` | `[R]` |
| `/psd/detector_max_db_hz` | `[R]` |
| `/psd/gain_db` | `[R]` |

All datasets involving `R` grow as event PSD frames are appended.

---

# 37. Example Python inspection

A simple closed-file reader can use:

```python
import h5py
import numpy as np

with h5py.File("data/meteoris_20260827.h5", "r") as h5:
    meta = h5["/metadata"]
    freq = np.asarray(h5["/psd/frequency_hz"])
    psd = np.asarray(h5["/psd/power_density"])
    ts = np.asarray(h5["/psd/timestamp_ns"])
    event_id = np.asarray(h5["/psd/event_id"])
    state = np.asarray(h5["/psd/detector_state"])

    print(meta.attrs["format"])
    print(psd.shape)
```

Convert one PSD row to dB/Hz with:

```python
row_db = 10.0 * np.log10(
    np.maximum(psd[row], np.finfo(np.float32).tiny)
)
```

---

# 38. Reconstructing event segments

For one uninterrupted Meteoris process, event boundaries can usually be found
where:

```text
event_id[i] != event_id[i-1]
```

However, because IDs can restart after a process restart that appends to the
same daily file, a robust reader should also look for a timestamp
discontinuity.

The current `meteoris_plot` logic treats a new segment as beginning if either:

```text
event ID changes
```

or:

```text
timestamp gap is much larger than normal PSD cadence
```

or time is non-monotonic.

This makes file-local event enumeration more robust than using the numeric
stored ID alone.

---

# 39. Converting offset frequency to approximate RF frequency

`/psd/frequency_hz` is the frequency **after digital translation**.

With the normal complex convention used by Meteoris:

```text
f_final =
f_RF - center_frequency_hz + shift_hz
```

therefore approximately:

```text
f_RF =
center_frequency_hz - shift_hz + f_final
```

For example:

```text
center_frequency = 142151200 Hz
shift_hz         = -1100000 Hz
PSD bin          = +12000 Hz
```

gives approximately:

```text
f_RF =
142151200 - (-1100000) + 12000
= 143263200 Hz
```

Actual RF accuracy also depends on the SDR oscillator, driver convention, and
frequency calibration.

---

# 40. Why raw I/Q is not stored

At 10 Msps with signed 8-bit I and Q, continuous raw storage would require
approximately:

```text
10,000,000 complex samples/s
x 2 bytes/complex sample
= 20 MB/s
```

before filesystem/container overhead.

That is approximately:

```text
1.2 GB/min
72 GB/hour
```

Meteoris instead records only the reduced PSD representation around detected
events.

This greatly lowers:

- storage bandwidth;
- file size;
- long-term archive requirements;
- later analysis cost.

The trade-off is that the original time-domain I/Q waveform cannot be
reconstructed from the HDF5 PSD file.

---

# 41. Format design rationale

The `meteoris v0` format uses one append-only PSD table rather than per-event
groups because this provides:

- simple real-time extension;
- efficient HDF5 chunking;
- straightforward SWMR operation;
- no dynamic HDF5 object creation during acquisition;
- easy range-based event reads;
- compact storage;
- simple plotting.

Event semantics are carried by aligned scalar columns rather than by HDF5
object hierarchy.

---

# 42. Current format limitations

The current format intentionally remains simple, but several limitations are
worth understanding.

## No raw I/Q

Only PSD information is stored.

## No explicit event-end record

Event completion is inferred from row/state/timing information.

## No explicit forced-cutoff flag per event

A maximum-duration termination is not represented by a dedicated HDF5 field.

## Numeric event IDs can restart across process restarts

A robust reader should use timing/session context in addition to `event_id`.

## Metadata describe the file/session configuration

Some runtime values, especially SDR gain under AGC, can change after file
creation; per-row `gain_db` is therefore authoritative for each recorded PSD.

These limitations can be addressed in a future HDF5 format version if needed.

---

# 43. Compact schema reference

```text
/metadata
    attrs:
        format                  string   "meteoris v0"
        author                  string   "Fabrizio Pollastri"
        software_version        string
        hostname                string
        driver                  string
        hardware_info           string
        acquisition_start_utc   string
        acquisition_start_ns    uint64
        sample_rate_hz          double
        center_frequency_hz     double
        sdr_bandwidth_hz        double
        gain_db                 double
        shift_hz                double
        output_rate_hz          double
        psd_bandwidth_hz        double
        decimation_stage1       uint64
        decimation_stage2       uint64
        fft_size                uint64
        day_boundary            string
        swmr                    string

/metadata/toml_config
    char[C]

/psd/frequency_hz
    double[F]

/psd/power_density
    float32[R,F]
    unlimited first dimension
    chunk = [128,F]
    optional gzip compression

/psd/timestamp_ns
    uint64[R]
    chunk = [256]

/psd/frame_index
    uint64[R]
    chunk = [256]

/psd/event_id
    uint64[R]
    chunk = [256]

/psd/detector_state
    uint8[R]
    chunk = [256]
    0 = pre-trigger
    1 = active
    2 = post-trigger

/psd/detector_max_db_hz
    float32[R]
    chunk = [256]

/psd/gain_db
    float32[R]
    chunk = [256]
```

---

# 44. Summary

A Meteoris recording is best understood as:

> **A daily, append-only, SWMR-capable HDF5 table of triggered Welch PSD
> frames, with a fixed shared frequency axis, per-frame timing/detector/gain
> columns, and file-level acquisition/configuration metadata.**

The format preserves enough information to:

- display time-frequency waterfalls;
- enumerate and inspect detected events;
- identify trigger/retrigger intervals;
- recover PSD levels in linear units or dB/Hz;
- associate rows with UTC time and receiver gain;
- reproduce the acquisition/DSP configuration;
- inspect an active recording safely through SWMR.


# Continuous recording mode

With:

```toml
[recording]
mode = "continuous"
```

every generated PSD is written. Data is divided into bounded sequences using
`recording.segment_seconds`. Each sequence receives a new `event_id`.

The existing `detector_state` field is retained for `meteoris v0`
compatibility and is written as neutral value `0`; it must not be interpreted
as a detector trigger in continuous mode. The embedded TOML identifies the
recording mode.

Daily file rotation is delayed until a continuous sequence boundary, matching
the existing rule that a triggered event is never split across daily files.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public License v3.0; see `LICENSE`.
