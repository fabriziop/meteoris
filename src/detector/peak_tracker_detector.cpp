#include "detector.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace meteoris {
namespace detector {
namespace {

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

PeakTrackerConfig parsePeakTrackerConfig(const Config &c)
{
    PeakTrackerConfig x;
    x.frequencyMeanBins = c.sizeValue("frequency_mean_bins", x.frequencyMeanBins);
    x.timeMeanPsds = c.sizeValue("time_mean_psds", x.timeMeanPsds);
    x.peakThresholdDb = c.doubleValue("peak_threshold_db", x.peakThresholdDb);
    x.minPeakSeparationHz = c.doubleValue("min_peak_separation_hz", x.minPeakSeparationHz);
    x.maxPeaksPerPsd = c.sizeValue("max_peaks_per_psd", x.maxPeaksPerPsd);
    x.stationaryEnabled = c.boolValue("stationary.enabled", x.stationaryEnabled);
    x.stationaryMinHz = c.doubleValue("stationary.min_hz", x.stationaryMinHz);
    x.stationaryMaxHz = c.doubleValue("stationary.max_hz", x.stationaryMaxHz);
    x.stationaryMaxDfHz = c.doubleValue("stationary.max_df_hz", x.stationaryMaxDfHz);
    x.stationaryActivationSeconds = c.doubleValue("stationary.activation_time_s", x.stationaryActivationSeconds);
    x.stationaryActivationFraction = c.doubleValue("stationary.activation_fraction", x.stationaryActivationFraction);
    x.stationaryLostSeconds = c.doubleValue("stationary.lost_s", x.stationaryLostSeconds);
    x.stationaryMaxDriftHzS = c.doubleValue("stationary.max_drift_hz_s", x.stationaryMaxDriftHzS);
    x.chirpEnabled = c.boolValue("chirp.enabled", x.chirpEnabled);
    x.chirpMinHz = c.doubleValue("chirp.min_hz", x.chirpMinHz);
    x.chirpMaxHz = c.doubleValue("chirp.max_hz", x.chirpMaxHz);
    x.chirpMaxDfHz = c.doubleValue("chirp.max_df_hz", x.chirpMaxDfHz);
    x.chirpActivationSeconds = c.doubleValue("chirp.activation_time_s", x.chirpActivationSeconds);
    x.chirpActivationFraction = c.doubleValue("chirp.activation_fraction", x.chirpActivationFraction);
    x.chirpLostSeconds = c.doubleValue("chirp.lost_s", x.chirpLostSeconds);
    x.chirpMinDriftHzS = c.doubleValue("chirp.min_drift_hz_s", x.chirpMinDriftHzS);
    x.chirpMaxDriftHzS = c.doubleValue("chirp.max_drift_hz_s", x.chirpMaxDriftHzS);
    return x;
}

struct SpectralPeak
{
    double frequencyHz = 0.0;
    float psdDbHz = -300.0f;
    float excessDb = 0.0f;
};

struct PeakExtractionResult
{
    bool ready = false;
    float backgroundDbHz = -300.0f;
    float frameMaxDbHz = -300.0f;
    size_t rawCandidates = 0;
    size_t closeSuppressed = 0;
    size_t droppedByLimit = 0;
    std::vector<SpectralPeak> peaks;
};

class PsdPeakExtractor
{
public:
    PsdPeakExtractor(const PeakTrackerConfig &cfg, const std::vector<double> &frequencies)
        : _cfg(cfg), _frequencies(frequencies),
          _timeSum(frequencies.size(), 0.0),
          _frequencyMean(frequencies.size(), 0.0f),
          _smoothed(frequencies.size(), 0.0f),
          _prefix(frequencies.size() + 1, 0.0),
          _scratch(frequencies.size(), 0.0f)
    {
        if (_frequencies.size() < 3)
            throw std::runtime_error("detector requires at least three PSD bins");
        if (_cfg.frequencyMeanBins == 0 ||
            (_cfg.frequencyMeanBins & 1u) == 0)
            throw std::runtime_error("detector.frequency_mean_bins must be an odd positive integer");
        if (_cfg.timeMeanPsds == 0)
            throw std::runtime_error("detector.time_mean_psds must be >= 1");
        if (!(_cfg.peakThresholdDb > 0.0))
            throw std::runtime_error("detector.peak_threshold_db must be > 0");
        if (_cfg.minPeakSeparationHz < 0.0)
            throw std::runtime_error("detector.min_peak_separation_hz must be >= 0");
        if (_cfg.maxPeaksPerPsd == 0)
            throw std::runtime_error("detector.max_peaks_per_psd must be >= 1");

        _halfFrequencyWindow = _cfg.frequencyMeanBins / 2;
        if (_frequencies.size() > 1)
            _binHz = std::abs(_frequencies[1] - _frequencies[0]);
        if (!(_binHz > 0.0)) _binHz = 1.0;
        _thresholdRatio = static_cast<float>(
            std::pow(10.0, _cfg.peakThresholdDb / 10.0));
    }

