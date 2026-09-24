#pragma once

#include <algorithm>
#include <cstdint>

namespace meteoris
{
namespace sim
{

inline bool cycleGeneratesSignals(const uint64_t cycle, const uint64_t cycleCount)
{
    return cycleCount == 0 || cycle < cycleCount;
}

inline double cycleAmplitude(const double configuredAmplitude,
                             const double amplitudeReduction,
                             const uint64_t cycle)
{
    return std::max(0.0, configuredAmplitude - double(cycle) * amplitudeReduction);
}

} // namespace sim
} // namespace meteoris
