#include "detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace meteoris {
namespace detector {
namespace {

struct EchoesAutomaticConfig
{
    std::string thresholdMode = "automatic";
    double detectionCenterHz = 0.0;
    double detectionWidthHz = 0.0;
    double absoluteLowerDbHz = -90.0;
    double absoluteUpperDbHz = -85.0;
    double differentialLowerDb = 4.0;
    double differentialUpperDb = 7.0;
    double automaticLowerOffsetDb = 4.0;
    double automaticUpperDeltaDb = 3.0;
    double automaticWarmupSeconds = 5.0;
    double automaticBaselineTimeConstantSeconds = 30.0;
    double automaticStddevWindowSeconds = 1.0;
    double automaticEndStddevFactor = 2.0;
    double delayBeforeTriggerSeconds = 0.0;
    double joinEventsSeconds = 1.0;
};

EchoesAutomaticConfig parseEchoesConfig(const Config &c)
{
    EchoesAutomaticConfig x;
    x.thresholdMode = c.stringValue("echoes.threshold_mode", x.thresholdMode);
    x.detectionCenterHz = c.doubleValue("echoes.detection_center_hz", x.detectionCenterHz);
    x.detectionWidthHz = c.doubleValue("echoes.detection_width_hz", x.detectionWidthHz);
    x.absoluteLowerDbHz = c.doubleValue("echoes.absolute_lower_db_hz", x.absoluteLowerDbHz);
    x.absoluteUpperDbHz = c.doubleValue("echoes.absolute_upper_db_hz", x.absoluteUpperDbHz);
    x.differentialLowerDb = c.doubleValue("echoes.differential_lower_db", x.differentialLowerDb);
    x.differentialUpperDb = c.doubleValue("echoes.differential_upper_db", x.differentialUpperDb);
    x.automaticLowerOffsetDb = c.doubleValue("echoes.automatic_lower_offset_db", x.automaticLowerOffsetDb);
    x.automaticUpperDeltaDb = c.doubleValue("echoes.automatic_upper_delta_db", x.automaticUpperDeltaDb);
    x.automaticWarmupSeconds = c.doubleValue("echoes.automatic_warmup_s", x.automaticWarmupSeconds);
    x.automaticBaselineTimeConstantSeconds = c.doubleValue("echoes.automatic_baseline_time_constant_s", x.automaticBaselineTimeConstantSeconds);
    x.automaticStddevWindowSeconds = c.doubleValue("echoes.automatic_stddev_window_s", x.automaticStddevWindowSeconds);
    x.automaticEndStddevFactor = c.doubleValue("echoes.automatic_end_stddev_factor", x.automaticEndStddevFactor);
    x.delayBeforeTriggerSeconds = c.doubleValue("echoes.delay_before_trigger_s", x.delayBeforeTriggerSeconds);
    x.joinEventsSeconds = c.doubleValue("echoes.join_events_closer_than_s", x.joinEventsSeconds);
    return x;
}

constexpr double MIN_POWER = 1e-30;

enum class ThresholdMode
{
    Absolute,
    Differential,
    Automatic
};

class EchoesAutomaticDetector final : public IDetector
{
public:
    EchoesAutomaticDetector(
        const EchoesAutomaticConfig &cfg,
        const Environment &environment)
        : _cfg(cfg), _environment(environment)
    {
        if (_environment.bins == 0 ||
            !(_environment.frequencyStepHz > 0.0) ||
            !(_environment.framePeriodSeconds > 0.0))
            throw std::runtime_error("echoes_automatic: invalid PSD geometry");

        if (_cfg.thresholdMode == "absolute")
            _mode = ThresholdMode::Absolute;
        else if (_cfg.thresholdMode == "differential")
            _mode = ThresholdMode::Differential;
        else if (_cfg.thresholdMode == "automatic")
            _mode = ThresholdMode::Automatic;
        else
            throw std::runtime_error(
                "echoes_automatic: threshold_mode must be absolute, differential, or automatic");

        selectDetectionBins();
        _warmupFrames = static_cast<uint64_t>(std::ceil(
            _cfg.automaticWarmupSeconds / _environment.framePeriodSeconds));
        _stddevWindowFrames = std::max<size_t>(1, static_cast<size_t>(std::ceil(
            _cfg.automaticStddevWindowSeconds /
            _environment.framePeriodSeconds)));

        const double tau = _cfg.automaticBaselineTimeConstantSeconds;
        _baselineAlpha = tau > 0.0
            ? 1.0 - std::exp(-_environment.framePeriodSeconds / tau)
            : 1.0;

        _info.apiVersion = DETECTOR_API_VERSION;
        _info.name = "echoes_automatic";
        _info.version = "1";
        _info.supportsStructuredDebug = true;
        _info.mayUseMultipleThreads = false;
    }

    const Info &info() const override { return _info; }

