#include "dsp/simd_dot.hpp"

#include <immintrin.h>

namespace meteoris_simd
{
std::complex<float> dotComplexAvx2(const float *h, const float *re,
                                   const float *im, const std::size_t n)
{
    std::size_t k = 0;
    __m256 vr = _mm256_setzero_ps();
    __m256 vi = _mm256_setzero_ps();
    for (; k + 8 <= n; k += 8)
    {
        const __m256 vh = _mm256_loadu_ps(h + k);
        const __m256 xr = _mm256_loadu_ps(re + k);
        const __m256 xi = _mm256_loadu_ps(im + k);
        vr = _mm256_add_ps(vr, _mm256_mul_ps(xr, vh));
        vi = _mm256_add_ps(vi, _mm256_mul_ps(xi, vh));
    }

    alignas(32) float sumRe[8];
    alignas(32) float sumIm[8];
    _mm256_store_ps(sumRe, vr);
    _mm256_store_ps(sumIm, vi);

    float accRe = 0.0f;
    float accIm = 0.0f;
    for (std::size_t lane = 0; lane < 8; ++lane)
    {
        accRe += sumRe[lane];
        accIm += sumIm[lane];
    }
    for (; k < n; ++k)
    {
        const float hk = h[k];
        accRe += re[k] * hk;
        accIm += im[k] * hk;
    }
    return {accRe, accIm};
}
} // namespace meteoris_simd
