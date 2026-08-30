#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Version.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
constexpr double PI = 3.141592653589793238462643383279502884;
constexpr const char *METEORIS_SIM_VERSION = "finite-events-2";

struct SimStream
{
    std::vector<std::vector<int8_t>> buffers;
    size_t nextHandle = 0;
    size_t mtu = 131072;
    bool active = false;
    uint64_t sampleIndex = 0;
    std::chrono::steady_clock::time_point epoch;

    // Signal state.
    std::complex<double> stationary = {1.0, 0.0};
    std::complex<double> chirp = {1.0, 0.0};
    std::complex<double> chirpStep = {1.0, 0.0};
    std::complex<double> chirpStepDelta = {1.0, 0.0};
    bool chirpWasActive = false;
    uint32_t rng = 0x4d657465u;
};

class MeteorisSim : public SoapySDR::Device
{
public:
    explicit MeteorisSim(const SoapySDR::Kwargs &args)
    {
        _eventPeriod = argDouble(args, "period_s", _eventPeriod);
        _noiseAmp = argDouble(args, "noise_amplitude", _noiseAmp);
        _stationaryEnabled = argBool(args, "stationary_enabled", _stationaryEnabled);
        _stationaryStart = argDouble(args, "stationary_start_s", _stationaryStart);
        _stationaryRise = argDouble(args, "stationary_rise_s", _stationaryRise);
        _stationaryHold = argDouble(args, "stationary_hold_s", _stationaryHold);
        _stationaryFall = argDouble(args, "stationary_fall_s", _stationaryFall);
        _stationaryOffsetHz = argDouble(args, "stationary_offset_hz", _stationaryOffsetHz);
        _stationaryAmp = argDouble(args, "stationary_amplitude", _stationaryAmp);
        _chirpEnabled = argBool(args, "chirp_enabled", _chirpEnabled);
        _chirpStart = argDouble(args, "chirp_start_s", _chirpStart);
        _chirpDuration = argDouble(args, "chirp_duration_s", _chirpDuration);
        _chirpStartOffsetHz = argDouble(args, "chirp_start_offset_hz", _chirpStartOffsetHz);
        _chirpEndOffsetHz = argDouble(args, "chirp_end_offset_hz", _chirpEndOffsetHz);
        _chirpAmp = argDouble(args, "chirp_amplitude", _chirpAmp);

        validateSignalConfig();

        std::ostringstream msg;
        msg << "Meteoris simulator " << METEORIS_SIM_VERSION
            << ": period=" << _eventPeriod << "s"
            << " noise_amp=" << _noiseAmp
            << " stationary=" << (_stationaryEnabled ? "on" : "off")
            << " start=" << _stationaryStart << "s"
            << " rise=" << _stationaryRise << "s"
            << " hold=" << _stationaryHold << "s"
            << " fall=" << _stationaryFall << "s"
            << " offset=" << _stationaryOffsetHz << "Hz"
            << " amp=" << _stationaryAmp
            << " chirp=" << (_chirpEnabled ? "on" : "off")
            << " start=" << _chirpStart << "s"
            << " duration=" << _chirpDuration << "s"
            << " offsets=" << _chirpStartOffsetHz << "->" << _chirpEndOffsetHz << "Hz"
            << " amp=" << _chirpAmp;
        SoapySDR::log(SOAPY_SDR_INFO, msg.str());
    }

    std::string getDriverKey(void) const override { return "meteoris_sim"; }
    std::string getHardwareKey(void) const override { return "Meteoris synthetic IQ source"; }
    SoapySDR::Kwargs getHardwareInfo(void) const override
    {
        return {
            {"origin", "synthetic"},
            {"native_format", "CS8"},
            {"sim_version", METEORIS_SIM_VERSION},
            {"signal", "finite near-zero stationary echo + one clean periodic linear chirp"}
        };
    }

    size_t getNumChannels(const int direction) const override
    {
        return direction == SOAPY_SDR_RX ? 1 : 0;
    }
    bool getFullDuplex(const int, const size_t) const override { return false; }

    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const override
    {
        if (direction != SOAPY_SDR_RX || channel != 0) return {};
        return {SOAPY_SDR_CS8};
    }