    void reset() override
    {
        clearEventState();
        _metrics.clear();
        _objects.clear();
        // Preserve scan history and the learned automatic baseline. This is
        // required when reset marks a recorder rearm boundary.
    }

    Result process(const Frame &frame, const ProcessOptions &options) override
    {
        if (frame.psd == nullptr || frame.bins != _environment.bins)
            throw std::runtime_error("echoes_automatic: PSD geometry changed");

        const Scan scan = measure(frame);
        updateSignalHistory(scan.signalDbHz);

        _metrics.clear();
        _objects.clear();

        if (options.mode == ProcessingMode::PreprocessOnly)
        {
            clearEventState();
            updateAutomaticBaseline(scan.differenceDb, currentSignalStddev());
            return makeResult(scan, State::Idle, false, false);
        }

        const bool automaticReady =
            _mode != ThresholdMode::Automatic ||
            (_baselineInitialized && _baselineFrames >= _warmupFrames);

        double lower = lowerThreshold();
        double upper = upperThreshold();
        const double observed = observedValue(scan);

        if (_active)
        {
            lower = _frozenLower;
            upper = _frozenUpper;
            bool endCondition = observed < lower;
            if (_mode == ThresholdMode::Automatic)
                endCondition = endCondition &&
                    currentSignalStddev() <= _frozenEndStddevLimit;

            if (endCondition)
            {
                if (_belowSinceNs == 0) _belowSinceNs = frame.timestampNs;
                const uint64_t joinNs = secondsToNs(_cfg.joinEventsSeconds);
                if (joinNs == 0 || frame.timestampNs >= _belowSinceNs + joinNs)
                {
                    _active = false;
                    _belowSinceNs = 0;
                }
            }
            else
            {
                _belowSinceNs = 0;
            }
        }
        else if (_pending)
        {
            lower = _frozenLower;
            upper = _frozenUpper;
            if (frame.timestampNs >= _pendingUntilNs)
            {
                _pending = false;
                _active = true;
            }
        }
        else if (automaticReady && observed >= upper)
        {
            freezeThresholds(lower, upper);
            const uint64_t delayNs =
                secondsToNs(_cfg.delayBeforeTriggerSeconds);
            if (delayNs == 0)
                _active = true;
            else
            {
                _pending = true;
                _pendingUntilNs = frame.timestampNs + delayNs;
            }
        }

        // Learn only from idle, non-triggering scans. Thresholds remain frozen
        // from the initial crossing through the complete joined event.
        if (!_active && !_pending)
            updateAutomaticBaseline(scan.differenceDb, currentSignalStddev());

        const State state = !automaticReady
            ? State::WarmingUp : (_active ? State::Active : State::Idle);
        const bool collectDebug = options.collectDebug;
        Result result = makeResult(scan, state, collectDebug, _pending);
        if (collectDebug)
            appendDebug(scan, observed, lower, upper, automaticReady);
        result.debug.metrics = _metrics.empty() ? nullptr : _metrics.data();
        result.debug.metricCount = _metrics.size();
        result.debug.objects = nullptr;
        result.debug.objectCount = 0;
        return result;
    }

private:
    struct Scan
    {
        double signalDbHz = -300.0;
        double noiseDbHz = -300.0;
        double differenceDb = 0.0;
        double peakFrequencyHz = 0.0;
    };

    static uint64_t secondsToNs(const double seconds)
    {
        if (!(seconds > 0.0)) return 0;
        return static_cast<uint64_t>(std::llround(seconds * 1e9));
    }

    void selectDetectionBins()
    {
        _firstBin = 0;
        _lastBin = _environment.bins;
        if (!(_cfg.detectionWidthHz > 0.0)) return;

        const double lo = _cfg.detectionCenterHz - 0.5 * _cfg.detectionWidthHz;
        const double hi = _cfg.detectionCenterHz + 0.5 * _cfg.detectionWidthHz;
        _firstBin = _environment.bins;
        _lastBin = 0;
        for (size_t k = 0; k < _environment.bins; ++k)
        {
            const double f = _environment.frequencyStartHz +
                             double(k) * _environment.frequencyStepHz;
            if (f >= lo && f <= hi)
            {
                _firstBin = std::min(_firstBin, k);
                _lastBin = std::max(_lastBin, k + 1);
            }
        }
        if (_firstBin >= _lastBin)
            throw std::runtime_error(
                "echoes_automatic: detection interval contains no PSD bins");
    }

