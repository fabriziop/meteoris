# Meteoris Cheat Sheet

A compact reference for installing, configuring, running, and inspecting
Meteoris. For details and parameter explanations, use the main README and the
files in `doc/`.

## 1. Build and install

### Ubuntu/Kubuntu

Install the dependencies described in [INSTALL.md](INSTALL.md), then from the
repository root:

```bash
./build.sh
```

Install for the current user:

```bash
./install.sh --local
```

or system-wide:

```bash
sudo ./install.sh --system
```

To omit the SoapySDR simulator plugin during installation:

```bash
./install.sh --local --no-sim
```

### Raspberry Pi OS

For a native Raspberry Pi build:

```bash
./build_rpi3.sh
```

For the complete native/cross-build procedure and dependencies, see
[INSTALL.md](INSTALL.md).

## 2. Prepare a working directory

Copy the example configurations into the directory from which Meteoris will be
run:

```bash
cp /path/to/meteoris/config/meteoris.toml .
cp /path/to/meteoris/config/meteoris_plot.toml .
```

The recorder automatically reads `./meteoris.toml` when present. An explicit
configuration can be selected with:

```bash
meteoris --config /path/to/meteoris.toml
```

Command-line options override the TOML configuration.

## 3. Minimum recorder configuration

For a real SDR, check at least these entries in `meteoris.toml`:

```toml
[sdr]
driver = "hackrf"
sample_rate = 10000000
center_frequency = 143760000
gain = 90

[dsp]
shift_hz = +708650

[detector]
plugin = "peak_tracker"

[output]
directory = "./data"
```

`center_frequency`, `shift_hz`, gain, and detector settings must be chosen for
the receiver and transmitter being observed. See the configuration sections in
[README.md](../README.md) and [CONFIG_FILE_REFERENCE.md](CONFIG_FILE_REFERENCE.md).

## 4. Run Meteoris

Foreground:

```bash
meteoris
```

Stop cleanly with:

```text
Ctrl-C
```

Run detached:

```bash
meteoris --daemon
```

Show command-line options:

```bash
meteoris --help
```

Show the version:

```bash
meteoris --version
```

With the default output settings, daily HDF5 files are written as:

```text
data/meteoris_YYYYMMDD.h5
```

and the text log is written as dated `meteoris_YYYYMMDD.log` files when daily
log rotation is enabled.

## 5. Useful command-line overrides

Examples:

```bash
meteoris --gain 80
meteoris --center-frequency 143050000
meteoris --shift-hz 998650
meteoris --run-seconds 60
meteoris --log-level debug
meteoris --record-mode continuous --record-segment-seconds 15
```

Configuration precedence is:

```text
defaults < TOML configuration < command-line overrides
```

## 6. Run the simulator

From the source tree:

```bash
./run_sim.sh
```

This forces SoapySDR to use the freshly built `meteoris_sim` plugin instead of
an older installed copy. Simulator signal parameters are in
`config/meteoris_sim.toml`.

## 7. Display recorded events

From a directory containing `meteoris_plot.toml`:

```bash
meteoris_plot data/meteoris_YYYYMMDD.h5
```

Useful non-interactive commands:

```bash
meteoris_plot --list-events data/meteoris_YYYYMMDD.h5
meteoris_plot --events 1-5 data/meteoris_YYYYMMDD.h5
meteoris_plot --events 1,3,7-9 data/meteoris_YYYYMMDD.h5
meteoris_plot --save-dir plots --no-show data/meteoris_YYYYMMDD.h5
```

### Viewer keys

| Key | Function |
| --- | --- |
| `Space` / `Enter` | Next event |
| `Shift+Space` / `Shift+Enter` | Previous event |
| digits + `Space` / `Enter` | Jump to event number |
| `Backspace` / `Delete` | Edit jump number |
| `Esc` | Clear jump number |
| `z` / `x` | Skip -20 / -10 events |
| `c` / `v` | Skip +10 / +20 events |
| `n` | Select HDF5 filename for saved events |
| `w` | Write/append current event to the selected HDF5 file |
| `r` | Refresh a live SWMR file |
| `q` | Quit |

If no destination was selected with `n`, `w` uses:

```text
meteoris_saved.h5
```

The **Max PSD**, **Trigger**, and **Grid** buttons control the corresponding
plot overlays/displays. PSD color limits can be adjusted interactively with the
sliders.

## 8. Live viewing

The default HDF5 output uses SWMR, so the active daily file can be viewed while
Meteoris is still recording:

```bash
meteoris_plot data/meteoris_YYYYMMDD.h5
```

Use `r` to refresh the growing file and event list.

## 9. Quick health checks

Normal real-time operation should show no SDR overflows or DSP deadline misses.
For detailed diagnostics run temporarily with:

```bash
meteoris --log-level debug
```

Important conditions to investigate include:

```text
SDR overflow / acquisition errors
DSP deadline misses
recorder queue overrun
storage free-space or growth-rate safety errors
```

The recorder queue is bounded; if storage falls behind far enough to fill it,
Meteoris reports the condition rather than silently dropping PSD frames.

## 10. Recovering an HDF5 file

After an ungraceful termination, if an output file cannot be reopened normally,
follow:

[RECOVER_HDF5.md](RECOVER_HDF5.md)

## 11. Where to look next

- Full installation instructions: [INSTALL.md](INSTALL.md)
- Configuration reference: [CONFIG_FILE_REFERENCE.md](CONFIG_FILE_REFERENCE.md)
- DSP pipeline: [DSP_PIPELINE.md](DSP_PIPELINE.md)
- Detector details: [DETECTOR.md](DETECTOR.md)
- Detector plugin API: [DETECTOR_PLUGIN_API.md](DETECTOR_PLUGIN_API.md)
- Recording format: [RECORDING_FORMAT.md](RECORDING_FORMAT.md)