    PeakExtractionResult process(const Frame &frame)
    {
        if (frame.bins != _frequencies.size())
            throw std::runtime_error("PSD bin count changed in peak detector");

        PeakExtractionResult out;

        // Keep the recorded detector metric compatible with the previous
        // format: maximum raw PSD density in the final observation band.
        float rawMax = 1e-30f;
        for (size_t k = 0; k < frame.bins; ++k)
            rawMax = std::max(rawMax, frame.psd[k]);
        out.frameMaxDbHz = 10.0f * std::log10(rawMax);

        // 1) Centered square mean on the frequency axis, in linear power.
        _prefix[0] = 0.0;
        for (size_t k = 0; k < frame.bins; ++k)
            _prefix[k + 1] = _prefix[k] + std::max<double>(frame.psd[k], 1e-30);

        for (size_t k = 0; k < frame.bins; ++k)
        {
            const size_t lo = (k > _halfFrequencyWindow) ? k - _halfFrequencyWindow : 0;
            const size_t hi = std::min(
                frame.bins - 1, k + _halfFrequencyWindow);
            const size_t count = hi - lo + 1;
            _frequencyMean[k] = static_cast<float>(
                (_prefix[hi + 1] - _prefix[lo]) / double(count));
        }

        // 2) Causal rolling mean on the time axis, again in linear power.
        if (_timeHistory.size() == _cfg.timeMeanPsds)
        {
            const std::vector<float> &oldest = _timeHistory.front();
            for (size_t k = 0; k < _timeSum.size(); ++k)
                _timeSum[k] -= oldest[k];
            _timeHistory.pop_front();
        }

        _timeHistory.push_back(_frequencyMean);
        for (size_t k = 0; k < _timeSum.size(); ++k)
            _timeSum[k] += _frequencyMean[k];

        if (_timeHistory.size() < _cfg.timeMeanPsds)
            return out;

        const double invTime = 1.0 / double(_timeHistory.size());
        for (size_t k = 0; k < _smoothed.size(); ++k)
            _smoothed[k] = static_cast<float>(_timeSum[k] * invTime);

        // Robust instantaneous background: median of the doubly-smoothed PSD.
        _scratch = _smoothed;
        const size_t mid = _scratch.size() / 2;
        std::nth_element(_scratch.begin(), _scratch.begin() + mid, _scratch.end());
        const float background = std::max(_scratch[mid], 1e-30f);
        out.backgroundDbHz = 10.0f * std::log10(background);
        out.ready = true;

        struct Candidate
        {
            SpectralPeak peak;
            float power = 0.0f;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        const float threshold = background * _thresholdRatio;
        for (size_t k = 1; k + 1 < _smoothed.size(); ++k)
        {
            const float y = _smoothed[k];
            if (!(y >= threshold && y > _smoothed[k - 1] && y >= _smoothed[k + 1]))
                continue;

            // Sub-bin peak interpolation on log-power values.
            const double ym1 = 10.0 * std::log10(std::max<double>(_smoothed[k - 1], 1e-30));
            const double y0  = 10.0 * std::log10(std::max<double>(_smoothed[k],     1e-30));
            const double yp1 = 10.0 * std::log10(std::max<double>(_smoothed[k + 1], 1e-30));
            const double denom = ym1 - 2.0 * y0 + yp1;
            double delta = 0.0;
            if (std::abs(denom) > 1e-12)
                delta = 0.5 * (ym1 - yp1) / denom;
            delta = std::max(-0.5, std::min(0.5, delta));

            Candidate c;
            c.power = y;
            c.peak.frequencyHz = _frequencies[k] + delta * _binHz;
            c.peak.psdDbHz = static_cast<float>(y0);
            c.peak.excessDb = static_cast<float>(
                10.0 * std::log10(std::max<double>(y / background, 1e-30)));
            candidates.push_back(c);
        }

        out.rawCandidates = candidates.size();
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
                return a.power > b.power;
            });

