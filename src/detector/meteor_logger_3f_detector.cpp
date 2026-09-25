#include "detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <vector>

namespace meteoris {
namespace detector {
namespace {

constexpr double MIN_POWER = 1e-30;

struct MeteorLogger3fConfig
{
    double detectionMinHz = -49000.0;
    double detectionMaxHz = 49000.0;
    double clusterWidthHz = 250.0;
    size_t sequenceFrames = 6;
    size_t allowedGaps = 1;
    double maxDriftHzS = 11000.0;
    double releaseSeconds = 0.15;
    bool autoNotchEnabled = true;
    double autoNotchAfterSeconds = 10.0;
    double autoNotchWidthHz = 200.0;
    size_t maxAutoNotches = 4;
};

MeteorLogger3fConfig parseConfig(const Config &c)
{
    MeteorLogger3fConfig x;
    x.detectionMinHz = c.doubleValue("meteor_logger.detection_min_hz", x.detectionMinHz);
    x.detectionMaxHz = c.doubleValue("meteor_logger.detection_max_hz", x.detectionMaxHz);
    x.clusterWidthHz = c.doubleValue("meteor_logger.cluster_width_hz", x.clusterWidthHz);
    x.sequenceFrames = c.sizeValue("meteor_logger.sequence_frames", x.sequenceFrames);
    x.allowedGaps = c.sizeValue("meteor_logger.allowed_gaps", x.allowedGaps);
    x.maxDriftHzS = c.doubleValue("meteor_logger.max_drift_hz_s", x.maxDriftHzS);
    x.releaseSeconds = c.doubleValue("meteor_logger.release_s", x.releaseSeconds);
    x.autoNotchEnabled = c.boolValue("meteor_logger.auto_notch_enabled", x.autoNotchEnabled);
    x.autoNotchAfterSeconds = c.doubleValue("meteor_logger.auto_notch_after_s", x.autoNotchAfterSeconds);
    x.autoNotchWidthHz = c.doubleValue("meteor_logger.auto_notch_width_hz", x.autoNotchWidthHz);
    x.maxAutoNotches = c.sizeValue("meteor_logger.max_auto_notches", x.maxAutoNotches);
    return x;
}

struct Observation
{
    bool hit = false;
    uint64_t timestampNs = 0;
    double frequencyHz = 0.0;
};

struct Notch
{
    double frequencyHz = 0.0;
};

struct Scan
{
    bool clustered = false;
    double peakFrequencyHz = 0.0;
    double peakDbHz = -300.0;
    double backgroundDbHz = -300.0;
    double excessDb = 0.0;
    double threeFrequencySpanHz = 0.0;
    float frameMaxDbHz = -300.0f;
};

class MeteorLogger3fDetector final : public IDetector
{
public:
    MeteorLogger3fDetector(const MeteorLogger3fConfig &cfg, const Environment &environment)
        : _cfg(cfg), _environment(environment)
    {
        selectDetectionBins();
        _info.apiVersion = DETECTOR_API_VERSION;
        _info.name = "meteor_logger_3f";
        _info.version = "1";
        _info.supportsStructuredDebug = true;
        _info.mayUseMultipleThreads = false;
    }

    const Info &info() const override { return _info; }

    void reset() override
    {
        clearEventState();
        _sequence.clear();
        _metrics.clear();
        _objects.clear();
        // Learned persistent-line notches intentionally survive recorder
        // rearm resets, just as an RF interferer survives an event boundary.
    }

