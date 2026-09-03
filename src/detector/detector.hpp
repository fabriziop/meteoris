#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace meteoris {
namespace detector {

constexpr uint32_t DETECTOR_API_VERSION = 3;

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

    // Lightweight per-frame peak statistics used by the recorder for
    // cumulative counters and rate-limited warnings. Unlike DebugView, these
    // are always populated and require no diagnostic vector construction.
    uint64_t rawCandidates = 0;
    uint64_t closeSuppressed = 0;
    uint64_t droppedByLimit = 0;
    uint64_t retainedPeaks = 0;

    DebugView debug;
};

enum class ProcessingMode
{
    Full,
    PreprocessOnly
};

struct ProcessOptions
{
    // PreprocessOnly advances detector preprocessing/history but suppresses
    // association and tracking. It is used while the recorder is rearming.
    ProcessingMode mode = ProcessingMode::Full;

    // Building named metrics and per-track objects is optional because it is
    // appreciably more expensive than returning the detector decision.
    bool collectDebug = false;
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

    // Synchronous, zero-copy detector entry point. Processing mode and
    // structured-diagnostic collection are controlled independently.
    virtual Result process(
        const Frame &frame,
        const ProcessOptions &options = ProcessOptions()) = 0;
};

// Detector configuration is intentionally schema-driven: Meteoris stores raw
// TOML scalar values without knowing detector-specific parameter types. Each
// compile-time plugin owns its schema, defaults, parsing, and validation.
struct ConfigField
{
    ConfigField(const char *k, const char *d, const char *desc)
        : key(k), defaultValue(d), description(desc) {}
    const char *key;          // relative to detector., e.g. stationary.min_hz
    const char *defaultValue; // TOML scalar spelling
    const char *description;
};

class Config
{
public:
    void set(const std::string &key, const std::string &rawValue);
    bool has(const std::string &key) const;
    std::string raw(const std::string &key, const std::string &defaultValue) const;
    std::string stringValue(const std::string &key, const std::string &defaultValue) const;
    bool boolValue(const std::string &key, bool defaultValue) const;
    size_t sizeValue(const std::string &key, size_t defaultValue) const;
    double doubleValue(const std::string &key, double defaultValue) const;
    const std::map<std::string, std::string> &values() const { return _values; }
private:
    std::map<std::string, std::string> _values;
};

struct Selection
{
    std::string plugin = "peak_tracker";
    unsigned threads = 1;
    Config config;
};

struct Requirements
{
    // Minimum recorder pre-context required by this detector configuration.
    double minPreContextSeconds = 0.0;
};

std::vector<ConfigField> schema(const std::string &plugin);
std::vector<std::string> pluginNames();
Requirements requirements(const Selection &selection,
                          const Environment &environment);
void validate(const Selection &selection, const Environment &environment);
std::unique_ptr<IDetector> create(const Selection &selection,
                                  const Environment &environment);

} // namespace detector
} // namespace meteoris