        // 1-D non-maximum suppression: a cluster of nearby local maxima is one
        // physical spectral peak. Keep the strongest member.
        std::vector<SpectralPeak> selected;
        selected.reserve(std::min(candidates.size(), _cfg.maxPeaksPerPsd));
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            bool tooClose = false;
            for (size_t j = 0; j < selected.size(); ++j)
            {
                if (std::abs(candidates[i].peak.frequencyHz - selected[j].frequencyHz) <
                    _cfg.minPeakSeparationHz)
                {
                    tooClose = true;
                    ++out.closeSuppressed;
                    break;
                }
            }
            if (!tooClose) selected.push_back(candidates[i].peak);
        }

        if (selected.size() > _cfg.maxPeaksPerPsd)
        {
            out.droppedByLimit = selected.size() - _cfg.maxPeaksPerPsd;
            selected.resize(_cfg.maxPeaksPerPsd);
        }
        out.peaks.swap(selected);
        return out;
    }

private:
    const PeakTrackerConfig &_cfg;
    const std::vector<double> &_frequencies;
    size_t _halfFrequencyWindow = 0;
    double _binHz = 1.0;
    float _thresholdRatio = 1.0f;
    std::vector<double> _timeSum;
    std::vector<float> _frequencyMean;
    std::vector<float> _smoothed;
    std::vector<double> _prefix;
    std::vector<float> _scratch;
    std::deque<std::vector<float>> _timeHistory;
};

enum class PeakTrackClass
{
    Unknown,
    Stationary,
    Chirp
};


struct PeakTrack
{
    uint64_t id = 0;
    PeakTrackClass classification = PeakTrackClass::Unknown;
    uint64_t firstSeenNs = 0;
    uint64_t lastSeenNs = 0;
    double firstFrequencyHz = 0.0;
    double frequencyHz = 0.0;
    double velocityHzS = 0.0;
    float strengthDbHz = -300.0f;
    float excessDb = 0.0f;
    uint64_t opportunities = 0;
    uint64_t hits = 0;
    bool active = false;
};

struct PeakTrackerResult
{
    bool active = false;
    size_t activeTracks = 0;
    size_t stationaryActive = 0;
    size_t chirpActive = 0;
    size_t totalTracks = 0;
    const PeakTrack *representative = nullptr;
    const PeakTrack *representativeTentative = nullptr;
};

class PeakTracker
{
public:
    PeakTracker(const PeakTrackerConfig &cfg, const double framePeriodSeconds)
        : _cfg(cfg), _framePeriodSeconds(framePeriodSeconds)
    {
        if (!(_framePeriodSeconds > 0.0))
            throw std::runtime_error("cannot derive PSD frame period for peak tracker");
    }

    void reset()
    {
        _tracks.clear();
    }

    const std::vector<PeakTrack> &tracks() const { return _tracks; }

