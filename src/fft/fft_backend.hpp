#pragma once

#include <complex>
#include <vector>
#include <memory>

/**
 * Abstract interface for FFT implementations.
 * Allows switching between internal Radix2 FFT and external FFTW library.
 */
class FftBackend
{
public:
    virtual ~FftBackend() = default;

    /**
     * Execute in-place complex FFT on the provided buffer.
     * @param buffer Input/output buffer of complex<float> samples
     */
    virtual void execute(std::vector<std::complex<float>> &buffer) const = 0;

    /**
     * Get the FFT size.
     * @return FFT length (must be power of two)
     */
    virtual size_t getSize() const = 0;
};

/**
 * Factory function to create an FFT backend.
 * Automatically selects FFTW if available and enabled, otherwise uses internal Radix2 FFT.
 *
 * @param size FFT length (must be power of two)
 * @return Shared pointer to FftBackend implementation
 */
std::shared_ptr<FftBackend> createFftBackend(size_t size);

/**
 * Get the name of the currently active FFT backend.
 * @return Backend name ("fftw3f" or "radix2")
 */
const char *getFftBackendName();