    std::string getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const override
    {
        if (direction != SOAPY_SDR_RX || channel != 0) return "";
        fullScale = 128.0;
        return SOAPY_SDR_CS8;
    }

    SoapySDR::Stream *setupStream(const int direction,
                                  const std::string &format,
                                  const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs &args) override
    {
        if (direction != SOAPY_SDR_RX) throw std::runtime_error("meteoris_sim is RX-only");
        if (format != SOAPY_SDR_CS8) throw std::runtime_error("meteoris_sim supports CS8 only");
        if (!channels.empty() && (channels.size() != 1 || channels.front() != 0))
            throw std::runtime_error("meteoris_sim supports RX channel 0 only");

        size_t nbuf = 16;
        auto it = args.find("buffers");
        if (it != args.end()) nbuf = std::max<size_t>(2, std::stoul(it->second));

        auto *s = new SimStream;
        s->buffers.resize(nbuf);
        for (auto &b : s->buffers) b.resize(2 * s->mtu);
        return reinterpret_cast<SoapySDR::Stream *>(s);
    }

    void closeStream(SoapySDR::Stream *stream) override
    {
        delete reinterpret_cast<SimStream *>(stream);
    }

    size_t getStreamMTU(SoapySDR::Stream *stream) const override
    {
        return reinterpret_cast<SimStream *>(stream)->mtu;
    }

