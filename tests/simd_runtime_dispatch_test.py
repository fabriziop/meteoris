#!/usr/bin/env python3
"""Static regression checks for portable x86 runtime SIMD dispatch."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    cmake = (ROOT / "CMakeLists.txt").read_text()
    main_cpp = (ROOT / "src" / "meteoris.cpp").read_text()
    dispatch = (ROOT / "src" / "dsp" / "simd_dot.cpp").read_text()
    avx2 = (ROOT / "src" / "dsp" / "simd_dot_avx2.cpp").read_text()

    assert 'METEORIS_X86_AVX2 "Build the optional x86/x64 AVX2 runtime-dispatch kernel" ON' in cmake
    assert 'src/dsp/simd_dot_avx2.cpp' in cmake
    assert 'COMPILE_OPTIONS "/arch:AVX2"' in cmake
    assert 'COMPILE_OPTIONS "-mavx2"' in cmake
    assert 'target_compile_options(meteoris PRIVATE /arch:AVX2)' not in cmake
    assert 'target_compile_options(meteoris PRIVATE -mavx2)' not in cmake
    assert '-mtune=native' in cmake

    assert 'cpuSupportsAvx2()' in dispatch
    assert '__builtin_cpu_supports("avx2")' in dispatch
    assert '_xgetbv(0)' in dispatch
    assert '"AVX2(runtime)"' in dispatch
    assert '#define METEORIS_HAVE_NEON 1' in dispatch
    assert '#if !defined(METEORIS_HAVE_NEON)' in dispatch
    assert '#if defined(METEORIS_HAVE_NEON)' in dispatch
    assert 'dotComplexAvx2' in avx2
    assert '_mm256_mul_ps' in avx2

    assert 'meteoris_simd::dotComplex' in main_cpp
    assert 'meteoris_simd::backendName()' in main_cpp
    assert '#if defined(__AVX2__)' not in main_cpp

    print("SIMD runtime dispatch structure test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