    Result process(const Frame &frame, const ProcessOptions &options) override
    {
        if (frame.psd == nullptr || frame.bins != _environment.bins)
            throw std::runtime_error("meteor_logger_3f: PSD geometry changed");

        const Scan scan = measure(frame);
        updateAutoNotch(scan, frame.timestampNs);
        _metrics.clear();
        _objects.clear();

        if (options.mode == ProcessingMode::PreprocessOnly)
        {
            clearEventState();
            _sequence.clear();
            return result(scan, State::Idle, options.collectDebug);
        }

        appendSequence(scan, frame.timestampNs);
        updateActiveState(scan, frame.timestampNs);

        if (!_active && sequenceQualifies())
            activateFromSequence(scan, frame.timestampNs);

        return result(scan, _active ? State::Active : State::Idle,
                      options.collectDebug);
    }

private:
    struct RankedBin
    {
        RankedBin() {}
        RankedBin(const float p, const size_t i) : power(p), index(i) {}
        float power = -std::numeric_limits<float>::infinity();
        size_t index = 0;
    };

    static double secondsBetween(const uint64_t newer, const uint64_t older)
    {
        return newer > older ? double(newer - older) * 1e-9 : 0.0;
    }

    void selectDetectionBins()
    {
        _firstBin = _environment.bins;
        _lastBin = 0;
        for (size_t k = 0; k < _environment.bins; ++k)
        {
            const double f = frequency(k);
            if (f >= _cfg.detectionMinHz && f <= _cfg.detectionMaxHz)
            {
                _firstBin = std::min(_firstBin, k);
                _lastBin = std::max(_lastBin, k + 1);
            }
        }
        if (_firstBin >= _lastBin || _lastBin - _firstBin < 3)
            throw std::runtime_error("meteor_logger_3f: detection interval has fewer than three PSD bins");
    }

    double frequency(const size_t bin) const
    {
        return _environment.frequencyStartHz + double(bin) * _environment.frequencyStepHz;
    }

    bool notched(const double frequencyHz) const
    {
        const double halfWidth = 0.5 * _cfg.autoNotchWidthHz;
        for (size_t i = 0; i < _notches.size(); ++i)
            if (std::abs(frequencyHz - _notches[i].frequencyHz) <= halfWidth)
                return true;
        return false;
    }

    Scan measure(const Frame &frame)
    {
        Scan out;
        float rawMax = 1e-30f;
        for (size_t k = 0; k < frame.bins; ++k)
            rawMax = std::max(rawMax, frame.psd[k]);
        out.frameMaxDbHz = static_cast<float>(10.0 * std::log10(rawMax));

        RankedBin top[3];
        std::vector<float> powers;
        powers.reserve(_lastBin - _firstBin);
        for (size_t k = _firstBin; k < _lastBin; ++k)
        {
            if (notched(frequency(k))) continue;
            const float p = std::max(frame.psd[k], 1e-30f);
            powers.push_back(p);
            if (p > top[0].power)
            {
                top[2] = top[1]; top[1] = top[0]; top[0] = RankedBin{p, k};
            }
            else if (p > top[1].power)
            {
                top[2] = top[1]; top[1] = RankedBin{p, k};
            }
            else if (p > top[2].power)
            {
                top[2] = RankedBin{p, k};
            }
        }
        if (powers.size() < 3) return out;

        const size_t middle = powers.size() / 2;
        std::nth_element(powers.begin(), powers.begin() + middle, powers.end());
        const double background = std::max<double>(powers[middle], MIN_POWER);
        out.backgroundDbHz = 10.0 * std::log10(background);
        out.peakDbHz = 10.0 * std::log10(std::max<double>(top[0].power, MIN_POWER));
        out.excessDb = out.peakDbHz - out.backgroundDbHz;
        out.peakFrequencyHz = frequency(top[0].index);

        double lo = frequency(top[0].index);
        double hi = lo;
        for (size_t i = 1; i < 3; ++i)
        {
            lo = std::min(lo, frequency(top[i].index));
            hi = std::max(hi, frequency(top[i].index));
        }
        out.threeFrequencySpanHz = hi - lo;

        // A mathematically flat PSD has no signature. This tie guard avoids a
        // deterministic first-three-bin cluster without adding an amplitude
        // threshold to the 3f method.
        const bool nonFlat = top[0].power > top[2].power * (1.0f + 1e-6f);
        out.clustered = nonFlat && out.threeFrequencySpanHz <= _cfg.clusterWidthHz;
        return out;
    }

