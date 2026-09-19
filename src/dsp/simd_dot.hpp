#pragma once

#include <complex>
#include <cstddef>

namespace meteoris_simd
{
// Compute two real dot products sharing the same coefficient vector and return
// them as a complex value. On x86/x64 this dispatches once at runtime to AVX2
// when both the CPU and OS support it; otherwise it uses the portable scalar
// implementation. ARM builds use NEON when the compiler exposes it.
std::complex<float> dotComplex(const float *coefficients,
                               const float *realSamples,
                               const float *imagSamples,
                               std::size_t count);

// Human-readable name of the implementation selected for this process.
const char *backendName();
}