    int activateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs, const size_t numElems) override
    {
        (void)flags; (void)timeNs; (void)numElems;
        auto *s = reinterpret_cast<SimStream *>(stream);
        s->active = true;
        s->sampleIndex = 0;
        s->nextHandle = 0;
        s->stationary = {1.0, 0.0};
        s->chirp = {1.0, 0.0};
        s->chirpStep = {1.0, 0.0};
        s->chirpStepDelta = {1.0, 0.0};
        s->chirpWasActive = false;
        s->epoch = std::chrono::steady_clock::now();
        return 0;
    }

    int deactivateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs) override
    {
        (void)flags; (void)timeNs;
        reinterpret_cast<SimStream *>(stream)->active = false;
        return 0;
    }

    size_t getNumDirectAccessBuffers(SoapySDR::Stream *stream) override
    {
        return reinterpret_cast<SimStream *>(stream)->buffers.size();
    }

    int getDirectAccessBufferAddrs(SoapySDR::Stream *stream, const size_t handle, void **buffs) override
    {
        auto *s = reinterpret_cast<SimStream *>(stream);
        if (handle >= s->buffers.size()) return SOAPY_SDR_STREAM_ERROR;
        buffs[0] = s->buffers[handle].data();
        return 0;
    }

    int acquireReadBuffer(SoapySDR::Stream *stream,
                          size_t &handle,
                          const void **buffs,
                          int &flags,
                          long long &timeNs,
                          const long timeoutUs) override
    {
        auto *s = reinterpret_cast<SimStream *>(stream);
        if (!s->active) return SOAPY_SDR_STREAM_ERROR;

        // Real-time pacing: a buffer becomes available when its last sample
        // would have arrived at the configured sample rate.
        const uint64_t nextEndSample = s->sampleIndex + s->mtu;
        const double targetSec = double(nextEndSample) / _sampleRate;
        const auto target = s->epoch + std::chrono::nanoseconds(static_cast<long long>(targetSec * 1e9));
        const auto now = std::chrono::steady_clock::now();
        if (target > now)
        {
            const auto wait = target - now;
            if (timeoutUs >= 0 && wait > std::chrono::microseconds(timeoutUs))
                return SOAPY_SDR_TIMEOUT;
            std::this_thread::sleep_until(target);
        }

        handle = s->nextHandle;
        s->nextHandle = (s->nextHandle + 1) % s->buffers.size();
        generate(*s, s->buffers[handle].data(), s->mtu);

        buffs[0] = s->buffers[handle].data();
        flags = SOAPY_SDR_HAS_TIME;
        timeNs = static_cast<long long>(std::llround(1e9 * double(s->sampleIndex) / _sampleRate));
        s->sampleIndex += s->mtu;
        return static_cast<int>(s->mtu);
    }

    void releaseReadBuffer(SoapySDR::Stream *, const size_t) override {}

    int readStream(SoapySDR::Stream *stream,
                   void * const *buffs,
                   const size_t numElems,
                   int &flags,
                   long long &timeNs,
                   const long timeoutUs) override
    {
        auto *s = reinterpret_cast<SimStream *>(stream);
        if (!s->active) return SOAPY_SDR_STREAM_ERROR;

        const uint64_t nextEndSample = s->sampleIndex + numElems;
        const double targetSec = double(nextEndSample) / _sampleRate;
        const auto target = s->epoch + std::chrono::nanoseconds(static_cast<long long>(targetSec * 1e9));
        const auto now = std::chrono::steady_clock::now();
        if (target > now)
        {
            const auto wait = target - now;
            if (timeoutUs >= 0 && wait > std::chrono::microseconds(timeoutUs))
                return SOAPY_SDR_TIMEOUT;
            std::this_thread::sleep_until(target);
        }

        flags = SOAPY_SDR_HAS_TIME;
        timeNs = static_cast<long long>(std::llround(1e9 * double(s->sampleIndex) / _sampleRate));
        generate(*s, static_cast<int8_t *>(buffs[0]), numElems);
        s->sampleIndex += numElems;
        return static_cast<int>(numElems);
    }

    void setSampleRate(const int direction, const size_t channel, const double rate) override
    {
        checkRx(direction, channel);
        if (rate < 100e3 || rate > 20e6) throw std::runtime_error("sample rate must be 100 kS/s .. 20 MS/s");
        _sampleRate = rate;
    }
    double getSampleRate(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return _sampleRate;
    }
    std::vector<double> listSampleRates(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return {2e6, 2.5e6, 5e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    }
    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return {SoapySDR::Range(100e3, 20e6)};
    }

    void setFrequency(const int direction, const size_t channel, const double frequency, const SoapySDR::Kwargs &) override
    {
        checkRx(direction, channel); _frequency = frequency;
    }
    double getFrequency(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return _frequency;
    }
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return {SoapySDR::Range(0.0, 6e9)};
    }

    void setBandwidth(const int direction, const size_t channel, const double bw) override
    {
        checkRx(direction, channel); _bandwidth = bw;
    }
    double getBandwidth(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return _bandwidth;
    }
    SoapySDR::RangeList getBandwidthRange(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return {SoapySDR::Range(50e3, 20e6)};
    }

    bool hasGainMode(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return false;
    }
    void setGain(const int direction, const size_t channel, const double value) override
    {
        checkRx(direction, channel); _gainDb = std::max(0.0, std::min(60.0, value));
    }
    double getGain(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return _gainDb;
    }
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override
    {
        checkRx(direction, channel); return SoapySDR::Range(0.0, 60.0, 1.0);
    }

