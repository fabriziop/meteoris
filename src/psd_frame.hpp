#pragma once

#include <cstdint>
#include <complex>
#include <memory>
#include <vector>

struct PsdFrame
{
    uint64_t frameIndex = 0;
    uint64_t centerSampleIndex = 0;
    uint64_t timestampNs = 0;
    std::vector<std::complex<float>> iq; // unwindowed final-decimator I/Q, one FFT window
    std::vector<float> powerDensity; // W/Hz-like normalized PSD density
    float gainDb = 0.0f;             // SDR gain applied while this PSD was acquired
};

using PsdFramePtr = std::shared_ptr<const PsdFrame>;
