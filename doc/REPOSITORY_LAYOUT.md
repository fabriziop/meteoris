# Repository Layout

```text
meteoris/
├── CMakeLists.txt
├── CMakePresets.json
├── build.sh
├── build_rpi3.sh
├── install.sh
├── run_sim.sh
├── cmake/
│   └── toolchains/
│       └── rpi3-trixie-aarch64.cmake
├── src/
│   ├── meteoris.cpp
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
│   └── echoes_automatic_detector_test.cpp
├── tools/
│   ├── meteoris_plot.py
│   └── meteoris_recover_hdf5.py
├── config/
│   ├── meteoris.toml
│   ├── meteoris_plot.toml
│   └── meteoris_sim.toml
├── doc/
│   ├── BUG20260902.md
│   ├── CHEATSHEET.md
│   ├── CONFIG_FILE_REFERENCE.md
│   ├── DETAILED_FEATURES.md
│   ├── DETECTOR.md
│   ├── DETECTOR_PLUGIN_API.md
│   ├── DSP_PIPELINE.md
│   ├── ECHOES_AUTOMATIC_DETECTOR.md
│   ├── INSTALL.md
│   ├── RECORDING_FORMAT.md
│   ├── RECOVER_HDF5.md
│   └── meteoris_event_*.png
├── README.md
└── LICENSE
```

The main executable is `src/meteoris.cpp`. Detector implementations are
separated behind the detector interface in `src/detector/` and registered at
compile time; the repository currently includes `peak_tracker` and
`echoes_automatic`. The FFT backend abstraction is in `src/fft/`, with the
embedded radix-2 implementation and optional FFTW support.

The SoapySDR test device is built from `sim/`, `tests/` contains detector tests,
and `tools/` contains the Python event viewer and HDF5 crash-recovery helper. Raspberry Pi native/cross-build
support is provided by `build_rpi3.sh`, `CMakePresets.json`, and the toolchain in
`cmake/toolchains/`. `run_sim.sh` runs against the freshly built in-tree
simulator plugin, and `install.sh` supports local or system installation.

Generated build directories, `.git/`, Python `__pycache__/` directories, and
other transient build/runtime files are intentionally omitted from the layout
above.
