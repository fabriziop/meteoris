#!/usr/bin/env python3
"""Static regression checks for the single-tree Windows/Linux target matrix."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    cmake = (ROOT / "CMakeLists.txt").read_text()
    source = (ROOT / "src" / "meteoris.cpp").read_text()

    assert "Windows ARM/ARM64 is not a supported Meteoris target" in cmake
    assert "METEORIS_X86_AVX2" in cmake
    assert "defined(_WIN32)" in source
    assert "GetDiskFreeSpaceExA" in source
    assert "GetComputerNameA" in source
    assert "runtime.daemon/--daemon is not supported on Windows" in source

    for name in ("meteoris_plot", "meteoris_config", "meteoris_recover_hdf5"):
        assert (ROOT / "tools" / f"{name}.cmd").is_file()

    assert (ROOT / "build.ps1").is_file()
    assert (ROOT / "install.ps1").is_file()
    print("Windows port structure test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