    Scan measure(const Frame &frame) const
    {
        double maximum = MIN_POWER;
        double sum = 0.0;
        size_t maximumBin = _firstBin;
        for (size_t k = _firstBin; k < _lastBin; ++k)
        {
            const double power = std::max<double>(frame.psd[k], MIN_POWER);
            sum += power;
            if (power > maximum)
            {
                maximum = power;
                maximumBin = k;
            }
        }

        Scan scan;
        scan.signalDbHz = 10.0 * std::log10(maximum);
        scan.noiseDbHz = 10.0 * std::log10(
            std::max(sum / double(_lastBin - _firstBin), MIN_POWER));
        scan.differenceDb = scan.signalDbHz - scan.noiseDbHz;
        scan.peakFrequencyHz = _environment.frequencyStartHz +
            double(maximumBin) * _environment.frequencyStepHz;
        return scan;
    }

    void updateSignalHistory(const double signalDbHz)
    {
        _signalHistory.push_back(signalDbHz);
        _signalSum += signalDbHz;
        _signalSumSquares += signalDbHz * signalDbHz;
        while (_signalHistory.size() > _stddevWindowFrames)
        {
            const double old = _signalHistory.front();
            _signalHistory.pop_front();
            _signalSum -= old;
            _signalSumSquares -= old * old;
        }
    }

    double currentSignalStddev() const
    {
        if (_signalHistory.size() < 2) return 0.0;
        const double n = double(_signalHistory.size());
        const double mean = _signalSum / n;
        return std::sqrt(std::max(0.0, _signalSumSquares / n - mean * mean));
    }

    void updateAutomaticBaseline(const double differenceDb,
                                 const double signalStddev)
    {
        if (_mode != ThresholdMode::Automatic) return;
        if (!_baselineInitialized)
        {
            _baselineDifference = differenceDb;
            _baselineSignalStddev = signalStddev;
            _baselineInitialized = true;
        }
        else
        {
            _baselineDifference +=
                _baselineAlpha * (differenceDb - _baselineDifference);
            _baselineSignalStddev +=
                _baselineAlpha * (signalStddev - _baselineSignalStddev);
        }
        ++_baselineFrames;
    }

    double observedValue(const Scan &scan) const
    {
        return _mode == ThresholdMode::Absolute
            ? scan.signalDbHz : scan.differenceDb;
    }

    double lowerThreshold() const
    {
        if (_mode == ThresholdMode::Absolute) return _cfg.absoluteLowerDbHz;
        if (_mode == ThresholdMode::Differential) return _cfg.differentialLowerDb;
        return _baselineDifference + _cfg.automaticLowerOffsetDb;
    }

    double upperThreshold() const
    {
        if (_mode == ThresholdMode::Absolute) return _cfg.absoluteUpperDbHz;
        if (_mode == ThresholdMode::Differential) return _cfg.differentialUpperDb;
        return lowerThreshold() + _cfg.automaticUpperDeltaDb;
    }

    void freezeThresholds(const double lower, const double upper)
    {
        _frozenLower = lower;
        _frozenUpper = upper;
        _frozenEndStddevLimit = std::max(
            0.1, _cfg.automaticEndStddevFactor * _baselineSignalStddev);
    }

    void clearEventState()
    {
        _active = false;
        _pending = false;
        _pendingUntilNs = 0;
        _belowSinceNs = 0;
    }

    Result makeResult(const Scan &scan, const State state,
                      const bool collectDebug, const bool pending)
    {
        Result result;
        result.state = state;
        result.activeObjects = _active ? 1 : 0;
        result.frameMaxDbHz = static_cast<float>(scan.signalDbHz);
        if (collectDebug)
        {
            addMetric("pending_trigger", pending ? 1.0 : 0.0, "bool");
            result.debug.metrics = _metrics.data();
            result.debug.metricCount = _metrics.size();
        }
        return result;
    }

    void appendDebug(const Scan &scan, const double observed,
                     const double lower, const double upper,
                     const bool automaticReady)
    {
        _metrics.reserve(14);
        addMetric("signal_db_hz", scan.signalDbHz, "dB/Hz");
        addMetric("noise_db_hz", scan.noiseDbHz, "dB/Hz");
        addMetric("difference_db", scan.differenceDb, "dB");
        addMetric("observed_value", observed,
                  _mode == ThresholdMode::Absolute ? "dB/Hz" : "dB");
        addMetric("lower_threshold", lower,
                  _mode == ThresholdMode::Absolute ? "dB/Hz" : "dB");
        addMetric("upper_threshold", upper,
                  _mode == ThresholdMode::Absolute ? "dB/Hz" : "dB");
        addMetric("average_difference_db", _baselineDifference, "dB");
        addMetric("signal_stddev_db", currentSignalStddev(), "dB");
        addMetric("end_stddev_limit_db", _frozenEndStddevLimit, "dB");
        addMetric("peak_frequency_hz", scan.peakFrequencyHz, "Hz");
        addMetric("automatic_ready", automaticReady ? 1.0 : 0.0, "bool");
    }

    void addMetric(const char *name, const double value, const char *unit)
    {
        Metric metric;
        metric.name = name;
        metric.value = value;
        metric.unit = unit;
        _metrics.push_back(metric);
    }