    PeakTrackerResult update(const uint64_t nowNs, const std::vector<SpectralPeak> &peaks)
    {
        for (size_t ti = 0; ti < _tracks.size(); ++ti)
            ++_tracks[ti].opportunities;

        struct Pair
        {
            size_t track = 0;
            size_t peak = 0;
            double cost = 0.0;
        };
        std::vector<Pair> pairs;

        for (size_t ti = 0; ti < _tracks.size(); ++ti)
        {
            const PeakTrack &tr = _tracks[ti];
            const double dt = (tr.lastSeenNs < nowNs)
                ? double(nowNs - tr.lastSeenNs) * 1e-9 : _framePeriodSeconds;
            const double predicted = (tr.classification == PeakTrackClass::Chirp)
                ? tr.frequencyHz + tr.velocityHzS * dt
                : tr.frequencyHz;
            const double window = matchWindowHz(tr);

            for (size_t pi = 0; pi < peaks.size(); ++pi)
            {
                if (!peakAllowedForTrack(tr, peaks[pi])) continue;
                const double df = std::abs(peaks[pi].frequencyHz - predicted);
                if (df <= window)
                {
                    Pair p;
                    p.track = ti;
                    p.peak = pi;
                    p.cost = df / std::max(window, 1e-12);
                    pairs.push_back(p);
                }
            }
        }

        std::sort(pairs.begin(), pairs.end(),
            [](const Pair &a, const Pair &b) { return a.cost < b.cost; });

        std::vector<bool> trackUsed(_tracks.size(), false);
        std::vector<bool> peakUsed(peaks.size(), false);
        for (size_t i = 0; i < pairs.size(); ++i)
        {
            const Pair &p = pairs[i];
            if (trackUsed[p.track] || peakUsed[p.peak]) continue;
            trackUsed[p.track] = true;
            peakUsed[p.peak] = true;
            updateMatchedTrack(_tracks[p.track], peaks[p.peak], nowNs);
        }

        // Drop tracks only after their class-specific loss time. Active tracks
        // remain active through shorter gaps, so event trigger-off corresponds
        // to loss of the last ACTIVE physical track.
        for (size_t ti = _tracks.size(); ti-- > 0;)
        {
            if (trackUsed[ti]) continue;
            const double missed = (nowNs > _tracks[ti].lastSeenNs)
                ? double(nowNs - _tracks[ti].lastSeenNs) * 1e-9 : 0.0;
            if (missed > lostSeconds(_tracks[ti]))
                _tracks.erase(_tracks.begin() + static_cast<std::ptrdiff_t>(ti));
        }

        // Unassigned peaks start tentative generic tracks. Classification is
        // postponed until several observations provide a meaningful drift.
        for (size_t pi = 0; pi < peaks.size(); ++pi)
        {
            if (peakUsed[pi] || !peakInsideAnySearchBand(peaks[pi].frequencyHz))
                continue;
            PeakTrack tr;
            tr.id = ++_nextTrackId;
            tr.firstSeenNs = nowNs;
            tr.lastSeenNs = nowNs;
            tr.firstFrequencyHz = peaks[pi].frequencyHz;
            tr.frequencyHz = peaks[pi].frequencyHz;
            tr.strengthDbHz = peaks[pi].psdDbHz;
            tr.excessDb = peaks[pi].excessDb;
            tr.opportunities = 1;
            tr.hits = 1;
            _tracks.push_back(tr);
        }

        PeakTrackerResult result;
        result.totalTracks = _tracks.size();
        for (size_t ti = 0; ti < _tracks.size(); ++ti)
        {
            PeakTrack &tr = _tracks[ti];
            maybeActivate(tr, nowNs);

            if (!tr.active)
            {
                // For diagnostics, prefer the oldest tentative track; break
                // ties with higher occupancy/excess so the line follows the
                // most plausible candidate rather than a fresh noise blip.
                const double age = (nowNs > tr.firstSeenNs)
                    ? double(nowNs - tr.firstSeenNs) * 1e-9 : 0.0;
                const double occ = tr.opportunities
                    ? double(tr.hits) / double(tr.opportunities) : 0.0;

                bool choose = false;
                if (result.representativeTentative == nullptr)
                {
                    choose = true;
                }
                else
                {
                    const PeakTrack &best = *result.representativeTentative;
                    const double bestAge = (nowNs > best.firstSeenNs)
                        ? double(nowNs - best.firstSeenNs) * 1e-9 : 0.0;
                    const double bestOcc = best.opportunities
                        ? double(best.hits) / double(best.opportunities) : 0.0;

                    if (age > bestAge + 1e-12)
                        choose = true;
                    else if (std::abs(age - bestAge) <= 1e-12 &&
                             (occ > bestOcc + 1e-12 ||
                              (std::abs(occ - bestOcc) <= 1e-12 &&
                               tr.excessDb > best.excessDb)))
                        choose = true;
                }

                if (choose)
                    result.representativeTentative = &tr;
                continue;
            }

            ++result.activeTracks;
            if (tr.classification == PeakTrackClass::Stationary)
                ++result.stationaryActive;
            else if (tr.classification == PeakTrackClass::Chirp)
                ++result.chirpActive;

            if (result.representative == nullptr ||
                tr.excessDb > result.representative->excessDb)
                result.representative = &tr;
        }
        result.active = result.activeTracks != 0;
        return result;
    }

private:
    bool inStationaryBand(const double f) const
    {
        return _cfg.stationaryEnabled &&
               f >= _cfg.stationaryMinHz &&
               f <= _cfg.stationaryMaxHz;
    }

    bool inChirpBand(const double f) const
    {
        return _cfg.chirpEnabled &&
               f >= _cfg.chirpMinHz &&
               f <= _cfg.chirpMaxHz;
    }

    bool peakInsideAnySearchBand(const double f) const
    {
        return inStationaryBand(f) || inChirpBand(f);
    }

