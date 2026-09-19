# Download, Build and Install

Meteoris is built from one repository for three supported target families:
Linux x86/x86_64, Linux ARM64, and Windows x86/x64. Windows ARM/ARM64 is
intentionally excluded.


## Package version

The root `VERSION` file is the single source of truth for the package version.
CMake, the C++ executable, and the installed Python tools all derive their
reported version from it. See [Versioning](VERSIONING.md) for the release
workflow.

## Getting Meteoris

Meteoris can be cloned of downloaded from the [github
repository](https://github.com/fabriziop/meteoris).

### Repository layout

See [Repository Layout](REPOSITORY_LAYOUT.md) for the current source-tree structure and component overview.

## Kubuntu/Ubuntu

### Build Dependencies

First, comply with the following dependencies installing the required
packages as follows.

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsoapysdr-dev soapysdr-tools \
    libhdf5-dev hdf5-tools libspdlog-dev \
    python3-numpy python3-matplotlib python3-h5py
```

Python 3.11+ supplies `tomllib`. On older Python versions, install `tomli`
or adapt the plotting tool accordingly.

`hdf5-tools` is required by `meteoris_recover_hdf5` because the recovery helper
uses the standard `h5dump`, `h5clear`, and `h5ls` utilities. It is therefore a
runtime dependency in both Raspberry Pi build modes, including recorder-only.


### Build

```bash
./build.sh
```

The script asks for `1) Full` or `2) Recorder only`. The corresponding native
build directories are `build-full/` and `build-recorder-only/`. For scripted
use, select the mode directly with `--full` or `--recorder-only`.

A clean full rebuild is:

```bash
rm -rf build-full
./build.sh --full
```

### Portable build

Native CPU optimization is enabled by default. Disable it when building
binaries intended to run on a different CPU:

```bash
./build.sh -DMETEORIS_NATIVE_OPTIMIZATION=OFF
```

For a recorder-only build, use the dedicated mode rather than overriding
individual CMake switches:

```bash
./build.sh --recorder-only
```

## Windows x86/x64

Windows uses the same `CMakeLists.txt`, C++ sources, configuration files, and
Python tools as Linux. Do not create or maintain a separate Windows source
branch or copy. Windows ARM/ARM64 is intentionally rejected by CMake; ARM64 is
a Linux-only target for this project.

### Build dependencies

Install:

- Visual Studio 2022 or Build Tools with **Desktop development with C++**.
- CMake and Git.
- Python 3.11 or newer for the helper tools.
- SoapySDR, HDF5, spdlog, and optionally FFTW3 for the same target architecture.

[vcpkg](https://github.com/microsoft/vcpkg) is a convenient way to provide the
C/C++ dependencies. For an x64 build, a typical dependency setup is:

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
& $env:VCPKG_ROOT\vcpkg.exe install soapysdr hdf5 spdlog fftw3 --triplet x64-windows
```

Meteoris itself only requires the SoapySDR core API at build time. To use real
SDR hardware, install a matching Windows SoapySDR hardware module/driver as
well. Availability is device-specific.

For the Python plotting and HDF5 tools:

```powershell
py -3 -m pip install numpy matplotlib h5py
```

`meteoris_recover_hdf5` also requires the HDF5 command-line programs `h5dump`,
`h5clear`, and `h5ls` to be available on `PATH`.

### Build

From a Developer PowerShell prompt:

```powershell
.\build.ps1 -Mode full -Architecture x64 -Generator "Visual Studio 17 2022"
```

Recorder-only:

```powershell
.\build.ps1 -Mode recorder-only -Architecture x64 -Generator "Visual Studio 17 2022"
```

If `VCPKG_ROOT` is set, `build.ps1` automatically uses its CMake toolchain.
Without vcpkg, the script also augments `CMAKE_PREFIX_PATH` with dependency
installations it finds in common Windows locations, including `Program Files`
(for spdlog, SoapySDR/PothosSDR and HDF5), `C:\deps`, and sibling
`spdlog`/`SoapySDR` build or install trees. Existing `CMAKE_PREFIX_PATH` entries
are preserved. An explicit `-DCMAKE_PREFIX_PATH=...` passed with `-CMakeOption`
remains authoritative.

Additional CMake definitions can be supplied with `-CMakeOption`, for example:

```powershell
.\build.ps1 -Mode full -CMakeOption '-DMETEORIS_USE_FFTW=OFF'
```