    EchoesAutomaticConfig _cfg;
    Environment _environment;
    ThresholdMode _mode = ThresholdMode::Automatic;
    Info _info;
    size_t _firstBin = 0;
    size_t _lastBin = 0;
    uint64_t _warmupFrames = 0;
    size_t _stddevWindowFrames = 1;
    double _baselineAlpha = 1.0;
    bool _baselineInitialized = false;
    uint64_t _baselineFrames = 0;
    double _baselineDifference = 0.0;
    double _baselineSignalStddev = 0.0;
    std::deque<double> _signalHistory;
    double _signalSum = 0.0;
    double _signalSumSquares = 0.0;
    bool _active = false;
    bool _pending = false;
    uint64_t _pendingUntilNs = 0;
    uint64_t _belowSinceNs = 0;
    double _frozenLower = 0.0;
    double _frozenUpper = 0.0;
    double _frozenEndStddevLimit = 0.1;
    std::vector<Metric> _metrics;
    std::vector<Object> _objects;
};

} // namespace

std::vector<ConfigField> echoesAutomaticSchema()
{
    return {
        {"echoes.threshold_mode", "\"automatic\"", "absolute, differential, or automatic"},
        {"echoes.detection_center_hz", "0.0", "Detection interval center frequency"},
        {"echoes.detection_width_hz", "0.0", "Detection interval width; <=0 means full PSD band"},
        {"echoes.absolute_lower_db_hz", "-90.0", "Absolute lower threshold"},
        {"echoes.absolute_upper_db_hz", "-85.0", "Absolute upper threshold"},
        {"echoes.differential_lower_db", "4.0", "Differential lower threshold"},
        {"echoes.differential_upper_db", "7.0", "Differential upper threshold"},
        {"echoes.automatic_lower_offset_db", "4.0", "Automatic lower-threshold offset"},
        {"echoes.automatic_upper_delta_db", "3.0", "Automatic upper-threshold delta"},
        {"echoes.automatic_warmup_s", "5.0", "Automatic baseline warmup"},
        {"echoes.automatic_baseline_time_constant_s", "30.0", "Automatic baseline time constant"},
        {"echoes.automatic_stddev_window_s", "1.0", "Automatic signal standard-deviation window"},
        {"echoes.automatic_end_stddev_factor", "2.0", "Automatic event-end standard-deviation factor"},
        {"echoes.delay_before_trigger_s", "0.0", "Delay before reporting a trigger"},
        {"echoes.join_events_closer_than_s", "1.0", "Join events separated by less than this interval"}
    };
}

void validateEchoesAutomaticConfig(const Config &config, const Environment &environment)
{
    const EchoesAutomaticConfig c = parseEchoesConfig(config);
    const bool modeValid = c.thresholdMode == "absolute" || c.thresholdMode == "differential" || c.thresholdMode == "automatic";
    if (!modeValid) throw std::runtime_error("detector.echoes.threshold_mode must be absolute, differential, or automatic");
    const double loBand = environment.frequencyStartHz;
    const double hiBand = environment.frequencyStartHz + environment.frequencyStepHz * double(environment.bins ? environment.bins - 1 : 0);
    if (c.detectionWidthHz < 0.0 || (c.detectionWidthHz > 0.0 &&
        (c.detectionCenterHz - 0.5*c.detectionWidthHz < loBand || c.detectionCenterHz + 0.5*c.detectionWidthHz > hiBand)))
        throw std::runtime_error("detector.echoes detection interval must lie inside detector PSD band");
    if (!(c.absoluteLowerDbHz < c.absoluteUpperDbHz) || !(c.differentialLowerDb < c.differentialUpperDb) ||
        c.automaticLowerOffsetDb < 0.0 || !(c.automaticUpperDeltaDb > 0.0) || c.automaticWarmupSeconds < 0.0 ||
        !(c.automaticBaselineTimeConstantSeconds > 0.0) || !(c.automaticStddevWindowSeconds > 0.0) ||
        !(c.automaticEndStddevFactor > 0.0) || c.delayBeforeTriggerSeconds < 0.0 || c.joinEventsSeconds < 0.0)
        throw std::runtime_error("invalid detector.echoes threshold or timing parameters");
}

Requirements echoesAutomaticRequirements(const Config &config)
{
    Requirements r;
    r.minPreContextSeconds = parseEchoesConfig(config).delayBeforeTriggerSeconds;
    return r;
}

std::unique_ptr<IDetector> makeEchoesAutomaticDetector(
    const Config &config,
    const Environment &environment)
{
    validateEchoesAutomaticConfig(config, environment);
    return std::unique_ptr<IDetector>(
        new EchoesAutomaticDetector(parseEchoesConfig(config), environment));
}

} // namespace detector
} // namespace meteoris
