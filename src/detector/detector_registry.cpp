#include "detector.hpp"

#include <stdexcept>

namespace meteoris {
namespace detector {

std::unique_ptr<IDetector> makePeakTrackerDetector(
    const PeakTrackerConfig &cfg,
    const Environment &environment);

std::unique_ptr<IDetector> create(
    const Selection &selection,
    const Environment &environment)
{
    if (selection.plugin == "peak_tracker")
        return makePeakTrackerDetector(selection.peakTracker, environment);

    throw std::runtime_error("unknown detector plugin: " + selection.plugin);
}


} // namespace detector
} // namespace meteoris