    bool peakAllowedForTrack(const PeakTrack &tr, const SpectralPeak &peak) const
    {
        if (tr.classification == PeakTrackClass::Stationary)
            return inStationaryBand(peak.frequencyHz);
        if (tr.classification == PeakTrackClass::Chirp)
            return inChirpBand(peak.frequencyHz);
        return peakInsideAnySearchBand(peak.frequencyHz);
    }

    double matchWindowHz(const PeakTrack &tr) const
    {
        if (tr.classification == PeakTrackClass::Stationary)
            return _cfg.stationaryMaxDfHz;
        if (tr.classification == PeakTrackClass::Chirp)
            return _cfg.chirpMaxDfHz;

        double w = 0.0;
        if (inStationaryBand(tr.frequencyHz))
            w = std::max(w, _cfg.stationaryMaxDfHz);
        if (inChirpBand(tr.frequencyHz))
            w = std::max(w, _cfg.chirpMaxDfHz);
        return std::max(w, 1.0);
    }

    double lostSeconds(const PeakTrack &tr) const
    {
        if (tr.classification == PeakTrackClass::Stationary)
            return _cfg.stationaryLostSeconds;
        if (tr.classification == PeakTrackClass::Chirp)
            return _cfg.chirpLostSeconds;

        // An unknown central track may still turn out to be a long-lived
        // stationary signal, so permit the larger applicable gap.
        double lost = 0.0;
        if (inStationaryBand(tr.frequencyHz))
            lost = std::max(lost, _cfg.stationaryLostSeconds);
        if (inChirpBand(tr.frequencyHz))
            lost = std::max(lost, _cfg.chirpLostSeconds);
        return std::max(lost, _framePeriodSeconds);
    }

    void updateMatchedTrack(PeakTrack &tr, const SpectralPeak &peak, const uint64_t nowNs)
    {
        ++tr.hits;
        tr.lastSeenNs = nowNs;
        tr.frequencyHz = peak.frequencyHz;
        tr.strengthDbHz = peak.psdDbHz;
        tr.excessDb = peak.excessDb;

        const double age = (nowNs > tr.firstSeenNs)
            ? double(nowNs - tr.firstSeenNs) * 1e-9 : 0.0;
        if (age > 0.0)
            tr.velocityHzS = (tr.frequencyHz - tr.firstFrequencyHz) / age;

        // Wait for at least three hits and a few PSD intervals before assigning
        // a physical motion class. Average drift from first observation is far
        // less sensitive to one-bin peak jitter than a one-frame derivative.
        double classificationAge = 0.05;
        if (_cfg.stationaryEnabled)
            classificationAge = std::min(classificationAge, _cfg.stationaryActivationSeconds);
        if (_cfg.chirpEnabled)
            classificationAge = std::min(classificationAge, _cfg.chirpActivationSeconds);
        classificationAge = std::max(classificationAge, 2.0 * _framePeriodSeconds);
        if (tr.classification == PeakTrackClass::Unknown &&
            tr.hits >= 3 && age >= classificationAge)
        {
            const double av = std::abs(tr.velocityHzS);
            if (inStationaryBand(tr.frequencyHz) &&
                av <= _cfg.stationaryMaxDriftHzS)
            {
                tr.classification = PeakTrackClass::Stationary;
                tr.velocityHzS = 0.0;
            }
            else if (inChirpBand(tr.frequencyHz) &&
                     av >= _cfg.chirpMinDriftHzS &&
                     av <= _cfg.chirpMaxDriftHzS)
            {
                tr.classification = PeakTrackClass::Chirp;
            }
        }
    }

    void maybeActivate(PeakTrack &tr, const uint64_t nowNs)
    {
        if (tr.active || tr.classification == PeakTrackClass::Unknown)
            return;

        const double age = (nowNs > tr.firstSeenNs)
            ? double(nowNs - tr.firstSeenNs) * 1e-9 : 0.0;
        const double occupancy = (tr.opportunities != 0)
            ? double(tr.hits) / double(tr.opportunities) : 0.0;

        if (tr.classification == PeakTrackClass::Stationary)
        {
            if (age >= _cfg.stationaryActivationSeconds &&
                occupancy >= _cfg.stationaryActivationFraction)
                tr.active = true;
        }
        else if (tr.classification == PeakTrackClass::Chirp)
        {
            if (age >= _cfg.chirpActivationSeconds &&
                occupancy >= _cfg.chirpActivationFraction)
                tr.active = true;
        }
    }

