#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace meteoris {
namespace detector {

constexpr uint32_t DETECTOR_API_VERSION = 1;

enum class State
{
    WarmingUp,
    Idle,
    Active
};

enum class ObjectType
{
    Unknown,
    Stationary,
    Chirp
};

struct Frame
{
    uint64_t timestampNs = 0;
    uint64_t frameIndex = 0;

    // Borrowed PSD memory. Valid only for the duration of IDetector::process().
    const float *psd = nullptr;
    size_t bins = 0;

    double frequencyStartHz = 0.0;
    double frequencyStepHz = 0.0;
    double framePeriodSeconds = 0.0;
};

struct Metric
{
    const char *name = nullptr;
    double value = 0.0;
    const char *unit = nullptr;
};

struct Object
{
    uint64_t id = 0;
    ObjectType type = ObjectType::Unknown;
    bool active = false;

    double frequencyHz = 0.0;
    double frequencyRateHzS = 0.0;
    double strengthDbHz = -300.0;
    double excessDb = 0.0;

    double ageSeconds = 0.0;
    double occupancy = 0.0;
    uint64_t hits = 0;
    uint64_t opportunities = 0;
    double missedSeconds = 0.0;
};

// Borrowed diagnostic views. They remain valid until the next process()/reset()
// call on the same detector. This permits zero-copy use by logging, HDF5, and
// future Meteoris diagnostic/replay tools.
struct DebugView
{
    const Metric *metrics = nullptr;
    size_t metricCount = 0;

    const Object *objects = nullptr;
    size_t objectCount = 0;
};

struct Result
{
    State state = State::WarmingUp;
    uint32_t activeObjects = 0;

    // Common recorder metric retained for HDF5 meteoris-v0 compatibility.
    float frameMaxDbHz = -300.0f;

    DebugView debug;
};

struct Environment
{
    size_t bins = 0;
    double frequencyStartHz = 0.0;
    double frequencyStepHz = 0.0;
    double framePeriodSeconds = 0.0;

    // Phase 1 is synchronous. A detector may internally use these resources,
    // but all frame work must complete before process() returns.
    unsigned requestedThreads = 1; // 0 = detector decides
    unsigned hardwareThreads = 1;
};

struct Info
{
    uint32_t apiVersion = DETECTOR_API_VERSION;
    const char *name = nullptr;
    const char *version = nullptr;
    bool supportsStructuredDebug = true;
    bool mayUseMultipleThreads = false;
};

class IDetector
{
public:
    virtual ~IDetector() {}

    virtual const Info &info() const = 0;

    // Reset detector temporal/track state. Implementations may retain allocated
    // buffers so reset remains inexpensive.
    virtual void reset() = 0;

    // Synchronous, zero-copy detector entry point.
    virtual Result process(const Frame &frame) = 0;
};

// Current built-in plugin configuration. Phase 1 keeps the existing TOML
// parameter names for compatibility while moving ownership of algorithm logic
// out of meteoris.cpp.
struct PeakTrackerConfig
{
    size_t frequencyMeanBins = 5;
    size_t timeMeanPsds = 3;
    double peakThresholdDb = 5.0;
    double minPeakSeparationHz = 300.0;
    size_t maxPeaksPerPsd = 20;

    bool stationaryEnabled = true;
    double stationaryMinHz = -5000.0;
    double stationaryMaxHz = 5000.0;
    double stationaryMaxDfHz = 250.0;
    double stationaryActivationSeconds = 0.5;
    double stationaryActivationFraction = 0.50;
    double stationaryLostSeconds = 0.30;
    double stationaryMaxDriftHzS = 1000.0;

    bool chirpEnabled = true;
    double chirpMinHz = -50000.0;
    double chirpMaxHz = 50000.0;
    double chirpMaxDfHz = 1500.0;
    double chirpActivationSeconds = 0.05;
    double chirpActivationFraction = 0.70;
    double chirpLostSeconds = 0.08;
    double chirpMinDriftHzS = 3000.0;
    double chirpMaxDriftHzS = 150000.0;
};

struct Selection
{
    std::string plugin = "peak_tracker";
    unsigned threads = 1;
    PeakTrackerConfig peakTracker;
};

std::unique_ptr<IDetector> create(const Selection &selection,
                                  const Environment &environment);

} // namespace detector
} // namespace meteoris