    bool compatible(const double frequencyHz, const uint64_t timestampNs,
                    const double previousFrequencyHz, const uint64_t previousTimestampNs) const
    {
        const double dt = std::max(_environment.framePeriodSeconds,
                                   secondsBetween(timestampNs, previousTimestampNs));
        return std::abs(frequencyHz - previousFrequencyHz) <= _cfg.maxDriftHzS * dt;
    }

    void appendSequence(const Scan &scan, const uint64_t timestampNs)
    {
        Observation observation;
        observation.hit = scan.clustered;
        observation.timestampNs = timestampNs;
        observation.frequencyHz = scan.peakFrequencyHz;

        if (observation.hit)
        {
            for (size_t i = _sequence.size(); i-- > 0;)
            {
                if (!_sequence[i].hit) continue;
                if (!compatible(observation.frequencyHz, timestampNs,
                                _sequence[i].frequencyHz, _sequence[i].timestampNs))
                    _sequence.clear();
                break;
            }
        }

        _sequence.push_back(observation);
        while (_sequence.size() > _cfg.sequenceFrames) _sequence.pop_front();
    }

    bool sequenceQualifies() const
    {
        if (_sequence.size() != _cfg.sequenceFrames) return false;
        size_t gaps = 0;
        for (size_t i = 0; i < _sequence.size(); ++i)
            if (!_sequence[i].hit) ++gaps;
        return gaps <= _cfg.allowedGaps;
    }

    void updateActiveState(const Scan &scan, const uint64_t timestampNs)
    {
        if (!_active) return;
        ++_activeOpportunities;
        if (scan.clustered && compatible(scan.peakFrequencyHz, timestampNs,
                                         _activeFrequencyHz, _lastActiveHitNs))
        {
            const double dt = secondsBetween(timestampNs, _lastActiveHitNs);
            if (dt > 0.0)
                _activeRateHzS = (scan.peakFrequencyHz - _activeFrequencyHz) / dt;
            _activeFrequencyHz = scan.peakFrequencyHz;
            _activeStrengthDbHz = scan.peakDbHz;
            _activeExcessDb = scan.excessDb;
            _lastActiveHitNs = timestampNs;
            ++_activeHits;
        }
        if (secondsBetween(timestampNs, _lastActiveHitNs) > _cfg.releaseSeconds)
            clearEventState();
    }

    void activateFromSequence(const Scan &scan, const uint64_t timestampNs)
    {
        const Observation *lastHit = nullptr;
        size_t hits = 0;
        for (size_t i = 0; i < _sequence.size(); ++i)
        {
            if (_sequence[i].hit) { lastHit = &_sequence[i]; ++hits; }
        }
        if (lastHit == nullptr) return;
        _active = true;
        _activeId = ++_nextObjectId;
        _activeStartNs = _sequence.front().timestampNs;
        _lastActiveHitNs = lastHit->timestampNs;
        _activeFrequencyHz = lastHit->frequencyHz;
        _activeRateHzS = 0.0;
        _activeStrengthDbHz = scan.peakDbHz;
        _activeExcessDb = scan.excessDb;
        _activeHits = hits;
        _activeOpportunities = _sequence.size();
        (void)timestampNs;
    }

