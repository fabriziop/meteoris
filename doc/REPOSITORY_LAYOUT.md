# Repository Layout

```text
meteoris/
├── VERSION
├── CMakeLists.txt
├── build.sh
├── install.sh
├── run_sim.sh
├── cmake/
│   └── toolchains/
│       └── aarch64-linux.cmake
├── src/
│   ├── meteoris.cpp
│   ├── version.hpp.in
│   ├── detector/
│   │   ├── detector.hpp
│   │   ├── detector_registry.cpp
│   │   ├── echoes_automatic_detector.cpp
│   │   └── peak_tracker_detector.cpp
│   └── fft/
│       ├── fft_backend.cpp
│       └── fft_backend.hpp
├── sim/
│   ├── CMakeLists.txt
│   └── SoapyMeteorisSim.cpp
├── tests/
│   ├── echoes_automatic_detector_test.cpp
│   ├── meteoris_config_test.py
│   └── version_consistency_test.py
├── tools/
│   ├── meteoris_plot.py
│   ├── meteoris_config.py
│   └── meteoris_recover_hdf5.py
├── config/
│   ├── meteoris.toml
│   ├── meteoris_plot.toml
│   └── meteoris_sim.toml
├── doc/
│   ├── BUG20260902.md
│   ├── CHEATSHEET.md
│   ├── CONFIG_FILE_REFERENCE.md
│   ├── CONFIG_WIZARD.md
│   ├── DETAILED_FEATURES.md
│   ├── DETECTOR.md
│   ├── DETECTOR_PLUGIN_API.md
│   ├── DSP_PIPELINE.md
│   ├── ECHOES_AUTOMATIC_DETECTOR.md
│   ├── INSTALL.md
│   ├── RECORDING_FORMAT.md
│   ├── RECOVER_HDF5.md
│   ├── VERSIONING.md
│   └── meteoris_event_*.png
├── README.md
└── LICENSE
```


`VERSION` is the single authoritative package version. CMake derives
`PROJECT_VERSION` from it, generates the C++ version header, and installs the
file for the standalone Python commands. See `doc/VERSIONING.md`.

The main executable is `src/meteoris.cpp`. Detector implementations are
separated behind the detector interface in `src/detector/` and registered at
compile time; the repository currently includes `peak_tracker` and
`echoes_automatic`. The FFT backend abstraction is in `src/fft/`, with the
embedded radix-2 implementation and optional FFTW support.

The SoapySDR test device is built from `sim/`, `tests/` contains detector tests,
and `tools/` contains the Python event viewer and HDF5 crash-recovery helper. Native and aarch64 cross-build support is provided by the single `build.sh`
entry point and the generic toolchain in `cmake/toolchains/`. `run_sim.sh` runs against the freshly built in-tree
simulator plugin, and `install.sh` supports local or system installation.

Generated build directories, `.git/`, Python `__pycache__/` directories, and
other transient build/runtime files are intentionally omitted from the layout
above.

## Platform build entry points

The repository remains the single reference source for every supported target.
Linux x86/x86_64 and Linux ARM64 use `build.sh` / `install.sh`; Windows x86/x64
uses `build.ps1` / `install.ps1`. Windows helper commands use the `.cmd`
launchers in `tools/`. Windows ARM/ARM64 is intentionally not part of the build
matrix.
