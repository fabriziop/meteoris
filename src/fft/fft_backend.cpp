#include "fft_backend.hpp"
#include <stdexcept>
#include <cmath>

#ifdef METEORIS_USE_FFTW
    #include <fftw3.h>
#endif

// Constants
constexpr double PI = 3.14159265358979323846;

// ============================================================================
// Internal Radix-2 FFT Implementation
// ============================================================================

class Radix2Fft : public FftBackend
{
public:
    explicit Radix2Fft(const size_t n) : _n(n), _bitrev(n), _twiddle(n / 2)
    {
        if (_n == 0 || (_n & (_n - 1)) != 0)
            throw std::runtime_error("FFT length must be a power of two");

        // Precompute bit-reversal permutation
        unsigned bits = 0;
        for (size_t t = _n; t > 1; t >>= 1) ++bits;
        for (size_t i = 0; i < _n; ++i)
        {
            size_t x = i, r = 0;
            for (unsigned b = 0; b < bits; ++b)
            {
                r = (r << 1) | (x & 1u);
                x >>= 1;
            }
            _bitrev[i] = r;
        }

        // Precompute twiddle factors (complex exponentials)
        for (size_t k = 0; k < _n / 2; ++k)
        {
            const double a = -2.0 * PI * double(k) / double(_n);
            _twiddle[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
    }

    void execute(std::vector<std::complex<float>> &a) const override
    {
        // Bit-reversal permutation
        for (size_t i = 0; i < _n; ++i)
        {
            const size_t j = _bitrev[i];
            if (i < j) std::swap(a[i], a[j]);
        }

        // Cooley-Tukey decimation-in-time FFT
        for (size_t len = 2; len <= _n; len <<= 1)
        {
            const size_t half = len >> 1;
            const size_t step = _n / len;
            for (size_t i = 0; i < _n; i += len)
            {
                for (size_t j = 0; j < half; ++j)
                {
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * _twiddle[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

    size_t getSize() const override { return _n; }

private:
    size_t _n;
    std::vector<size_t> _bitrev;
    std::vector<std::complex<float>> _twiddle;
};

// ============================================================================
// FFTW Backend Implementation (if available)
// ============================================================================

#ifdef METEORIS_USE_FFTW

class FftwBackend : public FftBackend
{
public:
    explicit FftwBackend(const size_t n) : _n(n)
    {
        if (_n == 0 || (_n & (_n - 1)) != 0)
            throw std::runtime_error("FFT length must be a power of two");

        // Allocate aligned memory as required by FFTW
        _buffer = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * _n);
        if (!_buffer)
            throw std::runtime_error("Failed to allocate FFTW buffer");

        // Create plan for in-place complex FFT (single precision)
        _plan = fftwf_plan_dft_1d(static_cast<int>(_n), _buffer, _buffer,
                                  FFTW_FORWARD, FFTW_ESTIMATE);
        if (!_plan)
        {
            fftwf_free(_buffer);
            throw std::runtime_error("Failed to create FFTW plan");
        }
    }

    ~FftwBackend()
    {
        if (_plan) fftwf_destroy_plan(_plan);
        if (_buffer) fftwf_free(_buffer);
    }

    // Prevent copying
    FftwBackend(const FftwBackend &) = delete;
    FftwBackend &operator=(const FftwBackend &) = delete;

    void execute(std::vector<std::complex<float>> &a) const override
    {
        if (a.size() != _n)
            throw std::runtime_error("Buffer size mismatch for FFTW execution");

        // Copy input to FFTW buffer
        std::copy(a.begin(), a.end(), reinterpret_cast<std::complex<float> *>(_buffer));

        // Execute plan (operates in-place)
        fftwf_execute(_plan);

        // Copy result back
        std::copy(reinterpret_cast<std::complex<float> *>(_buffer), 
                  reinterpret_cast<std::complex<float> *>(_buffer) + _n,
                  a.begin());
    }

    size_t getSize() const override { return _n; }

private:
    size_t _n;
    fftwf_complex *_buffer = nullptr;
    fftwf_plan _plan = nullptr;
};

#endif  // METEORIS_USE_FFTW

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<FftBackend> createFftBackend(size_t size)
{
#ifdef METEORIS_USE_FFTW
    try
    {
        return std::make_shared<FftwBackend>(size);
    }
    catch (const std::exception &e)
    {
        // Fall back to internal FFT if FFTW fails
        return std::make_shared<Radix2Fft>(size);
    }
#else
    return std::make_shared<Radix2Fft>(size);
#endif
}

const char *getFftBackendName()
{
#ifdef METEORIS_USE_FFTW
    return "fftw3f";
#else
    return "radix2";
#endif
}