    const PeakTrackerConfig &_cfg;
    double _framePeriodSeconds = 0.0;
    std::vector<PeakTrack> _tracks;
    uint64_t _nextTrackId = 0;
};

struct PeakDetectorResult
{
    bool ready = false;
    bool active = false;
    float frameMaxDbHz = -300.0f;
    float backgroundDbHz = -300.0f;
    size_t rawCandidates = 0;
    size_t closeSuppressed = 0;
    size_t droppedByLimit = 0;
    size_t peakCount = 0;
    size_t trackCount = 0;
    size_t activeTracks = 0;
    size_t stationaryActive = 0;
    size_t chirpActive = 0;
    SpectralPeak strongestPeak;
    bool haveStrongestPeak = false;
    PeakTrack representativeTrack;
    bool haveRepresentativeTrack = false;
    PeakTrack representativeTentativeTrack;
    bool haveRepresentativeTentativeTrack = false;
};

class PeakTrackerCore
{
public:
    PeakTrackerCore(const PeakTrackerConfig &cfg, const std::vector<double> &frequencies,
                      const double framePeriodSeconds)
        : _cfg(cfg), _extractor(cfg, frequencies),
          _tracker(cfg, framePeriodSeconds)
    {
    }

    PeakDetectorResult process(const Frame &frame, const bool trackingEnabled = true)
    {
        const PeakExtractionResult px = _extractor.process(frame);
        PeakDetectorResult r;
        r.ready = px.ready;
        r.frameMaxDbHz = px.frameMaxDbHz;
        r.backgroundDbHz = px.backgroundDbHz;
        r.rawCandidates = px.rawCandidates;
        r.closeSuppressed = px.closeSuppressed;
        r.droppedByLimit = px.droppedByLimit;
        r.peakCount = px.peaks.size();

        if (!px.peaks.empty())
        {
            r.strongestPeak = px.peaks.front();
            r.haveStrongestPeak = true;
        }

        if (!trackingEnabled)
        {
            _tracker.reset();
            return r;
        }
        if (!px.ready) return r;

        const PeakTrackerResult tr = _tracker.update(frame.timestampNs, px.peaks);
        r.active = tr.active;
        r.trackCount = tr.totalTracks;
        r.activeTracks = tr.activeTracks;
        r.stationaryActive = tr.stationaryActive;
        r.chirpActive = tr.chirpActive;
        if (tr.representative != nullptr)
        {
            r.representativeTrack = *tr.representative;
            r.haveRepresentativeTrack = true;
        }
        if (tr.representativeTentative != nullptr)
        {
            r.representativeTentativeTrack = *tr.representativeTentative;
            r.haveRepresentativeTentativeTrack = true;
        }
        return r;
    }

    void resetTracks() { _tracker.reset(); }
    const std::vector<PeakTrack> &tracks() const { return _tracker.tracks(); }

private:
    const PeakTrackerConfig &_cfg;
    PsdPeakExtractor _extractor;
    PeakTracker _tracker;
};


} // namespace

class PeakTrackerDetectorPlugin final : public IDetector
{
public:
    PeakTrackerDetectorPlugin(
        const PeakTrackerConfig &cfg,
        const Environment &environment)
        : _cfg(cfg),
          _environment(environment),
          _frequencies(environment.bins),
          _core(nullptr)
    {
        if (_environment.bins < 3)
            throw std::runtime_error("peak_tracker: at least three PSD bins are required");
        if (!(_environment.frequencyStepHz > 0.0))
            throw std::runtime_error("peak_tracker: invalid PSD frequency spacing");
        if (!(_environment.framePeriodSeconds > 0.0))
            throw std::runtime_error("peak_tracker: invalid PSD frame period");

        for (size_t k = 0; k < _frequencies.size(); ++k)
            _frequencies[k] = _environment.frequencyStartHz
                            + double(k) * _environment.frequencyStepHz;

        _core.reset(new PeakTrackerCore(
            _cfg, _frequencies, _environment.framePeriodSeconds));

        _info.apiVersion = DETECTOR_API_VERSION;
        _info.name = "peak_tracker";
        _info.version = "1";
        _info.supportsStructuredDebug = true;
        _info.mayUseMultipleThreads = false;
    }

    const Info &info() const override
    {
        return _info;
    }

    void reset() override
    {
        _core->resetTracks();
        _metrics.clear();
        _objects.clear();
    }

