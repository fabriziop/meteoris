# Repository and Package Layout

## Repository layout

```text
meteoris/
├── .git/                         # Git metadata and complete repository history
├── VERSION                       # Single authoritative package version
├── CMakeLists.txt                # Shared build definition for all supported targets
├── build.sh                      # Linux x86/x86_64 and Linux ARM64 build entry point
├── install.sh                    # Linux installer
├── build.ps1                     # Windows x86/x64 build entry point
├── install.ps1                   # Windows installer
├── run_sim.sh                    # Run against the in-tree SoapySDR simulator
├── cmake/
│   └── toolchains/
│       └── aarch64-linux.cmake   # Linux ARM64 cross-build toolchain
├── src/
│   ├── meteoris.cpp              # Main recorder executable
│   ├── version.hpp.in
│   ├── detector/
│   │   ├── detector.hpp
│   │   ├── detector_registry.cpp
│   │   ├── echoes_automatic_detector.cpp
│   │   └── peak_tracker_detector.cpp
│   ├── dsp/
│   │   ├── simd_dot.hpp          # SIMD dispatch interface
│   │   ├── simd_dot.cpp          # Scalar/NEON implementation and runtime dispatch
│   │   └── simd_dot_avx2.cpp     # Isolated x86/x64 AVX2 implementation
│   └── fft/
│       ├── fft_backend.cpp
│       └── fft_backend.hpp
├── sim/
│   ├── CMakeLists.txt
│   └── SoapyMeteorisSim.cpp      # SoapySDR simulator module
├── tools/
│   ├── meteoris_plot.py
│   ├── meteoris_plot.cmd         # Windows launcher
│   ├── meteoris_config.py
│   ├── meteoris_config.cmd       # Windows launcher
│   ├── meteoris_recover_hdf5.py
│   └── meteoris_recover_hdf5.cmd # Windows launcher
├── tests/
│   ├── echoes_automatic_detector_test.cpp
│   ├── simd_runtime_dispatch_test.py
│   ├── windows_port_structure_test.py
│   ├── build_install_workflow_test.py
│   ├── meteoris_config_test.py
│   ├── meteoris_plot_widget_reuse_test.py
│   ├── meteoris_recover_hdf5_cli_test.py
│   └── version_consistency_test.py
├── config/
│   ├── meteoris.toml
│   ├── meteoris_plot.toml
│   └── meteoris_sim.toml
├── doc/
│   ├── INSTALL.md
│   ├── REPOSITORY_LAYOUT.md
│   ├── VERSIONING.md
│   └── ...
├── README.md
└── LICENSE
```

`.git/` is part of the authoritative development repository and must be kept
when the working repository is copied or archived for continued development.
It is not part of an end-user binary installation package.

Generated build directories, `.pytest_cache/`, Python `__pycache__/` folders,
`.meteoris-last-build*` state files, and other transient build/runtime files are
not part of the reference source layout.

`VERSION` is the single authoritative package version. CMake derives the
project version from it, generates the C++ version header, and installs the
version data required by the standalone Python commands. See
`doc/VERSIONING.md`.

The repository remains the single reference source for every supported target:

- Linux x86/x86_64: `build.sh` / `install.sh`
- Linux ARM64: `build.sh` / `install.sh`
- Windows x86/x64: `build.ps1` / `install.ps1`

Windows ARM/ARM64 is intentionally not part of the supported build matrix.

On x86/x64, the executable keeps a baseline ISA while the AVX2 FIR kernel is
compiled separately and selected at runtime only when CPU and OS support are
available. Linux ARM64 uses its NEON path. This keeps the source tree and
package layout common across the supported targets without requiring separate
platform forks.

## Installed package layout

The install prefix changes by platform and install mode, but CMake installs the
same logical Meteoris package beneath that prefix.

### Linux

Default prefixes:

```text
Local:   ~/.local
System:  /usr/local
```

The installed layout is conceptually:

```text
<PREFIX>/
├── bin/
│   ├── meteoris
│   ├── meteoris_plot             # full build only
│   ├── meteoris_config
│   └── meteoris_recover_hdf5
├── share/
│   └── meteoris/
│       └── ...                    # installed Meteoris data/configuration
└── ...                            # simulator/plugin files when selected
```

`install.sh --local` uses `~/.local` unless `--prefix` overrides it.
`install.sh --system` uses `/usr/local` unless `--prefix` overrides it.

### Windows x86/x64

Default prefixes:

```text
Local:   %LOCALAPPDATA%\Meteoris
System:  %ProgramFiles%\Meteoris
```

The installed layout is conceptually:

```text
<PREFIX>\
├── bin\
│   ├── meteoris.exe
│   ├── meteoris_plot.cmd          # full build only
│   ├── meteoris_plot.py           # full build only
│   ├── meteoris_config.cmd
│   ├── meteoris_config.py
│   ├── meteoris_recover_hdf5.cmd
│   └── meteoris_recover_hdf5.py
├── share\
│   └── meteoris\
│       └── ...                    # installed Meteoris data/configuration
└── ...                            # simulator/plugin files when selected
```

`install.ps1 -Local` uses `%LOCALAPPDATA%\Meteoris` unless `-Prefix` overrides
it. `install.ps1 -System` uses `%ProgramFiles%\Meteoris` unless `-Prefix`
overrides it.

The Windows installer adds `<PREFIX>\bin` to `PATH`: the user PATH for a local
install and the machine PATH for a system install. Therefore the installed
recorder can be invoked simply as:

```powershell
meteoris
```

The `.cmd` files are Windows command launchers for the Python helper programs;
the corresponding `.py` files contain the shared implementations.

## Build output versus installed package

Build directories such as `build-*` and `build-win-*` are intermediate build
trees and are not the distribution layout. End-user packages should be staged
from the CMake install result so that executables, helper commands, shared data,
plugins, and runtime dependencies are collected under one installation prefix.

The development repository, including `.git/`, remains the reference source
from which all target-specific build and install packages are produced.
