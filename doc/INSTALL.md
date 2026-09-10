# Download, Build and Install

Meteoris can be bult and installed on X86 and ARM platforms.


## Getting Meteoris

Meteoris can be cloned of downloaded from the [github
repository](https://github.com/fabriziop/meteoris).

### Repository layout

```text
meteoris/
├── CMakeLists.txt
├── build.sh
├── install.sh
├── run_sim.sh
├── src/
│   ├── meteoris.cpp
│   └── detector/
│       ├── detector.hpp
│       ├── detector_registry.cpp
│       └── peak_tracker_detector.cpp
├── sim/
│   ├── CMakeLists.txt
│   └── SoapyMeteorisSim.cpp
├── tools/
│   └── meteoris_plot.py
├── config/
│   ├── meteoris.toml
│   ├── meteoris_sim.toml
│   └── meteoris_plot.toml
├── doc/
│   ├── CONFIG_FILE_REFERENCE.md
│   ├── DETAILED_FEATURES.md
│   ├── DETECTOR.md
│   ├── DETECTOR_PLUGIN_API.md
│   ├── DSP_PIPELINE.md
│   ├── INSTALL.md
│   ├── RECORDING_FORMAT.md
│   └── event.png
├── README.md
└── LICENSE
```

The main executable remains in `src/meteoris.cpp`. Detector algorithms are
separated behind the phase-1 detector interface in `src/detector/`; the
currently registered implementation is `peak_tracker_detector.cpp`.

The SoapySDR test device is built from `sim/`, while `tools/` contains the
Python event viewer. `run_sim.sh` runs against the freshly built in-tree
simulator plugin, and `install.sh` supports local or system installation.

The generated `build/` directory is intentionally out of source and is not
part of the repository layout above.


## Kubuntu/Ubuntu

### Build Dependencies

First, comply with the following dependencies installing the required
packages as follows.

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsoapysdr-dev soapysdr-tools \
    libhdf5-dev libspdlog-dev \
    python3-numpy python3-matplotlib python3-h5py
```

Python 3.11+ supplies `tomllib`. On older Python versions, install `tomli`
or adapt the plotting tool accordingly.


### Build

```bash
./build.sh
```

This is equivalent to:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
```

A clean rebuild is:

```bash
rm -rf build
./build.sh
```

### Portable build

Native CPU optimization is enabled by default. Disable it when building
binaries intended to run on a different CPU:

```bash
./build.sh -DMETEORIS_NATIVE_OPTIMIZATION=OFF
```

Disable the simulator if only the recorder is needed:

```bash
./build.sh -DMETEORIS_BUILD_SIM=OFF
```

## Raspberry Pi with Raspberry Pi OS Trixie

Two supported workflows are available:

- Native build directly on the Raspberry Pi.
- Cross-build on a faster x86_64 Linux host.

### Native build on the Pi

Install dependencies according to the build mode.

Full build (recorder + plotting tool):

```bash
sudo apt update
sudo apt install \
        build-essential cmake pkg-config \
        libsoapysdr-dev soapysdr-tools \
        libhdf5-dev libspdlog-dev libfftw3-dev \
        python3-numpy python3-matplotlib python3-h5py
```

Recorder-only build (no plotting tool):

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsoapysdr-dev soapysdr-tools \
    libhdf5-dev libspdlog-dev libfftw3-dev
```

Then configure and build:

```bash
./build_rpi3.sh
```

`build_rpi3.sh` first checks local hardware/OS; on a Raspberry Pi it selects
the native preset even if `RPI_SYSROOT` is set in the environment.

### Cross-build for Pi 3B (aarch64)

On the host machine, install aarch64 cross tools:

```bash
sudo apt update
sudo apt install \
        cmake make pkg-config \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

For cross builds, Python plotting dependencies are only required in the target
sysroot when using the full build mode.

Create or sync a Raspberry Pi sysroot that includes target runtime and
development libraries (SoapySDR, HDF5, spdlog, FFTW and their dependencies).

Set the sysroot path and run the Pi preset:

```bash
export RPI_SYSROOT=/opt/sysroots/rpi-trixie-aarch64
./build_rpi3.sh
```

When `RPI_SYSROOT` is set, `build_rpi3.sh` automatically uses a cross-build
preset.

You can force behavior explicitly:

```bash
./build_rpi3.sh --native
export RPI_SYSROOT=/opt/sysroots/rpi-trixie-aarch64
./build_rpi3.sh --cross
```

If neither condition is met (not running on a Raspberry Pi and no
`RPI_SYSROOT`), the script stops with a clear error message.

`build_rpi3.sh` asks which build you want:

- full build: `meteoris` + `meteoris_plot`
- recorder-only build: `meteoris` only

You can also select the preset directly:

```bash
export RPI_SYSROOT=/opt/sysroots/rpi-trixie-aarch64
cmake --preset rpi3-aarch64-cross-release-full
cmake --build --preset rpi3-aarch64-cross-release-full

cmake --preset rpi3-aarch64-cross-release-recorder-only
cmake --build --preset rpi3-aarch64-cross-release-recorder-only

cmake --preset rpi3-native-release-full
cmake --build --preset rpi3-native-release-full

cmake --preset rpi3-native-release-recorder-only
cmake --build --preset rpi3-native-release-recorder-only
```

The cross preset intentionally uses:

- `METEORIS_NATIVE_OPTIMIZATION=OFF` to avoid `-march=native` on the host.
- `METEORIS_CPU_TUNE=cortex-a53` for Raspberry Pi 3B class CPUs.
- `METEORIS_BUILD_SIM=OFF` to avoid installing/testing a host-incompatible
    SoapySDR plugin in cross-build output.
- `METEORIS_INSTALL_PLOT=OFF` in recorder-only mode.

You can then deploy `build-rpi3-aarch64/meteoris` to the Pi together with the
runtime configuration files.

## Install

Meteoris provides two explicit install modes. By default they install the
recorder, viewer **and the current `meteoris_sim` SoapySDR plugin**. This avoids
leaving an older simulator module active after source changes. Use `--no-sim`
when the simulator is not wanted.

### Local user install

```bash
./install.sh --local
```

This installs:

```text
~/.local/bin/meteoris
~/.local/bin/meteoris_plot
~/.local/share/meteoris/
```

If `~/.local/bin` is not in `PATH`, add:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

### System-wide install

```bash
sudo ./install.sh --system
```

The default system prefix is `/usr/local`, producing:

```text
/usr/local/bin/meteoris
/usr/local/bin/meteoris_plot
/usr/local/share/meteoris/
```

A custom prefix is also supported:

```bash
./install.sh --local --prefix "$HOME/apps/meteoris"
```

The install script builds the complete project and installs the simulator
plugin by default. To install only Meteoris and the viewer:

```bash
./install.sh --local --no-sim
# or
sudo ./install.sh --system --no-sim
```

For development/testing without installing anything, use:

```bash
./run_sim.sh
```

`run_sim.sh` forces SoapySDR to load the plugin from this repository's build
directory, preventing an older installed `meteoris_sim` module from being used.

---

Copyright (c) 2026 Fabrizio Pollastri. Licensed under the GNU General Public
License v3.0; see `LICENSE`.