    Result process(const Frame &frame, const ProcessOptions &options) override
    {
        if (frame.psd == nullptr || frame.bins != _environment.bins)
            throw std::runtime_error("peak_tracker: PSD geometry changed");

        const bool trackingEnabled =
            options.mode == ProcessingMode::Full;
        const PeakDetectorResult d =
            _core->process(frame, trackingEnabled);

        _metrics.clear();
        _objects.clear();

        if (trackingEnabled && options.collectDebug)
        {
            _metrics.reserve(12);
            _objects.reserve(_core->tracks().size());

            addMetric("background_db_hz", d.backgroundDbHz, "dB/Hz");
            addMetric("raw_candidates", double(d.rawCandidates), "count");
            addMetric("close_suppressed", double(d.closeSuppressed), "count");
            addMetric("dropped_by_max_peaks", double(d.droppedByLimit), "count");
            addMetric("retained_peaks", double(d.peakCount), "count");
            addMetric("track_count", double(d.trackCount), "count");
            addMetric("active_tracks", double(d.activeTracks), "count");
            addMetric("stationary_active", double(d.stationaryActive), "count");
            addMetric("chirp_active", double(d.chirpActive), "count");

            if (d.haveStrongestPeak)
            {
                addMetric("strongest_peak_frequency_hz",
                          d.strongestPeak.frequencyHz, "Hz");
                addMetric("strongest_peak_level_db_hz",
                          d.strongestPeak.psdDbHz, "dB/Hz");
                addMetric("strongest_peak_excess_db",
                          d.strongestPeak.excessDb, "dB");
            }

            const std::vector<PeakTrack> &tracks = _core->tracks();
            for (size_t i = 0; i < tracks.size(); ++i)
                addTrackObject(tracks[i], frame.timestampNs);
        }

        Result r;
        r.state = !d.ready ? State::WarmingUp
                           : (d.active ? State::Active : State::Idle);
        r.activeObjects = static_cast<uint32_t>(d.activeTracks);
        r.frameMaxDbHz = d.frameMaxDbHz;
        r.rawCandidates = d.rawCandidates;
        r.closeSuppressed = d.closeSuppressed;
        r.droppedByLimit = d.droppedByLimit;
        r.retainedPeaks = d.peakCount;
        r.debug.metrics = _metrics.empty() ? nullptr : _metrics.data();
        r.debug.metricCount = _metrics.size();
        r.debug.objects = _objects.empty() ? nullptr : _objects.data();
        r.debug.objectCount = _objects.size();
        return r;
    }

private:
    void addMetric(const char *name, const double value, const char *unit)
    {
        Metric m;
        m.name = name;
        m.value = value;
        m.unit = unit;
        _metrics.push_back(m);
    }

    void addTrackObject(const PeakTrack &t, const uint64_t nowNs)
    {
        Object o;
        o.id = t.id;
        if (t.classification == PeakTrackClass::Stationary)
            o.type = ObjectType::Stationary;
        else if (t.classification == PeakTrackClass::Chirp)
            o.type = ObjectType::Chirp;
        else
            o.type = ObjectType::Unknown;

        o.active = t.active;
        o.frequencyHz = t.frequencyHz;
        o.frequencyRateHzS = t.velocityHzS;
        o.strengthDbHz = t.strengthDbHz;
        o.excessDb = t.excessDb;
        o.ageSeconds = nowNs > t.firstSeenNs
            ? double(nowNs - t.firstSeenNs) * 1e-9 : 0.0;
        o.occupancy = t.opportunities
            ? double(t.hits) / double(t.opportunities) : 0.0;
        o.hits = t.hits;
        o.opportunities = t.opportunities;
        o.missedSeconds = nowNs > t.lastSeenNs
            ? double(nowNs - t.lastSeenNs) * 1e-9 : 0.0;
        _objects.push_back(o);
    }

    PeakTrackerConfig _cfg;
    Environment _environment;
    std::vector<double> _frequencies;
    std::unique_ptr<PeakTrackerCore> _core;
    std::vector<Metric> _metrics;
    std::vector<Object> _objects;
    Info _info;
};