    void updateAutoNotch(const Scan &scan, const uint64_t timestampNs)
    {
        if (!_cfg.autoNotchEnabled || !scan.clustered ||
            _notches.size() >= _cfg.maxAutoNotches)
        {
            if (!scan.clustered)
            {
                _notchRunActive = false;
                _notchRunStartNs = 0;
                _notchRunSamples = 0;
            }
            return;
        }

        const double tolerance = 0.5 * _cfg.autoNotchWidthHz;
        if (!_notchRunActive ||
            std::abs(scan.peakFrequencyHz - _notchRunFrequencyHz) > tolerance)
        {
            _notchRunActive = true;
            _notchRunStartNs = timestampNs;
            _notchRunFrequencyHz = scan.peakFrequencyHz;
            _notchRunSamples = 1;
            return;
        }

        ++_notchRunSamples;
        _notchRunFrequencyHz +=
            (scan.peakFrequencyHz - _notchRunFrequencyHz) / double(_notchRunSamples);
        if (secondsBetween(timestampNs, _notchRunStartNs) >= _cfg.autoNotchAfterSeconds)
        {
            Notch notch;
            notch.frequencyHz = _notchRunFrequencyHz;
            _notches.push_back(notch);
            _notchRunActive = false;
            _notchRunStartNs = 0;
            _notchRunSamples = 0;
        }
    }

    void clearEventState()
    {
        _active = false;
        _activeId = 0;
        _activeStartNs = 0;
        _lastActiveHitNs = 0;
        _activeRateHzS = 0.0;
        _activeHits = 0;
        _activeOpportunities = 0;
    }

    Result result(const Scan &scan, const State state, const bool collectDebug)
    {
        Result out;
        out.state = state;
        out.activeObjects = _active ? 1u : 0u;
        out.frameMaxDbHz = scan.frameMaxDbHz;
        out.rawCandidates = 3;
        out.retainedPeaks = scan.clustered ? 1 : 0;

        if (collectDebug)
        {
            addMetric("three_frequency_span_hz", scan.threeFrequencySpanHz, "Hz");
            addMetric("clustered", scan.clustered ? 1.0 : 0.0, "bool");
            addMetric("peak_frequency_hz", scan.peakFrequencyHz, "Hz");
            addMetric("peak_level_db_hz", scan.peakDbHz, "dB/Hz");
            addMetric("background_db_hz", scan.backgroundDbHz, "dB/Hz");
            addMetric("peak_excess_db", scan.excessDb, "dB");
            addMetric("sequence_frames", double(_sequence.size()), "count");
            size_t hits = 0;
            for (size_t i = 0; i < _sequence.size(); ++i)
                if (_sequence[i].hit) ++hits;
            addMetric("sequence_hits", double(hits), "count");
            addMetric("auto_notch_count", double(_notches.size()), "count");

            if (_active)
            {
                Object object;
                object.id = _activeId;
                object.type = ObjectType::Unknown;
                object.active = true;
                object.frequencyHz = _activeFrequencyHz;
                object.frequencyRateHzS = _activeRateHzS;
                object.strengthDbHz = _activeStrengthDbHz;
                object.excessDb = _activeExcessDb;
                object.ageSeconds = secondsBetween(_lastActiveHitNs, _activeStartNs);
                object.occupancy = _activeOpportunities
                    ? double(_activeHits) / double(_activeOpportunities) : 0.0;
                object.hits = _activeHits;
                object.opportunities = _activeOpportunities;
                _objects.push_back(object);
            }
        }

        out.debug.metrics = _metrics.empty() ? nullptr : _metrics.data();
        out.debug.metricCount = _metrics.size();
        out.debug.objects = _objects.empty() ? nullptr : _objects.data();
        out.debug.objectCount = _objects.size();
        return out;
    }

    void addMetric(const char *name, const double value, const char *unit)
    {
        Metric metric;
        metric.name = name;
        metric.value = value;
        metric.unit = unit;
        _metrics.push_back(metric);
    }

