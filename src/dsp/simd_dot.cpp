#include "dsp/simd_dot.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#if defined(METEORIS_HAVE_AVX2_KERNEL) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#endif

#include <cstdint>

namespace meteoris_simd
{
#if defined(METEORIS_HAVE_AVX2_KERNEL)
std::complex<float> dotComplexAvx2(const float *h, const float *re,
                                   const float *im, std::size_t n);
#endif

namespace
{
using DotFn = std::complex<float> (*)(const float *, const float *, const float *, std::size_t);

std::complex<float> dotComplexScalar(const float *h, const float *re,
                                     const float *im, const std::size_t n)
{
    float accRe = 0.0f;
    float accIm = 0.0f;
    for (std::size_t k = 0; k < n; ++k)
    {
        const float hk = h[k];
        accRe += re[k] * hk;
        accIm += im[k] * hk;
    }
    return {accRe, accIm};
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
std::complex<float> dotComplexNeon(const float *h, const float *re,
                                   const float *im, const std::size_t n)
{
    std::size_t k = 0;
    float accRe = 0.0f;
    float accIm = 0.0f;
    float32x4_t vr = vdupq_n_f32(0.0f);
    float32x4_t vi = vdupq_n_f32(0.0f);
    for (; k + 4 <= n; k += 4)
    {
        const float32x4_t vh = vld1q_f32(h + k);
        vr = vmlaq_f32(vr, vld1q_f32(re + k), vh);
        vi = vmlaq_f32(vi, vld1q_f32(im + k), vh);
    }
#if defined(__aarch64__)
    accRe += vaddvq_f32(vr);
    accIm += vaddvq_f32(vi);
#else
    float32x2_t sr = vadd_f32(vget_low_f32(vr), vget_high_f32(vr));
    float32x2_t si = vadd_f32(vget_low_f32(vi), vget_high_f32(vi));
    sr = vpadd_f32(sr, sr);
    si = vpadd_f32(si, si);
    accRe += vget_lane_f32(sr, 0);
    accIm += vget_lane_f32(si, 0);
#endif
    for (; k < n; ++k)
    {
        const float hk = h[k];
        accRe += re[k] * hk;
        accIm += im[k] * hk;
    }
    return {accRe, accIm};
}
#endif

#if defined(METEORIS_HAVE_AVX2_KERNEL)
bool cpuSupportsAvx2()
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;

    __cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;

    const unsigned __int64 xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6u) != 0x6u) return false; // XMM + YMM state enabled by OS.

    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && \
      (defined(__i386__) || defined(__x86_64__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
}
#endif

struct Backend
{
    DotFn fn;
    const char *name;
};

Backend chooseBackend()
{
#if defined(METEORIS_HAVE_AVX2_KERNEL)
    if (cpuSupportsAvx2()) return {&dotComplexAvx2, "AVX2(runtime)"};
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    return {&dotComplexNeon, "NEON"};
#else
    return {&dotComplexScalar, "scalar"};
#endif
}

const Backend &selectedBackend()
{
    // C++11 guarantees thread-safe initialization of function-local statics.
    static const Backend backend = chooseBackend();
    return backend;
}
} // namespace

std::complex<float> dotComplex(const float *coefficients,
                               const float *realSamples,
                               const float *imagSamples,
                               const std::size_t count)
{
    return selectedBackend().fn(coefficients, realSamples, imagSamples, count);
}

const char *backendName()
{
    return selectedBackend().name;
}
} // namespace meteoris_simd