private:
    static double argDouble(const SoapySDR::Kwargs &args, const std::string &key, const double fallback)
    {
        const auto it = args.find(key);
        return it == args.end() ? fallback : std::stod(it->second);
    }

    static bool argBool(const SoapySDR::Kwargs &args, const std::string &key, const bool fallback)
    {
        const auto it = args.find(key);
        if (it == args.end()) return fallback;
        if (it->second == "true" || it->second == "1") return true;
        if (it->second == "false" || it->second == "0") return false;
        throw std::runtime_error("invalid boolean simulator argument " + key + "=" + it->second);
    }

    void validateSignalConfig() const
    {
        if (!(_eventPeriod > 0.0)) throw std::runtime_error("period_s must be > 0");
        if (_noiseAmp < 0.0 || _stationaryAmp < 0.0 || _chirpAmp < 0.0)
            throw std::runtime_error("simulator amplitudes must be >= 0");
        if (_stationaryStart < 0.0 || _stationaryRise < 0.0 ||
            _stationaryHold < 0.0 || _stationaryFall < 0.0)
            throw std::runtime_error("stationary timing must be >= 0");
        if (_stationaryEnabled &&
            _stationaryStart + _stationaryRise + _stationaryHold + _stationaryFall > _eventPeriod)
            throw std::runtime_error("stationary event exceeds period_s");
        if (_chirpStart < 0.0 || !(_chirpDuration > 0.0))
            throw std::runtime_error("chirp timing is invalid");
        if (_chirpEnabled && _chirpStart + _chirpDuration > _eventPeriod)
            throw std::runtime_error("chirp event exceeds period_s");
    }

    static void checkRx(const int direction, const size_t channel)
    {
        if (direction != SOAPY_SDR_RX || channel != 0)
            throw std::runtime_error("meteoris_sim supports RX channel 0 only");
    }

    static uint32_t xorshift32(uint32_t &x)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x;
    }

    static int8_t quantizeCs8(const double x, const uint32_t randomWord)
    {
        // Stochastic rounding makes the CS8 quantization error noise-like
        // instead of a deterministic memoryless non-linearity. This is
        // important when strong stationary and chirp signals are present together:
        // deterministic rounding can otherwise create coherent IM products
        // at translated_center+n*chirp_offset that appear as parallel chirps.
        const double limited = std::max(-127.0, std::min(127.0, x));
        const double lo = std::floor(limited);
        const double frac = limited - lo;
        const double u = double(randomWord & 0x00ffffffu) / 16777216.0;
        int q = static_cast<int>(lo) + ((u < frac) ? 1 : 0);
        q = std::max(-127, std::min(127, q));
        return static_cast<int8_t>(q);
    }

    void generate(SimStream &s, int8_t *dst, const size_t count)
    {
        // The normal Meteoris setup shifts -1 MHz, so signals generated around
        // +1 MHz appear around 0 Hz in the final PSD. Unlike the earlier
        // simulator, there is NO permanent carrier: the stationary component
        // is a finite rise/hold/fade event and the chirp is a separate event.
        const double translatedCenterHz = 1.0e6;

        // Gain is only a simulator amplitude control. 20 dB is unity relative
        // to the configured amplitudes below. Stochastic CS8 quantization is
        // retained to avoid coherent intermodulation replicas.
        const double gainScale = std::pow(10.0, (_gainDb - 20.0) / 20.0);
        const double stationaryAmp = _stationaryAmp * gainScale;
        const double chirpAmp = _chirpAmp * gainScale;
        const double noiseAmp = _noiseAmp * gainScale;

        const double stationaryHz = translatedCenterHz + _stationaryOffsetHz;
        const double stationaryW = 2.0 * PI * stationaryHz / _sampleRate;
        const std::complex<double> stationaryStep(std::cos(stationaryW), std::sin(stationaryW));

        for (size_t n = 0; n < count; ++n)
        {
            const uint64_t absolute = s.sampleIndex + n;
            const double t = double(absolute) / _sampleRate;
            const double ep = std::fmod(t, _eventPeriod);
            std::complex<double> v(0.0, 0.0);

            // Finite near-zero stationary echo: linear attack, flat hold and
            // linear decay. The oscillator continues through the period so no
            // phase discontinuity is introduced at the envelope boundaries.
            if (_stationaryEnabled)
            {
                const double stationaryEnd = _stationaryStart + _stationaryRise +
                    _stationaryHold + _stationaryFall;
                if (ep >= _stationaryStart && ep < stationaryEnd)
                {
                    const double x = ep - _stationaryStart;
                    double env = 1.0;
                    if (_stationaryRise > 0.0 && x < _stationaryRise)
                        env = x / _stationaryRise;
                    else if (x >= _stationaryRise + _stationaryHold && _stationaryFall > 0.0)
                        env = std::max(0.0, (stationaryEnd - ep) / _stationaryFall);
                    v += stationaryAmp * env * s.stationary;
                }
            }
            s.stationary *= stationaryStep;

            const bool chirpActive = _chirpEnabled &&
                ep >= _chirpStart && ep < (_chirpStart + _chirpDuration);
            if (chirpActive)
            {
                if (!s.chirpWasActive)
                {
                    // Linear chirp: phase increment changes by a constant each
                    // sample. Compute trig only at event start.
                    s.chirp = {1.0, 0.0};
                    const double slopeHzPerSec =
                        (_chirpEndOffsetHz - _chirpStartOffsetHz) / _chirpDuration;
                    const double w0 = 2.0 * PI *
                        (translatedCenterHz + _chirpStartOffsetHz) / _sampleRate;
                    const double dw = 2.0 * PI * slopeHzPerSec /
                        (_sampleRate * _sampleRate);
                    s.chirpStep = {std::cos(w0), std::sin(w0)};
                    s.chirpStepDelta = {std::cos(dw), std::sin(dw)};
                }

                // Short attack/release keeps the chirp spectrally clean.
                const double edge = std::min(0.10, _chirpDuration * 0.20);
                const double sinceOn = ep - _chirpStart;
                const double untilOff = _chirpStart + _chirpDuration - ep;
                double env = 1.0;
                if (edge > 0.0 && sinceOn < edge) env = std::min(env, sinceOn / edge);
                if (edge > 0.0 && untilOff < edge) env = std::min(env, untilOff / edge);
                v += chirpAmp * env * s.chirp;

                s.chirp *= s.chirpStep;
                s.chirpStep *= s.chirpStepDelta;
            }
            s.chirpWasActive = chirpActive;

            // Cheap approximately white analog noise. Reuse the lower 24
            // random bits as the stochastic-quantizer dither, so spectral
            // cleanliness improves without adding two more PRNG calls/sample.
            const uint32_t u1 = xorshift32(s.rng);
            const uint32_t u2 = xorshift32(s.rng);
            const int r1 = int((u1 >> 24) & 0xff) - 128;
            const int r2 = int((u2 >> 24) & 0xff) - 128;
            v += std::complex<double>(noiseAmp * r1 / 128.0, noiseAmp * r2 / 128.0);

            dst[2*n] = quantizeCs8(v.real(), u1);
            dst[2*n + 1] = quantizeCs8(v.imag(), u2);
        }

        // Keep recursive oscillators on the unit circle with negligible cost.
        const double sm0 = std::abs(s.stationary);
        if (sm0 > 0.0) s.stationary /= sm0;
        if (s.chirpWasActive)
        {
            const double zm = std::abs(s.chirp);
            const double sm = std::abs(s.chirpStep);
            if (zm > 0.0) s.chirp /= zm;
            if (sm > 0.0) s.chirpStep /= sm;
        }
    }

    double _sampleRate = 10e6;
    double _frequency = 143.051460e6;
    double _bandwidth = 10e6;
    double _gainDb = 20.0;

    double _eventPeriod = 8.0;
    double _noiseAmp = 5.0;
    bool _stationaryEnabled = true;
    double _stationaryStart = 0.8;
    double _stationaryRise = 0.35;
    double _stationaryHold = 1.0;
    double _stationaryFall = 0.65;
    double _stationaryOffsetHz = 0.0;
    double _stationaryAmp = 3.0;
    bool _chirpEnabled = true;
    double _chirpStart = 4.0;
    double _chirpDuration = 1.2;
    double _chirpStartOffsetHz = 38e3;
    double _chirpEndOffsetHz = 12e3;
    double _chirpAmp = 6.0;
};

SoapySDR::KwargsList findMeteorisSim(const SoapySDR::Kwargs &args)
{
    const auto it = args.find("driver");
    if (it != args.end() && it->second != "meteoris_sim") return {};
    return {{{"driver", "meteoris_sim"}, {"label", "Meteoris synthetic CS8 source"}}};
}

SoapySDR::Device *makeMeteorisSim(const SoapySDR::Kwargs &args)
{
    return new MeteorisSim(args);
}

static SoapySDR::Registry registerMeteorisSim(
    "meteoris_sim",
    &findMeteorisSim,
    &makeMeteorisSim,
    SOAPY_SDR_ABI_VERSION);
}