    MeteorLogger3fConfig _cfg;
    Environment _environment;
    Info _info;
    size_t _firstBin = 0;
    size_t _lastBin = 0;
    std::deque<Observation> _sequence;
    std::vector<Notch> _notches;
    uint64_t _notchRunStartNs = 0;
    bool _notchRunActive = false;
    double _notchRunFrequencyHz = 0.0;
    size_t _notchRunSamples = 0;
    bool _active = false;
    uint64_t _nextObjectId = 0;
    uint64_t _activeId = 0;
    uint64_t _activeStartNs = 0;
    uint64_t _lastActiveHitNs = 0;
    double _activeFrequencyHz = 0.0;
    double _activeRateHzS = 0.0;
    double _activeStrengthDbHz = -300.0;
    double _activeExcessDb = 0.0;
    uint64_t _activeHits = 0;
    uint64_t _activeOpportunities = 0;
    std::vector<Metric> _metrics;
    std::vector<Object> _objects;
};

} // namespace

std::vector<ConfigField> meteorLogger3fSchema()
{
    return {
        {"meteor_logger.detection_min_hz", "-49000.0", "3f search lower frequency"},
        {"meteor_logger.detection_max_hz", "49000.0", "3f search upper frequency"},
        {"meteor_logger.cluster_width_hz", "250.0", "Maximum span of the three strongest frequencies"},
        {"meteor_logger.sequence_frames", "6", "Frames in the temporal confirmation sequence"},
        {"meteor_logger.allowed_gaps", "1", "Missing clustered frames allowed per sequence"},
        {"meteor_logger.max_drift_hz_s", "11000.0", "Maximum peak-frequency drift rate"},
        {"meteor_logger.release_s", "0.15", "No-compatible-peak event release time"},
        {"meteor_logger.auto_notch_enabled", "true", "Learn and reject persistent interference lines"},
        {"meteor_logger.auto_notch_after_s", "10.0", "Persistence before learning an interference notch"},
        {"meteor_logger.auto_notch_width_hz", "200.0", "Learned interference notch width"},
        {"meteor_logger.max_auto_notches", "4", "Maximum learned interference notches"}
    };
}

void validateMeteorLogger3fConfig(const Config &config, const Environment &environment)
{
    const MeteorLogger3fConfig c = parseConfig(config);
    if (environment.bins < 2 || !(environment.frequencyStepHz > 0.0) ||
        !(environment.framePeriodSeconds > 0.0))
        throw std::runtime_error("meteor_logger_3f requires valid PSD geometry");
    const double bandLo = environment.frequencyStartHz;
    const double bandHi = bandLo + environment.frequencyStepHz * double(environment.bins - 1);
    if (!(c.detectionMinHz < c.detectionMaxHz) ||
        c.detectionMinHz < bandLo || c.detectionMaxHz > bandHi)
        throw std::runtime_error("detector.meteor_logger detection interval must lie inside detector PSD band");
    if (!(c.clusterWidthHz > 0.0))
        throw std::runtime_error("detector.meteor_logger.cluster_width_hz must be > 0");
    if (c.sequenceFrames < 2 || c.allowedGaps >= c.sequenceFrames)
        throw std::runtime_error("detector.meteor_logger requires sequence_frames >= 2 and allowed_gaps < sequence_frames");
    if (!(c.maxDriftHzS > 0.0) || c.releaseSeconds < 0.0)
        throw std::runtime_error("invalid detector.meteor_logger drift or release setting");
    if (c.autoNotchEnabled && (!(c.autoNotchAfterSeconds > 0.0) ||
        !(c.autoNotchWidthHz > 0.0) || c.maxAutoNotches == 0))
        throw std::runtime_error("invalid detector.meteor_logger auto-notch setting");
}

Requirements meteorLogger3fRequirements(const Config &config,
                                         const Environment &environment)
{
    Requirements requirements;
    (void)config;
    (void)environment;
    // The core's early configuration validation uses placeholder PSD timing.
    // The normal 0.5 s recorder pre-context comfortably exceeds the 5/6-frame
    // confirmation delay at Meteoris PSD cadences; document this relationship
    // rather than deriving a false requirement from placeholder geometry.
    return requirements;
}

std::unique_ptr<IDetector> makeMeteorLogger3fDetector(
    const Config &config, const Environment &environment)
{
    validateMeteorLogger3fConfig(config, environment);
    return std::unique_ptr<IDetector>(
        new MeteorLogger3fDetector(parseConfig(config), environment));
}

} // namespace detector
} // namespace meteoris