std::vector<ConfigField> peakTrackerSchema()
{
    return {
        {"frequency_mean_bins", "5", "Odd frequency smoothing width in PSD bins"},
        {"time_mean_psds", "3", "Causal PSD averaging depth"},
        {"peak_threshold_db", "5.0", "Peak threshold above the local background"},
        {"min_peak_separation_hz", "300.0", "Minimum separation between retained peaks"},
        {"max_peaks_per_psd", "20", "Maximum retained peaks per PSD"},
        {"stationary.enabled", "true", "Enable stationary-track classification"},
        {"stationary.min_hz", "-5000.0", "Stationary search lower frequency"},
        {"stationary.max_hz", "5000.0", "Stationary search upper frequency"},
        {"stationary.max_df_hz", "250.0", "Stationary association tolerance"},
        {"stationary.activation_time_s", "0.5", "Stationary activation time"},
        {"stationary.activation_fraction", "0.50", "Stationary activation occupancy"},
        {"stationary.lost_s", "0.30", "Stationary lost-track timeout"},
        {"stationary.max_drift_hz_s", "1000.0", "Maximum stationary drift rate"},
        {"chirp.enabled", "true", "Enable chirp-track classification"},
        {"chirp.min_hz", "-50000.0", "Chirp search lower frequency"},
        {"chirp.max_hz", "50000.0", "Chirp search upper frequency"},
        {"chirp.max_df_hz", "1500.0", "Chirp association tolerance"},
        {"chirp.activation_time_s", "0.05", "Chirp activation time"},
        {"chirp.activation_fraction", "0.70", "Chirp activation occupancy"},
        {"chirp.lost_s", "0.08", "Chirp lost-track timeout"},
        {"chirp.min_drift_hz_s", "3000.0", "Minimum chirp drift rate"},
        {"chirp.max_drift_hz_s", "150000.0", "Maximum chirp drift rate"}
    };
}

void validatePeakTrackerConfig(const Config &config, const Environment &environment)
{
    const PeakTrackerConfig c = parsePeakTrackerConfig(config);
    if (c.frequencyMeanBins == 0 || (c.frequencyMeanBins & 1u) == 0)
        throw std::runtime_error("detector.frequency_mean_bins must be an odd positive integer");
    if (c.timeMeanPsds == 0) throw std::runtime_error("detector.time_mean_psds must be >= 1");
    if (!(c.peakThresholdDb > 0.0)) throw std::runtime_error("detector.peak_threshold_db must be > 0");
    if (c.minPeakSeparationHz < 0.0) throw std::runtime_error("detector.min_peak_separation_hz must be >= 0");
    if (c.maxPeaksPerPsd == 0) throw std::runtime_error("detector.max_peaks_per_psd must be >= 1");
    if (!c.stationaryEnabled && !c.chirpEnabled)
        throw std::runtime_error("detector enabled but stationary and chirp tracking are both disabled");
    const double loBand = environment.frequencyStartHz;
    const double hiBand = environment.frequencyStartHz + environment.frequencyStepHz * double(environment.bins ? environment.bins - 1 : 0);
    auto range = [&](const char *name, bool enabled, double lo, double hi, double maxDf, double activationTime, double fraction, double lost) {
        if (!enabled) return;
        if (!(lo < hi && lo >= loBand && hi <= hiBand))
            throw std::runtime_error(std::string(name) + " frequency range must lie inside detector PSD band");
        if (!(maxDf > 0.0) || activationTime < 0.0 || !(fraction > 0.0 && fraction <= 1.0) || lost < 0.0)
            throw std::runtime_error(std::string("invalid ") + name + " tracking parameters");
    };
    range("detector.stationary", c.stationaryEnabled, c.stationaryMinHz, c.stationaryMaxHz, c.stationaryMaxDfHz, c.stationaryActivationSeconds, c.stationaryActivationFraction, c.stationaryLostSeconds);
    if (c.stationaryEnabled && c.stationaryMaxDriftHzS < 0.0)
        throw std::runtime_error("detector.stationary.max_drift_hz_s must be >= 0");
    range("detector.chirp", c.chirpEnabled, c.chirpMinHz, c.chirpMaxHz, c.chirpMaxDfHz, c.chirpActivationSeconds, c.chirpActivationFraction, c.chirpLostSeconds);
    if (c.chirpEnabled && !(c.chirpMinDriftHzS >= 0.0 && c.chirpMinDriftHzS < c.chirpMaxDriftHzS))
        throw std::runtime_error("detector.chirp drift limits must satisfy 0 <= min < max");
}

std::unique_ptr<IDetector> makePeakTrackerDetector(
    const Config &config,
    const Environment &environment)
{
    validatePeakTrackerConfig(config, environment);
    return std::unique_ptr<IDetector>(
        new PeakTrackerDetectorPlugin(parsePeakTrackerConfig(config), environment));
}

} // namespace detector
} // namespace meteoris