The default x64 build does not require AVX2. To deliberately build an AVX2-only
binary, add `-DMETEORIS_X86_AVX2=ON`.

### Install

A per-user install defaults to `%LOCALAPPDATA%\Meteoris`:

```powershell
.\install.ps1 -Local
```

A system install defaults to `%ProgramFiles%\Meteoris` and normally requires
an elevated PowerShell prompt:

```powershell
.\install.ps1 -System
```

On Windows the installer adds the install `bin` directory to `PATH` automatically.
`-System` adds `%ProgramFiles%\Meteoris\bin` to the machine `PATH`; `-Local`
adds `%LOCALAPPDATA%\Meteoris\bin` to the current user's `PATH`. The running
PowerShell process is updated too, so `meteoris` and the helper commands can be
called immediately after installation. Python commands are installed as a `.py`
implementation plus a `.cmd` launcher, for example `meteoris_plot.cmd`.

`runtime.daemon=true` and `--daemon` are Linux/POSIX-only. On Windows run
`meteoris.exe` in the foreground, or use an external Windows service wrapper if
a service deployment is required.

## Raspberry Pi with Raspberry Pi OS Trixie

The same `build.sh` is used on x86 Linux and Raspberry Pi. It asks for one of
two build modes:

- **Full**: `meteoris` + `meteoris_plot` + `meteoris_config` + `meteoris_recover_hdf5`
- **Recorder only**: `meteoris` + `meteoris_config` + `meteoris_recover_hdf5`

### Native build on the Pi

Install dependencies according to the selected build mode.

Full build:

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsoapysdr-dev soapysdr-tools \
    libhdf5-dev hdf5-tools libspdlog-dev libfftw3-dev \
    python3-numpy python3-matplotlib python3-h5py
```

Recorder-only build:

```bash
sudo apt update
sudo apt install \
    build-essential cmake pkg-config \
    libsoapysdr-dev soapysdr-tools \
    libhdf5-dev hdf5-tools libspdlog-dev libfftw3-dev
```

Then run:

```bash
./build.sh
```

Choose `1` for full or `2` for recorder-only. For non-interactive builds use:

```bash
./build.sh --full
./build.sh --recorder-only
```

On Raspberry Pi 3 hardware the script preserves the Cortex-A53 tuning used by
the previous Pi-specific build helper. No Raspberry-Pi-specific build filename
or preset is required.

### Cross-build for aarch64 Raspberry Pi

On the host machine install the aarch64 cross tools:

```bash
sudo apt update
sudo apt install \
    cmake make pkg-config \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

Create or sync a target sysroot containing the target development libraries
(SoapySDR, HDF5, spdlog, FFTW and dependencies), then:

```bash
export RPI_SYSROOT=/opt/sysroots/rpi-aarch64
./build.sh --cross
```

`build.sh` still asks for full vs recorder-only unless `--full` or
`--recorder-only` is supplied. When `RPI_SYSROOT` is set on a non-Pi host,
cross mode is selected automatically, so this is also valid:

```bash
export RPI_SYSROOT=/opt/sysroots/rpi-aarch64
./build.sh --recorder-only
```

Cross builds use the generic toolchain file
`cmake/toolchains/aarch64-linux.cmake`, disable host-native optimization, tune
for Cortex-A53, and disable the simulator plugin.

Build directories are generic:

```text
build-full/
build-recorder-only/
build-cross-full/
build-cross-recorder-only/
```

After a successful build, `build.sh` records the selected build directory in
`.meteoris-last-build`. This local marker is ignored by Git and lets
`install.sh` reuse the exact build instead of configuring and compiling a
second tree.

## Install

Meteoris provides two explicit install modes. By default they install the
recorder, viewer, `meteoris_config`, `meteoris_recover_hdf5`, **and the current `meteoris_sim`
SoapySDR plugin**. This avoids
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
~/.local/bin/meteoris_config
~/.local/bin/meteoris_recover_hdf5
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
/usr/local/bin/meteoris_config
/usr/local/bin/meteoris_recover_hdf5
/usr/local/share/meteoris/
```

A custom prefix is also supported:

```bash
./install.sh --local --prefix "$HOME/apps/meteoris"
```

Normally `install.sh` reuses the last successful `build.sh` directory and does not rebuild it.
Use `--rebuild` when you explicitly want another compile before installation.
To omit the simulator plugin from installation:

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
