/*
 * .+
 * .context    : soapy hackrf-htime driver
 * .title      : RX + frequency shift + 40:1 polyphase FIR + PSD benchmark
 * .kind       : C++ test program
 * .author     : Fabrizio Pollastri <mxgbot@gmail.com>
 * .site       : Revello - Italy
 * .creation   : 2026-08-22
 * .license    : MIT (see LICENSE file)
 * .description
 * Benchmark the complete real-time DSP chain:
 *   10 Msps native CS8 RX via direct-access buffers
 *     -> complex frequency translation by exp(+j*omega*n)
 *     -> 8:1 FIR decimator
 *     -> 5:1 FIR decimator
 *     -> 250 ksps complex output (40:1 total)
 *     -> Welch PSD of the final 100 kHz band (-50 kHz .. +50 kHz)
 *
 * FIR work is computed only at decimated output instants; this is the same
 * arithmetic reduction obtained by a polyphase decimator, without filtering
 * samples that are immediately discarded. Native CS8 is consumed directly
 * from Soapy driver buffers when direct access is available. FFT is an in-place radix-2 FFT,
 * so this benchmark has no external DSP/FFT dependency beyond SoapySDR.
 * .-
 */

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <hdf5.h>
#include <memory>
#include <cstdio>
#include <fcntl.h>
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#include "detector/detector.hpp"

#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#include <limits>

namespace
{
constexpr const char *METEORIS_VERSION = "0.1.0";
constexpr const char *METEORIS_AUTHOR = "Fabrizio Pollastri <mxgbot@gmail.com>";
constexpr const char *METEORIS_HDF5_FORMAT = "meteoris v0";
constexpr const char *METEORIS_HDF5_AUTHOR = "Fabrizio Pollastri";

constexpr double PI = 3.141592653589793238462643383279502884;
constexpr double DEFAULT_INPUT_RATE = 10e6;
constexpr size_t DEFAULT_DECIM_1 = 8;
constexpr size_t DEFAULT_DECIM_2 = 5;
constexpr double DEFAULT_BANDWIDTH = 100e3;
constexpr double DEFAULT_ATTENUATION_DB = 60.0;

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gForceStopRequested = 0;
volatile std::sig_atomic_t gStopSignal = 0;

extern "C" void meteorisSignalHandler(int signo)
{
    gStopSignal = signo;
    if (gStopRequested)
        gForceStopRequested = 1;  // second signal: force immediate shutdown
    else
        gStopRequested = 1;       // first signal: finish current event
}

void installSignalHandlers()
{
    std::signal(SIGINT, meteorisSignalHandler);
    std::signal(SIGTERM, meteorisSignalHandler);
}

#define METEORIS_LOG_STREAM(level, expr) \
    do { std::ostringstream _meteorisLogStream; _meteorisLogStream << expr; \
         spdlog::log(level, "{}", _meteorisLogStream.str()); } while (0)
#define LOG_INFO_STREAM(expr)  METEORIS_LOG_STREAM(spdlog::level::info, expr)
#define LOG_DEBUG_STREAM(expr) METEORIS_LOG_STREAM(spdlog::level::debug, expr)
#define LOG_WARN_STREAM(expr)  METEORIS_LOG_STREAM(spdlog::level::warn, expr)
#define LOG_ERROR_STREAM(expr) METEORIS_LOG_STREAM(spdlog::level::err, expr)

spdlog::level::level_enum parseLogLevel(const std::string &value)
{
    std::string s(value);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "trace") return spdlog::level::trace;
    if (s == "debug") return spdlog::level::debug;
    if (s == "info") return spdlog::level::info;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "error" || s == "err") return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    if (s == "off") return spdlog::level::off;
    throw std::runtime_error("logging.level must be one of trace, debug, info, warn, error, critical, off");
}

struct Config
{
    std::string driver = "hackrf-htime";
    double sampleRate = DEFAULT_INPUT_RATE;
    double centerFrequency = 0.0;
    double gain = -1.0;
    double runSeconds = 0.0; // 0 = run until SIGINT/SIGTERM
    double shiftHz = 1e6;
    size_t decim1 = DEFAULT_DECIM_1;
    size_t decim2 = DEFAULT_DECIM_2;
    double bandwidth = DEFAULT_BANDWIDTH;
    double attenuationDb = DEFAULT_ATTENUATION_DB;
    size_t nfft = 4096;
    size_t streamBuffers = 16;
    bool direct = true;
    size_t fir1Threads = 1; // 1 or 2; FIR1 block kernel workers
    bool daemonMode = false;
    std::string logLevel = "info";
    bool logColor = true;
    // File logging is enabled by default in foreground and daemon mode.
    // The path is relative to the process launch directory unless absolute.
    bool logFileEnabled = true;
    std::string logFile = "meteoris.log";
    bool logFileTruncate = false;
    // Daily text-log rotation boundary, minutes after 00:00 UTC.
    bool logDailyRotation = true;
    int logDailyRotateMinuteUtc = 0;
    std::string configPath;

    // Optional parameters for the meteoris_sim Soapy driver. They are ignored
    // by real SDR drivers. The simulator generates two independent finite
    // events per period: a near-zero stationary echo and a linear chirp.
    double simulatorPeriodSeconds = 8.0;
    double simulatorNoiseAmplitude = 5.0;
    bool simulatorStationaryEnabled = true;
    double simulatorStationaryStartSeconds = 0.8;
    double simulatorStationaryRiseSeconds = 0.35;
    double simulatorStationaryHoldSeconds = 1.0;
    double simulatorStationaryFallSeconds = 0.65;
    double simulatorStationaryOffsetHz = 0.0;
    double simulatorStationaryAmplitude = 3.0;
    bool simulatorChirpEnabled = true;
    double simulatorChirpStartSeconds = 4.0;
    double simulatorChirpDurationSeconds = 1.2;
    double simulatorChirpStartOffsetHz = 38e3;
    double simulatorChirpEndOffsetHz = 12e3;
    double simulatorChirpAmplitude = 6.0;

    // Recording controller mode. "triggered" uses the selected detector;
    // "continuous" records bounded PSD sequences and ignores detector state.
    std::string recordingMode = "triggered";
    double recordingSegmentSeconds = 15.0;
    uint64_t recordingSegmentCount = 0; // 0 = unlimited

    // Compile-time registered detector plugin selected at runtime.
    bool detectorEnabled = true;
    std::string detectorPlugin = "peak_tracker";
    // 0 lets the detector choose; phase-1 peak_tracker is currently serial.
    unsigned detectorThreads = 1;

    // Current peak_tracker plugin parameters.
    size_t detectorFrequencyMeanBins = 5; // odd: 3,5,7,...
    size_t detectorTimeMeanPsds = 3;      // causal rolling mean
    double detectorPeakThresholdDb = 5.0; // above current blurred PSD median
    double detectorMinPeakSeparationHz = 300.0;
    size_t detectorMaxPeaksPerPsd = 20;
    double detectorDiagnosticIntervalSeconds = 1.0;

    bool detectorStationaryEnabled = true;
    double detectorStationaryMinHz = -5000.0;
    double detectorStationaryMaxHz = 5000.0;
    double detectorStationaryMaxDfHz = 250.0;
    double detectorStationaryActivationSeconds = 0.5;
    double detectorStationaryActivationFraction = 0.50;
    double detectorStationaryLostSeconds = 0.30;
    double detectorStationaryMaxDriftHzS = 1000.0;

    bool detectorChirpEnabled = true;
    double detectorChirpMinHz = -50000.0;
    double detectorChirpMaxHz = 50000.0;
    double detectorChirpMaxDfHz = 1500.0;
    double detectorChirpActivationSeconds = 0.05;
    double detectorChirpActivationFraction = 0.70;
    double detectorChirpLostSeconds = 0.08;
    double detectorChirpMinDriftHzS = 3000.0;
    double detectorChirpMaxDriftHzS = 150000.0;

    // Recording/event parameters remain common to all active tracks.
    double detectorPreContextSeconds = 0.5;
    double detectorPostContextSeconds = 1.0;
    // 0 disables forced event cutoff. Rearm applies only after a forced cutoff.
    double detectorMaxEventSeconds = 15.0;
    double detectorRearmSeconds = 1.0;
    // Keep compiled defaults consistent with config/meteoris.toml so a run
    // without a local config writes beneath the working directory, not into it.
    std::string detectorOutputDirectory = "./data";
    std::string detectorFilePrefix = "meteoris";
    int detectorCompression = 1;
    // Enable HDF5 Single-Writer/Multiple-Reader mode for live readers.
    bool outputSwmr = true;
    // Publish dirty HDF5 data to SWMR readers at this interval. 0 = every append.
    double outputSwmrFlushSeconds = 1.0;
    // Daily HDF5 rotation boundary, minutes after 00:00 UTC.
    int outputDailyRotateMinuteUtc = 0;
    // Safety limits for long-running recording. 0 disables the corresponding check.
    double outputMaxGrowthMbPerMin = 0.0;
    double outputMinFreeGb = 1.0;

    // Low-cost ADC-clipping AGC. Saturation is measured directly on native
    // CS8 I/Q samples; PSD bins themselves do not have a universal clip level.
    bool agcEnabled = false;
    double agcTargetSaturationPercent = 1.0;
    double agcTimeConstantSeconds = 2.0;
    int agcClipLevel = 126;
    double agcMinGain = -1.0; // <0 means use device range
    double agcMaxGain = -1.0; // <0 means use device range
    double agcMaxStepDb = 1.0;
};

double besselI0(const double x)
{
    // Fast approximation used by common Kaiser-window implementations.
    const double ax = std::abs(x);
    if (ax < 3.75)
    {
        const double y = (x / 3.75) * (x / 3.75);
        return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 +
               y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
    }
    const double y = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax)) *
           (0.39894228 + y * (0.01328592 + y * (0.00225319 +
            y * (-0.00157565 + y * (0.00916281 + y * (-0.02057706 +
            y * (0.02635537 + y * (-0.01647633 + y * 0.00392377))))))));
}

double kaiserBeta(const double attenuation)
{
    if (attenuation > 50.0) return 0.1102 * (attenuation - 8.7);
    if (attenuation >= 21.0)
    {
        const double a = attenuation - 21.0;
        return 0.5842 * std::pow(a, 0.4) + 0.07886 * a;
    }
    return 0.0;
}

std::vector<float> designLowpass(
    const double fs,
    const double passHz,
    const double stopHz,
    const double attenuationDb)
{
    if (!(0.0 < passHz && passHz < stopHz && stopHz < fs / 2.0))
        throw std::runtime_error("invalid FIR edge frequencies");

    const double transition = stopHz - passHz;
    const double deltaOmega = 2.0 * PI * transition / fs;
    size_t taps = static_cast<size_t>(
        std::ceil((attenuationDb - 8.0) / (2.285 * deltaOmega))) + 1;
    taps = std::max<size_t>(15, taps);
    if ((taps & 1u) == 0) ++taps;

    const double beta = kaiserBeta(attenuationDb);
    const double denom = besselI0(beta);
    const double cutoff = 0.5 * (passHz + stopHz);
    const double fc = cutoff / fs;
    const double mid = 0.5 * double(taps - 1);

    std::vector<float> h(taps);
    double sum = 0.0;
    for (size_t n = 0; n < taps; ++n)
    {
        const double m = double(n) - mid;
        const double sinc = (std::abs(m) < 1e-15)
            ? 2.0 * fc
            : std::sin(2.0 * PI * fc * m) / (PI * m);
        const double r = (double(n) - mid) / mid;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
        h[n] = static_cast<float>(sinc * w);
        sum += h[n];
    }
    for (auto &v : h) v = static_cast<float>(v / sum);
    return h;
}

class FirDecimator
{
public:
    FirDecimator(std::vector<float> taps, const size_t decimation)
        : _tapsReversed(taps.rbegin(), taps.rend()),
          _delay(2 * taps.size(), std::complex<float>(0.0f, 0.0f)),
          _length(taps.size()), _decimation(decimation)
    {
        if (_tapsReversed.empty() || _decimation == 0)
            throw std::runtime_error("invalid FIR decimator");
    }

    // Push one sample and return true only when a decimated FIR output exists.
    // A countdown replaces inputCount % decimation in the hot path.
    inline bool push(const std::complex<float> sample, std::complex<float> &out)
    {
        const size_t length = _length;
        _delay[_write] = sample;
        _delay[_write + length] = sample;
        if (++_write == length) _write = 0;

        if (_countdown != 0)
        {
            --_countdown;
            return false;
        }
        _countdown = _decimation - 1;

        const std::complex<float> *history = _delay.data() + _write;
        float accRe = 0.0f;
        float accIm = 0.0f;
        // Real taps permit two independent real dot products. This vectorizes
        // more reliably than std::complex<float> accumulation with -O3.
        for (size_t k = 0; k < length; ++k)
        {
            const float h = _tapsReversed[k];
            accRe += history[k].real() * h;
            accIm += history[k].imag() * h;
        }
        out = std::complex<float>(accRe, accIm);
        return true;
    }

    size_t taps() const { return _tapsReversed.size(); }

private:
    std::vector<float> _tapsReversed;
    std::vector<std::complex<float>> _delay;
    size_t _length = 0;
    size_t _decimation = 1;
    size_t _write = 0;
    size_t _countdown = 0;
};

class NcoMixer
{
public:
    NcoMixer(const double shiftHz, const double sampleRate)
    {
        const double turns = shiftHz / sampleRate;
        // Many SDR offsets are exact rational fractions of Fs. In that case
        // a tiny LUT removes the per-input-sample oscillator recurrence. For
        // example -1 MHz at 10 Msps has an exact 10-sample period.
        for (size_t p = 1; p <= 4096; ++p)
        {
            const double q = turns * double(p);
            if (std::abs(q - std::round(q)) < 1e-12)
            {
                _lut.resize(p);
                for (size_t n = 0; n < p; ++n)
                {
                    const double a = 2.0 * PI * turns * double(n);
                    _lut[n] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
                }
                return;
            }
        }

        const double omega = 2.0 * PI * turns;
        _step = {static_cast<float>(std::cos(omega)), static_cast<float>(std::sin(omega))};
    }

    void mixBlockCs8(const int8_t *iq, const size_t count, float *outRe, float *outIm,
                     uint64_t *saturatedComplex = nullptr, const int clipLevel = 126)
    {
        uint64_t clipped = 0;
        if (_lut.size() == 1 && _lut[0].real() == 1.0f && _lut[0].imag() == 0.0f)
        {
            // Zero shift: avoid four floating-point multiplies/adds per input
            // sample. Keep clipping accounting in the same pass.
            for (size_t n = 0; n < count; ++n)
            {
                const int i8 = static_cast<int>(iq[2 * n]);
                const int q8 = static_cast<int>(iq[2 * n + 1]);
                if (saturatedComplex &&
                    (i8 >= clipLevel || i8 <= -clipLevel ||
                     q8 >= clipLevel || q8 <= -clipLevel))
                    ++clipped;
                outRe[n] = static_cast<float>(i8);
                outIm[n] = static_cast<float>(q8);
            }
        }
        else if (!_lut.empty())
        {
            size_t idx = _index;
            const size_t period = _lut.size();
            for (size_t n = 0; n < count; ++n)
            {
                const int i8 = static_cast<int>(iq[2 * n]);
                const int q8 = static_cast<int>(iq[2 * n + 1]);
                if (saturatedComplex &&
                    (i8 >= clipLevel || i8 <= -clipLevel || q8 >= clipLevel || q8 <= -clipLevel))
                    ++clipped;
                const float i = static_cast<float>(i8);
                const float q = static_cast<float>(q8);
                const float c = _lut[idx].real();
                const float s = _lut[idx].imag();
                outRe[n] = i * c - q * s;
                outIm[n] = i * s + q * c;
                if (++idx == period) idx = 0;
            }
            _index = idx;
        }
        else
        {
            std::complex<float> phase = _phase;
            size_t renorm = _renorm;
            for (size_t n = 0; n < count; ++n)
            {
                const int i8 = static_cast<int>(iq[2 * n]);
                const int q8 = static_cast<int>(iq[2 * n + 1]);
                if (saturatedComplex &&
                    (i8 >= clipLevel || i8 <= -clipLevel || q8 >= clipLevel || q8 <= -clipLevel))
                    ++clipped;
                const float i = static_cast<float>(i8);
                const float q = static_cast<float>(q8);
                const float c = phase.real();
                const float s = phase.imag();
                outRe[n] = i * c - q * s;
                outIm[n] = i * s + q * c;
                phase *= _step;
                if ((++renorm & 16383u) == 0)
                {
                    const float m2 = std::norm(phase);
                    if (m2 > 0.0f) phase *= 1.0f / std::sqrt(m2);
                }
            }
            _phase = phase;
            _renorm = renorm;
        }
        if (saturatedComplex) *saturatedComplex = clipped;
    }

    bool usesLut() const { return !_lut.empty(); }
    size_t lutPeriod() const { return _lut.size(); }

private:
    std::vector<std::complex<float>> _lut;
    size_t _index = 0;
    std::complex<float> _phase{1.0f, 0.0f};
    std::complex<float> _step{1.0f, 0.0f};
    size_t _renorm = 0;
};

class BlockFir1Decimator
{
public:
    BlockFir1Decimator(const std::vector<float> &taps, const size_t decimation,
                       const size_t threads)
        : _tapsReversed(taps.rbegin(), taps.rend()),
          _historyRe(taps.size() > 0 ? taps.size() - 1 : 0, 0.0f),
          _historyIm(taps.size() > 0 ? taps.size() - 1 : 0, 0.0f),
          _decimation(decimation), _threads(threads)
    {
        if (_tapsReversed.empty() || _decimation == 0 || (_threads != 1 && _threads != 2))
            throw std::runtime_error("invalid FIR1 block decimator");
        if (_threads == 2) _worker = std::thread(&BlockFir1Decimator::workerLoop, this);
    }

    ~BlockFir1Decimator()
    {
        if (_worker.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _stopWorker = true;
            }
            _cvJob.notify_one();
            _worker.join();
        }
    }

    BlockFir1Decimator(const BlockFir1Decimator &) = delete;
    BlockFir1Decimator &operator=(const BlockFir1Decimator &) = delete;

    // Mix one native-CS8 block into a structure-of-arrays buffer with FIR
    // history prepended. FIR1 outputs are then independent dot products at
    // decimated instants. This removes the sample-by-sample FIR state machine,
    // gives SIMD contiguous float arrays, and permits two workers to split the
    // output range without any filter-state synchronization.
    size_t processCs8(NcoMixer &mixer, const int8_t *iq, const size_t count,
                      std::vector<std::complex<float>> &out,
                      uint64_t *saturatedComplex = nullptr, const int clipLevel = 126)
    {
        const size_t hist = _historyRe.size();
        _blockRe.resize(hist + count);
        _blockIm.resize(hist + count);
        if (hist != 0)
        {
            std::copy(_historyRe.begin(), _historyRe.end(), _blockRe.begin());
            std::copy(_historyIm.begin(), _historyIm.end(), _blockIm.begin());
        }

        mixer.mixBlockCs8(iq, count, _blockRe.data() + hist, _blockIm.data() + hist,
                          saturatedComplex, clipLevel);

        const size_t firstLocal = static_cast<size_t>(_nextOutputIndex - _inputIndex);
        size_t nout = 0;
        if (firstLocal < count)
            nout = 1 + (count - 1 - firstLocal) / _decimation;
        out.resize(nout);
        _jobOut = &out;

        if (nout != 0)
        {
            if (_threads == 2 && nout >= 512)
            {
                const size_t mid = nout / 2;
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _jobBegin = mid;
                    _jobEnd = nout;
                    _jobFirstLocal = firstLocal;
                    _jobDone = false;
                    _jobReady = true;
                }
                _cvJob.notify_one();
                computeRange(out, 0, mid, firstLocal);
                std::unique_lock<std::mutex> lock(_mutex);
                _cvDone.wait(lock, [this] { return _jobDone; });
            }
            else computeRange(out, 0, nout, firstLocal);
            _nextOutputIndex += uint64_t(nout) * uint64_t(_decimation);
        }

        _inputIndex += count;
        if (hist != 0)
        {
            // The last 'hist' samples begin at index count in the concatenated
            // [old history | current block] array, even for a short final block.
            std::copy(_blockRe.begin() + count, _blockRe.begin() + count + hist,
                      _historyRe.begin());
            std::copy(_blockIm.begin() + count, _blockIm.begin() + count + hist,
                      _historyIm.begin());
        }
        return nout;
    }

    size_t taps() const { return _tapsReversed.size(); }
    size_t threads() const { return _threads; }

private:
    inline std::complex<float> dotComplex(const float *re, const float *im) const
    {
        const float *h = _tapsReversed.data();
        const size_t n = _tapsReversed.size();
        size_t k = 0;
        float accRe = 0.0f, accIm = 0.0f;

#if defined(__AVX2__)
        __m256 vr = _mm256_setzero_ps();
        __m256 vi = _mm256_setzero_ps();
        for (; k + 8 <= n; k += 8)
        {
            const __m256 vh = _mm256_loadu_ps(h + k);
            const __m256 xr = _mm256_loadu_ps(re + k);
            const __m256 xi = _mm256_loadu_ps(im + k);
#if defined(__FMA__)
            vr = _mm256_fmadd_ps(xr, vh, vr);
            vi = _mm256_fmadd_ps(xi, vh, vi);
#else
            vr = _mm256_add_ps(vr, _mm256_mul_ps(xr, vh));
            vi = _mm256_add_ps(vi, _mm256_mul_ps(xi, vh));
#endif
        }
        alignas(32) float tr[8], ti[8];
        _mm256_store_ps(tr, vr);
        _mm256_store_ps(ti, vi);
        for (size_t j = 0; j < 8; ++j) { accRe += tr[j]; accIm += ti[j]; }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        float32x4_t vr = vdupq_n_f32(0.0f);
        float32x4_t vi = vdupq_n_f32(0.0f);
        for (; k + 4 <= n; k += 4)
        {
            const float32x4_t vh = vld1q_f32(h + k);
            vr = vmlaq_f32(vr, vld1q_f32(re + k), vh);
            vi = vmlaq_f32(vi, vld1q_f32(im + k), vh);
        }
#if defined(__aarch64__)
        accRe += vaddvq_f32(vr);
        accIm += vaddvq_f32(vi);
#else
        float32x2_t sr = vadd_f32(vget_low_f32(vr), vget_high_f32(vr));
        float32x2_t si = vadd_f32(vget_low_f32(vi), vget_high_f32(vi));
        sr = vpadd_f32(sr, sr);
        si = vpadd_f32(si, si);
        accRe += vget_lane_f32(sr, 0);
        accIm += vget_lane_f32(si, 0);
#endif
#endif
        for (; k < n; ++k)
        {
            const float hk = h[k];
            accRe += re[k] * hk;
            accIm += im[k] * hk;
        }
        return {accRe, accIm};
    }

    void computeRange(std::vector<std::complex<float>> &out, const size_t begin,
                      const size_t end, const size_t firstLocal) const
    {
        for (size_t j = begin; j < end; ++j)
        {
            const size_t local = firstLocal + j * _decimation;
            out[j] = dotComplex(_blockRe.data() + local, _blockIm.data() + local);
        }
    }

    void workerLoop()
    {
        for (;;)
        {
            size_t begin = 0, end = 0, firstLocal = 0;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cvJob.wait(lock, [this] { return _jobReady || _stopWorker; });
                if (_stopWorker) return;
                begin = _jobBegin;
                end = _jobEnd;
                firstLocal = _jobFirstLocal;
                _jobReady = false;
            }
            computeRange(_stage1OutRef(), begin, end, firstLocal);
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _jobDone = true;
            }
            _cvDone.notify_one();
        }
    }

    // The caller-owned output vector is registered only for the duration of a
    // processCs8() call. The main thread waits for the worker before returning.
    std::vector<std::complex<float>> &_stage1OutRef() const { return *_jobOut; }

    std::vector<float> _tapsReversed;
    std::vector<float> _historyRe, _historyIm;
    std::vector<float> _blockRe, _blockIm;
    size_t _decimation = 1;
    size_t _threads = 1;
    uint64_t _inputIndex = 0;
    uint64_t _nextOutputIndex = 0;

    std::thread _worker;
    mutable std::mutex _mutex;
    std::condition_variable _cvJob, _cvDone;
    bool _stopWorker = false, _jobReady = false, _jobDone = false;
    size_t _jobBegin = 0, _jobEnd = 0, _jobFirstLocal = 0;
    mutable std::vector<std::complex<float>> *_jobOut = nullptr;
};

class Radix2Fft
{
public:
    explicit Radix2Fft(const size_t n) : _n(n), _bitrev(n), _twiddle(n / 2)
    {
        if (_n == 0 || (_n & (_n - 1)) != 0)
            throw std::runtime_error("FFT length must be a power of two");

        unsigned bits = 0;
        for (size_t t = _n; t > 1; t >>= 1) ++bits;
        for (size_t i = 0; i < _n; ++i)
        {
            size_t x = i, r = 0;
            for (unsigned b = 0; b < bits; ++b)
            {
                r = (r << 1) | (x & 1u);
                x >>= 1;
            }
            _bitrev[i] = r;
        }
        for (size_t k = 0; k < _n / 2; ++k)
        {
            const double a = -2.0 * PI * double(k) / double(_n);
            _twiddle[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        }
    }

    void execute(std::vector<std::complex<float>> &a) const
    {
        for (size_t i = 0; i < _n; ++i)
        {
            const size_t j = _bitrev[i];
            if (i < j) std::swap(a[i], a[j]);
        }

        for (size_t len = 2; len <= _n; len <<= 1)
        {
            const size_t half = len >> 1;
            const size_t step = _n / len;
            for (size_t i = 0; i < _n; i += len)
            {
                for (size_t j = 0; j < half; ++j)
                {
                    const auto u = a[i + j];
                    const auto v = a[i + j + half] * _twiddle[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

private:
    size_t _n;
    std::vector<size_t> _bitrev;
    std::vector<std::complex<float>> _twiddle;
};

struct PsdFrame
{
    uint64_t frameIndex = 0;
    uint64_t centerSampleIndex = 0;
    uint64_t timestampNs = 0;
    std::vector<float> powerDensity; // W/Hz-like normalized PSD density
    float gainDb = 0.0f;             // SDR gain applied while this PSD was acquired
};

class WelchBandPsd
{
public:
    WelchBandPsd(const double fs, const double bandwidth, const size_t nfft)
        : _fs(fs), _bandwidth(bandwidth), _nfft(nfft), _hop(nfft / 2),
          _ring(2 * nfft, {0.0f, 0.0f}), _window(nfft), _fft(nfft),
          _powerSum(nfft, 0.0), _fftPlan(nfft)
    {
        for (size_t i = 0; i < _nfft; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * PI * double(i) / double(_nfft - 1));
            _window[i] = static_cast<float>(w);
            _windowPower += w * w;
        }

        for (size_t k = 0; k < _nfft; ++k)
        {
            const double f = binFrequency(k);
            if (std::abs(f) <= _bandwidth / 2.0) _bandBins.push_back(k);
        }
        std::sort(_bandBins.begin(), _bandBins.end(), [&](size_t a, size_t b) {
            return binFrequency(a) < binFrequency(b);
        });
        for (const size_t k : _bandBins) _frequencies.push_back(binFrequency(k));
        _psdScale = 1.0 / (_windowPower * _fs);
        _frame.powerDensity.resize(_bandBins.size());
    }

    void setStartTimestampNs(const uint64_t ns) { _startTimestampNs = ns; }
    void setCurrentGainDb(const double gainDb) { _currentGainDb = static_cast<float>(gainDb); }
    void setFrameCallback(std::function<void(const PsdFrame &)> cb) { _callback = std::move(cb); }
    const std::vector<double> &frequencies() const { return _frequencies; }

    inline void consume(const std::complex<float> sample)
    {
        _ring[_write] = sample;
        _ring[_write + _nfft] = sample;
        if (++_write == _nfft) _write = 0;
        ++_samples;

        if (_samples >= _nfft)
        {
            if (_untilFrame == 0)
            {
                accumulateFrame();
                _untilFrame = _hop - 1;
            }
            else --_untilFrame;
        }
    }

    size_t frames() const { return _frames; }

    void printSummary() const
    {
        if (_frames == 0)
        {
            spdlog::info("PSD_frames=0");
            return;
        }

        double peak = -1.0;
        double peakHz = 0.0;
        double bandPower = 0.0;
        const double binHz = _fs / double(_nfft);
        for (const size_t k : _bandBins)
        {
            const double f = binFrequency(k);
            const double p = _powerSum[k] / (double(_frames) * _windowPower * _fs);
            bandPower += p * binHz;
            if (p > peak)
            {
                peak = p;
                peakHz = f;
            }
        }
        const double peakDb = 10.0 * std::log10(std::max(peak, 1e-30));
        LOG_INFO_STREAM("PSD_frames=" << _frames);
        LOG_INFO_STREAM("PSD_peak=" << std::showpos << std::fixed << std::setprecision(3)
                        << peakHz << std::noshowpos << " Hz peak_density="
                        << std::setprecision(2) << peakDb << " dB/Hz");
        LOG_INFO_STREAM("PSD_integrated_100kHz_power=" << std::setprecision(9)
                        << bandPower);
    }

private:
    double binFrequency(const size_t k) const
    {
        const double binHz = _fs / double(_nfft);
        return (k <= _nfft / 2) ? double(k) * binHz
                                : (double(k) - double(_nfft)) * binHz;
    }

    void accumulateFrame()
    {
        const std::complex<float> *oldest = _ring.data() + _write;
        for (size_t i = 0; i < _nfft; ++i) _fft[i] = oldest[i] * _window[i];
        _fftPlan.execute(_fft);

        _frame.frameIndex = _frames;
        _frame.centerSampleIndex = _samples - _nfft + (_nfft - 1) / 2;
        if (_startTimestampNs != 0)
        {
            const double dtNs = 1e9 * double(_frame.centerSampleIndex) / _fs;
            _frame.timestampNs = _startTimestampNs + static_cast<uint64_t>(std::llround(dtNs));
        }
        _frame.gainDb = _currentGainDb;
        for (size_t j = 0; j < _bandBins.size(); ++j)
        {
            const size_t k = _bandBins[j];
            const float re = _fft[k].real();
            const float im = _fft[k].imag();
            const double raw = double(re) * double(re) + double(im) * double(im);
            _powerSum[k] += raw;
            _frame.powerDensity[j] = static_cast<float>(raw * _psdScale);
        }
        ++_frames;
        if (_callback) _callback(_frame);
    }

    double _fs;
    double _bandwidth;
    size_t _nfft;
    size_t _hop;
    std::vector<std::complex<float>> _ring;
    std::vector<float> _window;
    std::vector<std::complex<float>> _fft;
    std::vector<double> _powerSum;
    Radix2Fft _fftPlan;
    std::vector<size_t> _bandBins;
    std::vector<double> _frequencies;
    std::function<void(const PsdFrame &)> _callback;
    size_t _write = 0;
    size_t _samples = 0;
    size_t _frames = 0;
    size_t _untilFrame = 0;
    double _windowPower = 0.0;
    uint64_t _startTimestampNs = 0;
    float _currentGainDb = 0.0f;
    double _psdScale = 0.0;
    PsdFrame _frame;
};

class IntegratedDspChain
{
public:
    IntegratedDspChain(const std::vector<float> &taps1,
                       const std::vector<float> &taps2,
                       const double shiftHz,
                       const double inputRate,
                       const size_t decim1,
                       const size_t decim2,
                       const double bandwidth,
                       const size_t nfft,
                       const size_t fir1Threads)
        : _mixer(shiftHz, inputRate), _dec1(taps1, decim1, fir1Threads),
          _dec2(taps2, decim2), _psd(inputRate / double(decim1 * decim2), bandwidth, nfft)
    {
    }

    size_t processCs8(const int8_t *iq, const size_t count,
                      uint64_t *saturatedComplex = nullptr, const int clipLevel = 126)
    {
        constexpr float CS8_SCALE = 1.0f / 128.0f;
        const auto t0 = std::chrono::steady_clock::now();
        _dec1.processCs8(_mixer, iq, count, _stage1, saturatedComplex, clipLevel);
        const auto t1 = std::chrono::steady_clock::now();

        size_t produced = 0;
        std::complex<float> y2;
        for (const std::complex<float> &y1 : _stage1)
        {
            // Preserve native integer amplitude through NCO/FIR1 and normalize
            // only at 1/decim1 rate. Constructing the scaled value directly
            // avoids a copy followed by complex operator*= in this hot loop.
            const std::complex<float> scaled(
                y1.real() * CS8_SCALE, y1.imag() * CS8_SCALE);
            if (_dec2.push(scaled, y2))
            {
                _psd.consume(y2);
                ++produced;
            }
        }
        const auto t2 = std::chrono::steady_clock::now();
        _fir1WallSeconds += std::chrono::duration<double>(t1 - t0).count();
        _postFir1WallSeconds += std::chrono::duration<double>(t2 - t1).count();
        return produced;
    }

    bool ncoUsesLut() const { return _mixer.usesLut(); }
    size_t ncoLutPeriod() const { return _mixer.lutPeriod(); }
    size_t fir1Threads() const { return _dec1.threads(); }
    double fir1WallSeconds() const { return _fir1WallSeconds; }
    double postFir1WallSeconds() const { return _postFir1WallSeconds; }
    WelchBandPsd &psd() { return _psd; }
    const WelchBandPsd &psd() const { return _psd; }

private:
    NcoMixer _mixer;
    BlockFir1Decimator _dec1;
    FirDecimator _dec2;
    WelchBandPsd _psd;
    std::vector<std::complex<float>> _stage1;
    double _fir1WallSeconds = 0.0;
    double _postFir1WallSeconds = 0.0;
};

class ClippingAgc
{
public:
    ClippingAgc(const Config &cfg, SoapySDR::Device *dev, const double initialGain,
                const double deviceMinGain, const double deviceMaxGain)
        : _cfg(cfg), _dev(dev), _gain(initialGain)
    {
        _minGain = (cfg.agcMinGain >= 0.0) ? cfg.agcMinGain : deviceMinGain;
        _maxGain = (cfg.agcMaxGain >= 0.0) ? cfg.agcMaxGain : deviceMaxGain;
        if (_minGain > _maxGain) std::swap(_minGain, _maxGain);
        _gain = std::max(_minGain, std::min(_maxGain, _gain));
    }

    double update(const uint64_t clipped, const size_t samples, const double blockSeconds)
    {
        if (!_cfg.agcEnabled || samples == 0) return _gain;
        const double measured = 100.0 * double(clipped) / double(samples);
        const double tau = std::max(1e-3, _cfg.agcTimeConstantSeconds);
        const double alpha = 1.0 - std::exp(-blockSeconds / tau);
        if (!_initialized) { _smoothedPercent = measured; _initialized = true; }
        else _smoothedPercent += alpha * (measured - _smoothedPercent);

        _elapsedSinceUpdate += blockSeconds;
        const double updatePeriod = std::max(0.10, std::min(1.0, tau / 4.0));
        if (_elapsedSinceUpdate < updatePeriod) return _gain;
        _elapsedSinceUpdate = 0.0;

        const double target = std::max(1e-6, _cfg.agcTargetSaturationPercent);
        // A logarithmic error gives fast protection above the target and gentle
        // recovery below it. The per-update change is capped to avoid pumping.
        const double floorPercent = target * 0.01;
        const double ratio = target / std::max(_smoothedPercent, floorPercent);
        double correctionDb = 10.0 * std::log10(ratio);
        const double deadband = 0.20 * target;
        if (std::abs(_smoothedPercent - target) <= deadband) correctionDb = 0.0;
        correctionDb = std::max(-_cfg.agcMaxStepDb, std::min(_cfg.agcMaxStepDb, correctionDb));

        const double requested = std::max(_minGain, std::min(_maxGain, _gain + correctionDb));
        if (std::abs(requested - _gain) >= 1e-6)
        {
            _dev->setGain(SOAPY_SDR_RX, 0, requested);
            try { _gain = _dev->getGain(SOAPY_SDR_RX, 0); } catch (...) { _gain = requested; }
            ++_gainChanges;
        }
        return _gain;
    }

    double gain() const { return _gain; }
    double smoothedPercent() const { return _smoothedPercent; }
    uint64_t gainChanges() const { return _gainChanges; }

private:
    const Config &_cfg;
    SoapySDR::Device *_dev;
    double _gain = 0.0, _minGain = 0.0, _maxGain = 0.0;
    double _smoothedPercent = 0.0, _elapsedSinceUpdate = 0.0;
    bool _initialized = false;
    uint64_t _gainChanges = 0;
};


uint64_t systemNowNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::string utcIso8601(const uint64_t ns)
{
    const std::time_t sec = static_cast<std::time_t>(ns / 1000000000ULL);
    std::tm tmv{};
    gmtime_r(&sec, &tmv);
    char buf[64];
    const unsigned ms = static_cast<unsigned>((ns / 1000000ULL) % 1000ULL);
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return std::string(buf);
}


std::string rotationDayUtc(const uint64_t ns, const int rotateMinuteUtc)
{
    // Shift UTC backwards by the configured rotation time, then use the
    // resulting calendar date as the logical file day. Example: with 06:00,
    // 2026-08-26 05:59 UTC belongs to the 20260825 file, while 06:00 belongs
    // to 20260826.
    const int64_t sec = static_cast<int64_t>(ns / 1000000000ULL)
                      - static_cast<int64_t>(rotateMinuteUtc) * 60;
    const std::time_t shifted = static_cast<std::time_t>(sec);
    std::tm tmv{};
    gmtime_r(&shifted, &tmv);
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%Y%m%d", &tmv) == 0)
        throw std::runtime_error("Failed to format UTC rotation day");
    return std::string(buf);
}

std::string formatDailyRotateTime(const int minuteUtc)
{
    // Configuration validation guarantees [0, 1439]. Using iostream formatting
    // avoids GCC's conservative -Wformat-truncation analysis on snprintf.
    const int hour = minuteUtc / 60;
    const int minute = minuteUtc % 60;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hour
        << ':' << std::setw(2) << minute;
    return out.str();
}

std::string hostnameString()
{
    char h[256] = {0};
    if (::gethostname(h, sizeof(h) - 1) != 0) return "unknown";
    return std::string(h);
}


void ensureDirectory(const std::string &path)
{
    if (path.empty() || path == ".") return;
    std::string current;
    if (path[0] == '/') current = "/";
    std::istringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (part.empty()) continue;
        if (!current.empty() && current.back() != '/') current += '/';
        current += part;
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
            throw std::runtime_error("cannot create directory " + current + ": " + std::strerror(errno));
    }
}

void h5Check(const herr_t status, const char *what)
{
    if (status < 0) throw std::runtime_error(std::string("HDF5 failure: ") + what);
}

void h5WriteStringAttribute(hid_t obj, const char *name, const std::string &value)
{
    hid_t type = H5Tcopy(H5T_C_S1);
    h5Check(type, "H5Tcopy string");
    h5Check(H5Tset_size(type, value.size() + 1), "H5Tset_size");
    h5Check(H5Tset_strpad(type, H5T_STR_NULLTERM), "H5Tset_strpad");
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(obj, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) { H5Sclose(space); H5Tclose(type); throw std::runtime_error(std::string("HDF5 create attribute: ") + name); }
    h5Check(H5Awrite(attr, type, value.c_str()), "H5Awrite string");
    H5Aclose(attr); H5Sclose(space); H5Tclose(type);
}

template <typename T>
void h5WriteScalarAttribute(hid_t obj, const char *name, hid_t type, const T &value)
{
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(obj, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) { H5Sclose(space); throw std::runtime_error(std::string("HDF5 create attribute: ") + name); }
    h5Check(H5Awrite(attr, type, &value), "H5Awrite scalar");
    H5Aclose(attr); H5Sclose(space);
}

struct RecorderMetadata
{
    std::string tomlText;
    std::string hostname;
    std::string driver;
    std::string hardwareInfo;
    uint64_t acquisitionStartNs = 0;
    double sampleRate = 0.0;
    double centerFrequency = 0.0;
    double sdrBandwidth = 0.0;
    double gain = 0.0;
    double shiftHz = 0.0;
    double outputRate = 0.0;
    double psdBandwidth = 0.0;
    size_t decim1 = 1;
    size_t decim2 = 1;
    size_t nfft = 0;
};

class DailyHdf5Writer
{
public:
    DailyHdf5Writer(const Config &cfg, const std::vector<double> &frequencies, RecorderMetadata metadata)
        : _cfg(cfg), _frequencies(frequencies), _metadata(std::move(metadata))
    {
        ensureDirectory(_cfg.detectorOutputDirectory);
        rotate(_metadata.acquisitionStartNs);
    }

    ~DailyHdf5Writer() { close(); }

    void append(const PsdFrame &frame, const uint64_t eventId, const uint8_t detectorState,
                const float detectorMaxDbHz)
    {
        // File rotation is controlled by PsdDetectorRecorder. In particular,
        // append() must never rotate while an event is pinned to the previous
        // daily file.
        const hsize_t row = _rows;
        const hsize_t newRows = row + 1;
        hsize_t dims2[2] = {newRows, static_cast<hsize_t>(_frequencies.size())};
        hsize_t dims1[1] = {newRows};
        h5Check(H5Dset_extent(_dPsd, dims2), "extend PSD");
        h5Check(H5Dset_extent(_dTimestamp, dims1), "extend timestamp");
        h5Check(H5Dset_extent(_dFrameIndex, dims1), "extend frame index");
        h5Check(H5Dset_extent(_dEventId, dims1), "extend event id");
        h5Check(H5Dset_extent(_dDetectorState, dims1), "extend detector state");
        h5Check(H5Dset_extent(_dDetectorMax, dims1), "extend detector max");
        h5Check(H5Dset_extent(_dGain, dims1), "extend gain");

        hsize_t start2[2] = {row, 0};
        hsize_t count2[2] = {1, static_cast<hsize_t>(_frequencies.size())};
        hid_t fileSpace = H5Dget_space(_dPsd);
        h5Check(H5Sselect_hyperslab(fileSpace, H5S_SELECT_SET, start2, nullptr, count2, nullptr), "select PSD row");
        hid_t memSpace = H5Screate_simple(2, count2, nullptr);
        h5Check(H5Dwrite(_dPsd, H5T_NATIVE_FLOAT, memSpace, fileSpace, H5P_DEFAULT, frame.powerDensity.data()), "write PSD row");
        H5Sclose(memSpace); H5Sclose(fileSpace);

        writeOne(_dTimestamp, row, H5T_NATIVE_UINT64, &frame.timestampNs);
        writeOne(_dFrameIndex, row, H5T_NATIVE_UINT64, &frame.frameIndex);
        writeOne(_dEventId, row, H5T_NATIVE_UINT64, &eventId);
        writeOne(_dDetectorState, row, H5T_NATIVE_UINT8, &detectorState);
        writeOne(_dDetectorMax, row, H5T_NATIVE_FLOAT, &detectorMaxDbHz);
        writeOne(_dGain, row, H5T_NATIVE_FLOAT, &frame.gainDb);
        ++_rows;
        _dirty = true;
        flushSwmr(false);
        checkStorageSafety();
    }

    const std::string &path() const { return _path; }

    // Called for every PSD frame. Rotation is allowed only while no detector
    // event is in progress. If a daily boundary is crossed during an event,
    // the writer remains pinned to the previous file until the event (including
    // its post-trigger context) has completely finished.
    void tick(const uint64_t timestampNs, const bool eventInProgress)
    {
        // Publish a short event even if no further rows arrive after it.
        flushSwmr(false);
        if (!eventInProgress) rotate(timestampNs);
    }

private:
    hid_t createFileAccessPlist() const
    {
        if (!_cfg.outputSwmr) return H5P_DEFAULT;
        hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
        if (fapl < 0) throw std::runtime_error("HDF5 failure: H5Pcreate file access");
        if (H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) < 0)
        {
            H5Pclose(fapl);
            throw std::runtime_error("HDF5 failure: H5Pset_libver_bounds latest");
        }
        return fapl;
    }

    void startSwmr()
    {
        if (!_cfg.outputSwmr) return;
        if (H5Fstart_swmr_write(_file) < 0)
            throw std::runtime_error(
                "cannot enable HDF5 SWMR writer mode for " + _path +
                "; the file may have been created by an older non-SWMR Meteoris version");
        _swmrActive = true;
        _lastSwmrFlush = std::chrono::steady_clock::now();
        // Publish the complete schema/metadata before readers attach.
        h5Check(H5Fflush(_file, H5F_SCOPE_GLOBAL), "initial SWMR file flush");
    }

    void flushSwmr(const bool force)
    {
        if (!_cfg.outputSwmr || !_swmrActive || !_dirty || _file < 0) return;

        const auto now = std::chrono::steady_clock::now();
        if (!force && _cfg.outputSwmrFlushSeconds > 0.0)
        {
            const double elapsed =
                std::chrono::duration<double>(now - _lastSwmrFlush).count();
            if (elapsed < _cfg.outputSwmrFlushSeconds) return;
        }

        // Flush every extendible dataset so SWMR readers can refresh dimensions
        // and then read a mutually consistent published prefix.
        h5Check(H5Dflush(_dPsd), "SWMR flush PSD");
        h5Check(H5Dflush(_dTimestamp), "SWMR flush timestamp");
        h5Check(H5Dflush(_dFrameIndex), "SWMR flush frame index");
        h5Check(H5Dflush(_dEventId), "SWMR flush event id");
        h5Check(H5Dflush(_dDetectorState), "SWMR flush detector state");
        h5Check(H5Dflush(_dDetectorMax), "SWMR flush detector max");
        h5Check(H5Dflush(_dGain), "SWMR flush gain");
        h5Check(H5Fflush(_file, H5F_SCOPE_GLOBAL), "SWMR file flush");
        _dirty = false;
        _lastSwmrFlush = now;
    }

    void checkStorageSafety()
    {
        // Limit filesystem queries to once per second; HDF5 appends may occur at
        // ~100 PSD frames/s during an event.
        const auto now = std::chrono::steady_clock::now();
        if (_storageCheckInitialized &&
            std::chrono::duration<double>(now - _lastStorageCheck).count() < 1.0)
            return;
        _lastStorageCheck = now;

        struct stat st{};
        if (::stat(_path.c_str(), &st) != 0)
            throw std::runtime_error("cannot stat output file " + _path + ": " + std::strerror(errno));
        const uint64_t bytes = static_cast<uint64_t>(st.st_size);

        if (!_storageCheckInitialized)
        {
            _storageCheckInitialized = true;
            _growthStart = now;
            _growthStartBytes = bytes;
        }
        else if (_cfg.outputMaxGrowthMbPerMin > 0.0)
        {
            const double elapsed = std::chrono::duration<double>(now - _growthStart).count();
            // Use a rolling baseline of at least 10 s to avoid false trips from
            // HDF5 chunk allocation/compression bursts.
            if (elapsed >= 10.0)
            {
                const uint64_t delta = bytes >= _growthStartBytes ? bytes - _growthStartBytes : 0;
                const double mbPerMin = (double(delta) / (1024.0 * 1024.0)) * (60.0 / elapsed);
                if (mbPerMin > _cfg.outputMaxGrowthMbPerMin)
                {
                    std::ostringstream msg;
                    msg << "output file growth too high: " << std::fixed << std::setprecision(2)
                        << mbPerMin << " MB/min > limit " << _cfg.outputMaxGrowthMbPerMin
                        << " MB/min (" << _path << ")";
                    throw std::runtime_error(msg.str());
                }
                _growthStart = now;
                _growthStartBytes = bytes;
            }
        }

        if (_cfg.outputMinFreeGb > 0.0)
        {
            struct statvfs fs{};
            if (::statvfs(_cfg.detectorOutputDirectory.c_str(), &fs) != 0)
                throw std::runtime_error("cannot query free disk space for " +
                                         _cfg.detectorOutputDirectory + ": " + std::strerror(errno));
            const double freeGb =
                (double(fs.f_bavail) * double(fs.f_frsize)) / (1024.0 * 1024.0 * 1024.0);
            if (freeGb < _cfg.outputMinFreeGb)
            {
                std::ostringstream msg;
                msg << "free disk space too low: " << std::fixed << std::setprecision(2)
                    << freeGb << " GB < limit " << _cfg.outputMinFreeGb
                    << " GB (" << _cfg.detectorOutputDirectory << ")";
                throw std::runtime_error(msg.str());
            }
        }
    }

    void resetStorageSafety()
    {
        _storageCheckInitialized = false;
        _growthStartBytes = 0;
        _lastStorageCheck = std::chrono::steady_clock::time_point{};
        _growthStart = std::chrono::steady_clock::time_point{};
    }

    void writeOne(hid_t dataset, hsize_t row, hid_t type, const void *value)
    {
        hsize_t start[1] = {row}, count[1] = {1};
        hid_t fs = H5Dget_space(dataset);
        h5Check(H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr), "select scalar row");
        hid_t ms = H5Screate_simple(1, count, nullptr);
        h5Check(H5Dwrite(dataset, type, ms, fs, H5P_DEFAULT, value), "write scalar row");
        H5Sclose(ms); H5Sclose(fs);
    }

    hid_t createUnlimited1D(const char *path, hid_t type)
    {
        hsize_t dims[1] = {0}, maxdims[1] = {H5S_UNLIMITED};
        hid_t space = H5Screate_simple(1, dims, maxdims);
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunk[1] = {256};
        h5Check(H5Pset_chunk(dcpl, 1, chunk), "set 1D chunk");
        hid_t d = H5Dcreate2(_file, path, type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        H5Pclose(dcpl); H5Sclose(space);
        if (d < 0) throw std::runtime_error(std::string("cannot create HDF5 dataset ") + path);
        return d;
    }

    void rotate(const uint64_t timestampNs)
    {
        const std::string day = rotationDayUtc(timestampNs, _cfg.outputDailyRotateMinuteUtc);
        if (_file >= 0 && day == _day) return;
        close();
        _day = day;
        const std::string sep = (_cfg.detectorOutputDirectory.empty() || _cfg.detectorOutputDirectory.back() == '/') ? "" : "/";
        _path = _cfg.detectorOutputDirectory + sep + _cfg.detectorFilePrefix + "_" + day + ".h5";
        resetStorageSafety();
        const bool exists = (::access(_path.c_str(), F_OK) == 0);
        if (exists)
        {
            hid_t fapl = createFileAccessPlist();
            _file = H5Fopen(_path.c_str(), H5F_ACC_RDWR, fapl);
            if (fapl != H5P_DEFAULT) H5Pclose(fapl);
            if (_file < 0) throw std::runtime_error("cannot open HDF5 file: " + _path);
            openExisting();
            startSwmr();
        }
        else createNew();
    }

    void createNew()
    {
        hid_t fapl = createFileAccessPlist();
        _file = H5Fcreate(_path.c_str(), H5F_ACC_EXCL, H5P_DEFAULT, fapl);
        if (fapl != H5P_DEFAULT) H5Pclose(fapl);
        if (_file < 0) throw std::runtime_error("cannot create HDF5 file: " + _path);
        hid_t gMeta = H5Gcreate2(_file, "/metadata", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        hid_t gPsd = H5Gcreate2(_file, "/psd", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Gclose(gPsd);

        h5WriteStringAttribute(gMeta, "format", METEORIS_HDF5_FORMAT);
        h5WriteStringAttribute(gMeta, "author", METEORIS_HDF5_AUTHOR);
        h5WriteStringAttribute(gMeta, "software_version", METEORIS_VERSION);
        h5WriteStringAttribute(gMeta, "hostname", _metadata.hostname);
        h5WriteStringAttribute(gMeta, "driver", _metadata.driver);
        h5WriteStringAttribute(gMeta, "hardware_info", _metadata.hardwareInfo);
        h5WriteStringAttribute(gMeta, "acquisition_start_utc", utcIso8601(_metadata.acquisitionStartNs));
        h5WriteScalarAttribute(gMeta, "acquisition_start_ns", H5T_NATIVE_UINT64, _metadata.acquisitionStartNs);
        h5WriteScalarAttribute(gMeta, "sample_rate_hz", H5T_NATIVE_DOUBLE, _metadata.sampleRate);
        h5WriteScalarAttribute(gMeta, "center_frequency_hz", H5T_NATIVE_DOUBLE, _metadata.centerFrequency);
        h5WriteScalarAttribute(gMeta, "sdr_bandwidth_hz", H5T_NATIVE_DOUBLE, _metadata.sdrBandwidth);
        h5WriteScalarAttribute(gMeta, "gain_db", H5T_NATIVE_DOUBLE, _metadata.gain);
        h5WriteScalarAttribute(gMeta, "shift_hz", H5T_NATIVE_DOUBLE, _metadata.shiftHz);
        h5WriteScalarAttribute(gMeta, "output_rate_hz", H5T_NATIVE_DOUBLE, _metadata.outputRate);
        h5WriteScalarAttribute(gMeta, "psd_bandwidth_hz", H5T_NATIVE_DOUBLE, _metadata.psdBandwidth);
        const uint64_t d1 = _metadata.decim1, d2 = _metadata.decim2, nf = _metadata.nfft;
        h5WriteScalarAttribute(gMeta, "decimation_stage1", H5T_NATIVE_UINT64, d1);
        h5WriteScalarAttribute(gMeta, "decimation_stage2", H5T_NATIVE_UINT64, d2);
        h5WriteScalarAttribute(gMeta, "fft_size", H5T_NATIVE_UINT64, nf);
        h5WriteStringAttribute(gMeta, "day_boundary",
                               std::string("UTC daily at ") +
                               formatDailyRotateTime(_cfg.outputDailyRotateMinuteUtc));
        h5WriteStringAttribute(gMeta, "swmr",
                               _cfg.outputSwmr ? "enabled" : "disabled");
        H5Gclose(gMeta);

        // Store the exact input TOML text once at file creation. If no config file
        // was supplied this contains a generated effective configuration.
        const std::string &txt = _metadata.tomlText;
        hsize_t tdims[1] = {static_cast<hsize_t>(txt.size())};
        hid_t ts = H5Screate_simple(1, tdims, nullptr);
        hid_t td = H5Dcreate2(_file, "/metadata/toml_config", H5T_NATIVE_CHAR, ts, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (!txt.empty()) h5Check(H5Dwrite(td, H5T_NATIVE_CHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, txt.data()), "write TOML");
        H5Dclose(td); H5Sclose(ts);

        hsize_t fdims[1] = {static_cast<hsize_t>(_frequencies.size())};
        hid_t fs = H5Screate_simple(1, fdims, nullptr);
        hid_t fd = H5Dcreate2(_file, "/psd/frequency_hz", H5T_NATIVE_DOUBLE, fs, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        h5Check(H5Dwrite(fd, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, _frequencies.data()), "write frequencies");
        H5Dclose(fd); H5Sclose(fs);

        hsize_t dims[2] = {0, static_cast<hsize_t>(_frequencies.size())};
        hsize_t maxdims[2] = {H5S_UNLIMITED, static_cast<hsize_t>(_frequencies.size())};
        hid_t space = H5Screate_simple(2, dims, maxdims);
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunk[2] = {128, static_cast<hsize_t>(_frequencies.size())};
        h5Check(H5Pset_chunk(dcpl, 2, chunk), "set PSD chunk");
        if (_cfg.detectorCompression > 0) h5Check(H5Pset_deflate(dcpl, static_cast<unsigned>(_cfg.detectorCompression)), "set gzip");
        _dPsd = H5Dcreate2(_file, "/psd/power_density", H5T_IEEE_F32LE, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        H5Pclose(dcpl); H5Sclose(space);
        if (_dPsd < 0) throw std::runtime_error("cannot create PSD dataset");
        _dTimestamp = createUnlimited1D("/psd/timestamp_ns", H5T_STD_U64LE);
        _dFrameIndex = createUnlimited1D("/psd/frame_index", H5T_STD_U64LE);
        _dEventId = createUnlimited1D("/psd/event_id", H5T_STD_U64LE);
        _dDetectorState = createUnlimited1D("/psd/detector_state", H5T_STD_U8LE);
        _dDetectorMax = createUnlimited1D("/psd/detector_max_db_hz", H5T_IEEE_F32LE);
        _dGain = createUnlimited1D("/psd/gain_db", H5T_IEEE_F32LE);
        _rows = 0;
        startSwmr();
        LOG_INFO_STREAM("HDF5_open=" << _path << " (new"
                        << (_swmrActive ? ", SWMR" : "") << ")");
    }

    void openExisting()
    {
        _dPsd = H5Dopen2(_file, "/psd/power_density", H5P_DEFAULT);
        _dTimestamp = H5Dopen2(_file, "/psd/timestamp_ns", H5P_DEFAULT);
        _dFrameIndex = H5Dopen2(_file, "/psd/frame_index", H5P_DEFAULT);
        _dEventId = H5Dopen2(_file, "/psd/event_id", H5P_DEFAULT);
        _dDetectorState = H5Dopen2(_file, "/psd/detector_state", H5P_DEFAULT);
        _dDetectorMax = H5Dopen2(_file, "/psd/detector_max_db_hz", H5P_DEFAULT);
        _dGain = H5Dopen2(_file, "/psd/gain_db", H5P_DEFAULT);
        if (_dPsd < 0 || _dTimestamp < 0 || _dFrameIndex < 0 || _dEventId < 0 || _dDetectorState < 0 || _dDetectorMax < 0)
            throw std::runtime_error("existing HDF5 file has incompatible schema: " + _path);
        hid_t sp = H5Dget_space(_dPsd);
        hsize_t dims[2] = {0,0};
        H5Sget_simple_extent_dims(sp, dims, nullptr);
        H5Sclose(sp);
        if (dims[1] != _frequencies.size()) throw std::runtime_error("existing HDF5 PSD bin count mismatch: " + _path);
        _rows = dims[0];
        if (_dGain < 0)
        {
            _dGain = createUnlimited1D("/psd/gain_db", H5T_IEEE_F32LE);
            hsize_t gdims[1] = {_rows};
            h5Check(H5Dset_extent(_dGain, gdims), "extend legacy gain dataset");
            if (_rows > 0)
            {
                std::vector<float> legacyGain(static_cast<size_t>(_rows), static_cast<float>(_metadata.gain));
                h5Check(H5Dwrite(_dGain, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, legacyGain.data()), "fill legacy gain dataset");
            }
        }
        LOG_INFO_STREAM("HDF5_open=" << _path << " (append rows=" << _rows
                        << (_cfg.outputSwmr ? ", SWMR requested" : "") << ")");
    }

    void close()
    {
        // Publish the final complete prefix before closing the SWMR writer.
        flushSwmr(true);
        if (_dGain >= 0) H5Dclose(_dGain);
        if (_dDetectorMax >= 0) H5Dclose(_dDetectorMax);
        if (_dDetectorState >= 0) H5Dclose(_dDetectorState);
        if (_dEventId >= 0) H5Dclose(_dEventId);
        if (_dFrameIndex >= 0) H5Dclose(_dFrameIndex);
        if (_dTimestamp >= 0) H5Dclose(_dTimestamp);
        if (_dPsd >= 0) H5Dclose(_dPsd);
        if (_file >= 0) { H5Fflush(_file, H5F_SCOPE_GLOBAL); H5Fclose(_file); }
        _dPsd = _dTimestamp = _dFrameIndex = _dEventId = _dDetectorState = _dDetectorMax = _dGain = -1;
        _file = -1; _rows = 0;
        _swmrActive = false;
        _dirty = false;
    }

    const Config &_cfg;
    const std::vector<double> &_frequencies;
    RecorderMetadata _metadata;
    std::string _day, _path;
    hid_t _file = -1;
    hid_t _dPsd = -1, _dTimestamp = -1, _dFrameIndex = -1, _dEventId = -1, _dDetectorState = -1, _dDetectorMax = -1, _dGain = -1;
    hsize_t _rows = 0;
    bool _swmrActive = false;
    bool _dirty = false;
    std::chrono::steady_clock::time_point _lastSwmrFlush{};
    bool _storageCheckInitialized = false;
    uint64_t _growthStartBytes = 0;
    std::chrono::steady_clock::time_point _lastStorageCheck{};
    std::chrono::steady_clock::time_point _growthStart{};
};


class ContinuousPsdRecorder
{
public:
    ContinuousPsdRecorder(const Config &cfg,
                          const std::vector<double> &frequencies,
                          RecorderMetadata metadata)
        : _cfg(cfg),
          _framePeriodSeconds(
              (metadata.outputRate > 0.0 && metadata.nfft > 0)
                  ? (double(metadata.nfft / 2) / metadata.outputRate) : 0.0),
          _writer(cfg, frequencies, std::move(metadata))
    {
        if (!(_framePeriodSeconds > 0.0))
            throw std::runtime_error("cannot derive PSD frame period for continuous recorder");
        _segmentFrames = std::max<uint64_t>(
            1, static_cast<uint64_t>(
                std::ceil(_cfg.recordingSegmentSeconds / _framePeriodSeconds)));
        LOG_INFO_STREAM("recording_mode=continuous segment_seconds="
                        << _cfg.recordingSegmentSeconds
                        << " segment_frames=" << _segmentFrames
                        << " segment_count=" << _cfg.recordingSegmentCount);
    }

    void consume(const PsdFrame &frame)
    {
        if (_finished) return;

        if (_framesInSegment == 0)
        {
            ++_segmentId;
            _writer.tick(frame.timestampNs, false);
        }

        // detector_state=0 is deliberately neutral in continuous mode.
        float maxDb = -300.0f;
        for (float p : frame.powerDensity)
            if (p > 0.0f) maxDb = std::max(maxDb, 10.0f * std::log10(p));
        _writer.append(frame, _segmentId, 0, maxDb);
        ++_savedFrames;
        ++_framesInSegment;

        if (_framesInSegment >= _segmentFrames)
        {
            _framesInSegment = 0;
            ++_completedSegments;
            // The daily HDF5 writer is allowed to rotate only at a segment
            // boundary, so a segment crossing the daily boundary remains whole.
            _writer.tick(frame.timestampNs, false);
            if (_cfg.recordingSegmentCount != 0 &&
                _completedSegments >= _cfg.recordingSegmentCount)
                _finished = true;
        }
    }

    bool finished() const { return _finished; }
    bool eventInProgress() const { return false; }

    void printSummary() const
    {
        LOG_INFO_STREAM("recording_mode=continuous segments_completed="
                        << _completedSegments
                        << " current_segment_frames=" << _framesInSegment
                        << " saved_psd_frames=" << _savedFrames);
    }

private:
    const Config &_cfg;
    double _framePeriodSeconds = 0.0;
    uint64_t _segmentFrames = 1;
    DailyHdf5Writer _writer;
    uint64_t _segmentId = 0;
    uint64_t _framesInSegment = 0;
    uint64_t _completedSegments = 0;
    uint64_t _savedFrames = 0;
    bool _finished = false;
};


class PsdDetectorRecorder
{
public:
    PsdDetectorRecorder(const Config &cfg,
                        const std::vector<double> &frequencies,
                        RecorderMetadata metadata)
        : _cfg(cfg),
          _frequencies(frequencies),
          _framePeriodSeconds(
              (metadata.outputRate > 0.0 && metadata.nfft > 0)
                  ? (double(metadata.nfft / 2) / metadata.outputRate)
                  : 0.0),
          _writer(cfg, frequencies, metadata)
    {
        if (!(_framePeriodSeconds > 0.0))
            throw std::runtime_error("cannot derive PSD frame period for detector");
        if (_frequencies.size() < 2)
            throw std::runtime_error("cannot derive detector PSD geometry");

        meteoris::detector::Environment env;
        env.bins = _frequencies.size();
        env.frequencyStartHz = _frequencies.front();
        env.frequencyStepHz = _frequencies[1] - _frequencies[0];
        env.framePeriodSeconds = _framePeriodSeconds;
        env.requestedThreads = _cfg.detectorThreads;
        const unsigned hc = std::thread::hardware_concurrency();
        env.hardwareThreads = hc == 0 ? 1 : hc;

        meteoris::detector::Selection selection;
        selection.plugin = _cfg.detectorPlugin;
        selection.threads = _cfg.detectorThreads;
        selection.peakTracker.frequencyMeanBins = _cfg.detectorFrequencyMeanBins;
        selection.peakTracker.timeMeanPsds = _cfg.detectorTimeMeanPsds;
        selection.peakTracker.peakThresholdDb = _cfg.detectorPeakThresholdDb;
        selection.peakTracker.minPeakSeparationHz = _cfg.detectorMinPeakSeparationHz;
        selection.peakTracker.maxPeaksPerPsd = _cfg.detectorMaxPeaksPerPsd;
        selection.peakTracker.stationaryEnabled = _cfg.detectorStationaryEnabled;
        selection.peakTracker.stationaryMinHz = _cfg.detectorStationaryMinHz;
        selection.peakTracker.stationaryMaxHz = _cfg.detectorStationaryMaxHz;
        selection.peakTracker.stationaryMaxDfHz = _cfg.detectorStationaryMaxDfHz;
        selection.peakTracker.stationaryActivationSeconds = _cfg.detectorStationaryActivationSeconds;
        selection.peakTracker.stationaryActivationFraction = _cfg.detectorStationaryActivationFraction;
        selection.peakTracker.stationaryLostSeconds = _cfg.detectorStationaryLostSeconds;
        selection.peakTracker.stationaryMaxDriftHzS = _cfg.detectorStationaryMaxDriftHzS;
        selection.peakTracker.chirpEnabled = _cfg.detectorChirpEnabled;
        selection.peakTracker.chirpMinHz = _cfg.detectorChirpMinHz;
        selection.peakTracker.chirpMaxHz = _cfg.detectorChirpMaxHz;
        selection.peakTracker.chirpMaxDfHz = _cfg.detectorChirpMaxDfHz;
        selection.peakTracker.chirpActivationSeconds = _cfg.detectorChirpActivationSeconds;
        selection.peakTracker.chirpActivationFraction = _cfg.detectorChirpActivationFraction;
        selection.peakTracker.chirpLostSeconds = _cfg.detectorChirpLostSeconds;
        selection.peakTracker.chirpMinDriftHzS = _cfg.detectorChirpMinDriftHzS;
        selection.peakTracker.chirpMaxDriftHzS = _cfg.detectorChirpMaxDriftHzS;

        _detector = meteoris::detector::create(selection, env);
        if (_detector->info().apiVersion != meteoris::detector::DETECTOR_API_VERSION)
            throw std::runtime_error("detector API version mismatch");

        LOG_INFO_STREAM("detector_plugin=" << _detector->info().name
                        << " plugin_version=" << _detector->info().version
                        << " api=" << _detector->info().apiVersion
                        << " requested_threads=" << env.requestedThreads
                        << " hardware_threads=" << env.hardwareThreads
                        << " plugin_multithread="
                        << (_detector->info().mayUseMultipleThreads ? "YES" : "NO"));

        _preContextFrames = static_cast<size_t>(std::ceil(
            _cfg.detectorPreContextSeconds / _framePeriodSeconds));
        _postContextFrames = static_cast<size_t>(std::ceil(
            _cfg.detectorPostContextSeconds / _framePeriodSeconds));
    }

    void consume(const PsdFrame &frame)
    {
        const bool eventInProgressAtEntry = _active || _postRemaining > 0;
        _writer.tick(frame.timestampNs, eventInProgressAtEntry);

        if (_rearmUntilNs != 0)
        {
            if (frame.timestampNs < _rearmUntilNs)
            {
                // Keep plugin smoothing/history current but remove tracks so
                // rearm cannot inherit an old candidate.
                (void)_detector->process(makeDetectorFrame(frame));
                _detector->reset();
                _pre.clear();
                return;
            }
            _rearmUntilNs = 0;
            _detector->reset();
            _pre.clear();
        }

        if (_cfg.detectorMaxEventSeconds > 0.0 && _eventStartNs != 0 &&
            (_active || _postRemaining > 0))
        {
            const uint64_t maxNs = static_cast<uint64_t>(
                std::llround(_cfg.detectorMaxEventSeconds * 1e9));
            if (frame.timestampNs >= _eventStartNs + maxNs)
            {
                (void)_detector->process(makeDetectorFrame(frame));
                _detector->reset();
                _active = false;
                _postRemaining = 0;
                _pre.clear();
                ++_forcedCutoffs;
                const uint64_t rearmNs = static_cast<uint64_t>(
                    std::llround(_cfg.detectorRearmSeconds * 1e9));
                _rearmUntilNs = frame.timestampNs + rearmNs;
                _eventStartNs = 0;
                _writer.tick(frame.timestampNs, false);
                return;
            }
        }

        const auto processStart = std::chrono::steady_clock::now();
        const meteoris::detector::Result decision =
            _detector->process(makeDetectorFrame(frame));
        const auto processEnd = std::chrono::steady_clock::now();
        _lastDetectorProcessUs =
            std::chrono::duration<double, std::micro>(
                processEnd - processStart).count();

        consumeDebugCounters(decision);
        emitWarnings(frame, decision);
        printDiagnostic(frame, decision);

        const bool above =
            decision.state == meteoris::detector::State::Active;
        const float metricDbHz = decision.frameMaxDbHz;

        if (_active)
        {
            _writer.append(frame, _eventId, 1, metricDbHz);
            ++_savedFrames;
            if (!above)
            {
                _active = false;
                _postRemaining = _postContextFrames;
                ++_triggerOffs;
                if (_postRemaining == 0)
                {
                    _eventStartNs = 0;
                    _writer.tick(frame.timestampNs, false);
                }
            }
            return;
        }

        if (_postRemaining > 0)
        {
            _writer.append(frame, _eventId, 2, metricDbHz);
            ++_savedFrames;

            if (above)
            {
                _active = true;
                _postRemaining = 0;
                ++_triggerOns;
            }
            else
            {
                --_postRemaining;
                if (_postRemaining == 0)
                {
                    _eventStartNs = 0;
                    _writer.tick(frame.timestampNs, false);
                }
            }
            remember(frame, metricDbHz);
            return;
        }

        if (above)
        {
            ++_eventId;
            ++_triggerOns;
            for (size_t i = 0; i < _pre.size(); ++i)
            {
                _writer.append(_pre[i].frame, _eventId, 0, _pre[i].maxDb);
                ++_savedFrames;
            }
            _pre.clear();
            _writer.append(frame, _eventId, 1, metricDbHz);
            ++_savedFrames;
            _active = true;
            _eventStartNs = frame.timestampNs;
        }
        else
        {
            remember(frame, metricDbHz);
        }
    }

    bool eventInProgress() const
    {
        return _active || _postRemaining > 0;
    }

    void printSummary() const
    {
        LOG_INFO_STREAM("detector_plugin=" << _cfg.detectorPlugin
                        << " detector_events=" << _eventId
                        << " trigger_ons=" << _triggerOns
                        << " trigger_offs=" << _triggerOffs
                        << " forced_cutoffs=" << _forcedCutoffs
                        << " peak_limit_drops=" << _peakLimitDrops
                        << " close_peak_suppressions=" << _closeSuppressions
                        << " last_detector_process_us=" << _lastDetectorProcessUs
                        << " saved_psd_frames=" << _savedFrames);
    }

private:
    struct BufferedFrame
    {
        PsdFrame frame;
        float maxDb = -300.0f;
    };

    meteoris::detector::Frame makeDetectorFrame(const PsdFrame &frame) const
    {
        meteoris::detector::Frame d;
        d.timestampNs = frame.timestampNs;
        d.frameIndex = frame.frameIndex;
        d.psd = frame.powerDensity.empty() ? nullptr : frame.powerDensity.data();
        d.bins = frame.powerDensity.size();
        d.frequencyStartHz = _frequencies.front();
        d.frequencyStepHz = _frequencies[1] - _frequencies[0];
        d.framePeriodSeconds = _framePeriodSeconds;
        return d;
    }

    static const char *objectTypeName(
        const meteoris::detector::ObjectType type)
    {
        switch (type)
        {
            case meteoris::detector::ObjectType::Stationary: return "stationary";
            case meteoris::detector::ObjectType::Chirp: return "chirp";
            default: return "unknown";
        }
    }

    static double metricValue(
        const meteoris::detector::Result &d,
        const char *name,
        const double fallback = 0.0)
    {
        for (size_t i = 0; i < d.debug.metricCount; ++i)
        {
            const auto &m = d.debug.metrics[i];
            if (m.name != nullptr && std::strcmp(m.name, name) == 0)
                return m.value;
        }
        return fallback;
    }

    void remember(const PsdFrame &frame, const float maxDb)
    {
        if (_preContextFrames == 0) return;
        BufferedFrame b;
        b.frame = frame;
        b.maxDb = maxDb;
        _pre.push_back(b);
        while (_pre.size() > _preContextFrames) _pre.pop_front();
    }

    void consumeDebugCounters(const meteoris::detector::Result &d)
    {
        _peakLimitDrops += static_cast<uint64_t>(
            std::max(0.0, metricValue(d, "dropped_by_max_peaks")));
        _closeSuppressions += static_cast<uint64_t>(
            std::max(0.0, metricValue(d, "close_suppressed")));
    }

    void emitWarnings(const PsdFrame &frame,
                      const meteoris::detector::Result &d)
    {
        const double dropped = metricValue(d, "dropped_by_max_peaks");
        if (dropped <= 0.0) return;

        const uint64_t intervalNs = UINT64_C(10) * UINT64_C(1000000000);
        if (_lastWarningNs != 0 &&
            frame.timestampNs < _lastWarningNs + intervalNs)
            return;
        _lastWarningNs = frame.timestampNs;

        spdlog::warn(
            "detector={} peak warning: raw_candidates={} close_suppressed={} "
            "dropped_by_max_peaks={} retained_peaks={}",
            _cfg.detectorPlugin,
            static_cast<uint64_t>(metricValue(d, "raw_candidates")),
            static_cast<uint64_t>(metricValue(d, "close_suppressed")),
            static_cast<uint64_t>(dropped),
            static_cast<uint64_t>(metricValue(d, "retained_peaks")));
    }

    void printDiagnostic(const PsdFrame &frame,
                         const meteoris::detector::Result &d)
    {
        if (_cfg.detectorDiagnosticIntervalSeconds <= 0.0) return;
        const uint64_t intervalNs = static_cast<uint64_t>(
            std::llround(_cfg.detectorDiagnosticIntervalSeconds * 1e9));
        if (_lastDiagnosticNs != 0 &&
            frame.timestampNs < _lastDiagnosticNs + intervalNs)
            return;
        _lastDiagnosticNs = frame.timestampNs;

        std::ostringstream o;
        o << std::fixed << std::setprecision(2)
          << "DET plugin=" << _cfg.detectorPlugin
          << " state="
          << (d.state == meteoris::detector::State::WarmingUp ? "WARMUP"
              : d.state == meteoris::detector::State::Active ? "ACTIVE"
              : "IDLE")
          << " event="
          << (_active ? "ACTIVE" : (_postRemaining > 0 ? "POST" : "IDLE"))
          << " active_objects=" << d.activeObjects
          << " process_us=" << _lastDetectorProcessUs;

        if (d.debug.metricCount != 0)
        {
            o << " metrics{";
            for (size_t i = 0; i < d.debug.metricCount; ++i)
            {
                if (i != 0) o << ' ';
                const auto &m = d.debug.metrics[i];
                o << (m.name ? m.name : "?") << '=' << m.value;
                if (m.unit && *m.unit) o << m.unit;
            }
            o << '}';
        }

        // Print the most relevant object: active object first, otherwise the
        // oldest tentative object. Full object vectors remain available
        // structurally to future HDF5/replay/plot tools.
        const meteoris::detector::Object *chosen = nullptr;
        for (size_t i = 0; i < d.debug.objectCount; ++i)
        {
            const auto &obj = d.debug.objects[i];
            if (obj.active)
            {
                chosen = &obj;
                break;
            }
            if (chosen == nullptr || obj.ageSeconds > chosen->ageSeconds)
                chosen = &obj;
        }

        if (chosen != nullptr)
        {
            o << (chosen->active ? " object{" : " tentative{")
              << "id=" << chosen->id
              << " type=" << objectTypeName(chosen->type)
              << " freq=" << chosen->frequencyHz << "Hz"
              << " velocity=" << chosen->frequencyRateHzS << "Hz/s"
              << " excess=" << chosen->excessDb << "dB"
              << " age=" << chosen->ageSeconds << "s"
              << " occupancy=" << chosen->occupancy
              << " hits=" << chosen->hits << '/' << chosen->opportunities
              << " missed=" << chosen->missedSeconds << "s}";
        }

        spdlog::debug("{}", o.str());
    }

    const Config &_cfg;
    const std::vector<double> &_frequencies;
    double _framePeriodSeconds = 0.0;
    std::unique_ptr<meteoris::detector::IDetector> _detector;
    DailyHdf5Writer _writer;
    std::deque<BufferedFrame> _pre;
    size_t _preContextFrames = 0;
    size_t _postContextFrames = 0;
    bool _active = false;
    size_t _postRemaining = 0;
    uint64_t _eventStartNs = 0;
    uint64_t _rearmUntilNs = 0;
    uint64_t _eventId = 0;
    uint64_t _triggerOns = 0;
    uint64_t _triggerOffs = 0;
    uint64_t _forcedCutoffs = 0;
    uint64_t _savedFrames = 0;
    uint64_t _peakLimitDrops = 0;
    uint64_t _closeSuppressions = 0;
    uint64_t _lastDiagnosticNs = 0;
    uint64_t _lastWarningNs = 0;
    double _lastDetectorProcessUs = 0.0;
};

std::string effectiveToml(const Config &c)
{
    std::ostringstream o;
    o << "[sdr]\n"
      << "driver = \"" << c.driver << "\"\n"
      << "sample_rate = " << c.sampleRate << "\n"
      << "center_frequency = " << c.centerFrequency << "\n"
      << "gain = " << c.gain << "\n";
    if (c.driver == "meteoris_sim")
    {
        o << "\n[simulator]\n"
          << "period_s = " << c.simulatorPeriodSeconds << "\n"
          << "noise_amplitude = " << c.simulatorNoiseAmplitude << "\n"
          << "stationary_enabled = " << (c.simulatorStationaryEnabled ? "true" : "false") << "\n"
          << "stationary_start_s = " << c.simulatorStationaryStartSeconds << "\n"
          << "stationary_rise_s = " << c.simulatorStationaryRiseSeconds << "\n"
          << "stationary_hold_s = " << c.simulatorStationaryHoldSeconds << "\n"
          << "stationary_fall_s = " << c.simulatorStationaryFallSeconds << "\n"
          << "stationary_offset_hz = " << c.simulatorStationaryOffsetHz << "\n"
          << "stationary_amplitude = " << c.simulatorStationaryAmplitude << "\n"
          << "chirp_enabled = " << (c.simulatorChirpEnabled ? "true" : "false") << "\n"
          << "chirp_start_s = " << c.simulatorChirpStartSeconds << "\n"
          << "chirp_duration_s = " << c.simulatorChirpDurationSeconds << "\n"
          << "chirp_start_offset_hz = " << c.simulatorChirpStartOffsetHz << "\n"
          << "chirp_end_offset_hz = " << c.simulatorChirpEndOffsetHz << "\n"
          << "chirp_amplitude = " << c.simulatorChirpAmplitude << "\n";
    }
    o << "\n[dsp]\n"
      << "shift_hz = " << c.shiftHz << "\n"
      << "decimation_stage1 = " << c.decim1 << "\n"
      << "decimation_stage2 = " << c.decim2 << "\n"
      << "bandwidth_hz = " << c.bandwidth << "\n"
      << "attenuation_db = " << c.attenuationDb << "\n\n[psd]\n"
      << "fft_size = " << c.nfft << "\n\n[runtime]\n"
      << "daemon = " << (c.daemonMode ? "true" : "false") << "\n"
      << "run_seconds = " << c.runSeconds << "\n"
      << "stream_buffers = " << c.streamBuffers << "\n"
      << "direct_buffer = " << (c.direct ? "true" : "false") << "\n"
      << "fir1_threads = " << c.fir1Threads << "\n\n[logging]\n"
      << "level = \"" << c.logLevel << "\"\n"
      << "color = " << (c.logColor ? "true" : "false") << "\n"
      << "file_enabled = " << (c.logFileEnabled ? "true" : "false") << "\n"
      << "file = \"" << c.logFile << "\"\n"
      << "file_truncate = " << (c.logFileTruncate ? "true" : "false") << "\n"
      << "daily_rotation = " << (c.logDailyRotation ? "true" : "false") << "\n"
      << "daily_rotate_time = \"" << formatDailyRotateTime(c.logDailyRotateMinuteUtc) << "\"\n\n[agc]\n"
      << "enabled = " << (c.agcEnabled ? "true" : "false") << "\n"
      << "target_saturation_percent = " << c.agcTargetSaturationPercent << "\n"
      << "time_constant_s = " << c.agcTimeConstantSeconds << "\n"
      << "clip_level = " << c.agcClipLevel << "\n"
      << "min_gain_db = " << c.agcMinGain << "\n"
      << "max_gain_db = " << c.agcMaxGain << "\n"
      << "max_step_db = " << c.agcMaxStepDb << "\n\n[recording]\n"
      << "mode = \"" << c.recordingMode << "\"\n"
      << "segment_seconds = " << c.recordingSegmentSeconds << "\n"
      << "segment_count = " << c.recordingSegmentCount << "\n\n[detector]\n"
      << "enabled = " << (c.detectorEnabled ? "true" : "false") << "\n"
      << "plugin = \"" << c.detectorPlugin << "\"\n"
      << "threads = " << c.detectorThreads << "\n"
      << "frequency_mean_bins = " << c.detectorFrequencyMeanBins << "\n"
      << "time_mean_psds = " << c.detectorTimeMeanPsds << "\n"
      << "peak_threshold_db = " << c.detectorPeakThresholdDb << "\n"
      << "min_peak_separation_hz = " << c.detectorMinPeakSeparationHz << "\n"
      << "max_peaks_per_psd = " << c.detectorMaxPeaksPerPsd << "\n"
      << "diagnostic_interval_s = " << c.detectorDiagnosticIntervalSeconds << "\n"
      << "pre_context_s = " << c.detectorPreContextSeconds << "\n"
      << "post_context_s = " << c.detectorPostContextSeconds << "\n"
      << "max_event_seconds = " << c.detectorMaxEventSeconds << "\n"
      << "rearm_seconds = " << c.detectorRearmSeconds << "\n\n[detector.stationary]\n"
      << "enabled = " << (c.detectorStationaryEnabled ? "true" : "false") << "\n"
      << "min_hz = " << c.detectorStationaryMinHz << "\n"
      << "max_hz = " << c.detectorStationaryMaxHz << "\n"
      << "max_df_hz = " << c.detectorStationaryMaxDfHz << "\n"
      << "activation_time_s = " << c.detectorStationaryActivationSeconds << "\n"
      << "activation_fraction = " << c.detectorStationaryActivationFraction << "\n"
      << "lost_s = " << c.detectorStationaryLostSeconds << "\n"
      << "max_drift_hz_s = " << c.detectorStationaryMaxDriftHzS << "\n\n[detector.chirp]\n"
      << "enabled = " << (c.detectorChirpEnabled ? "true" : "false") << "\n"
      << "min_hz = " << c.detectorChirpMinHz << "\n"
      << "max_hz = " << c.detectorChirpMaxHz << "\n"
      << "max_df_hz = " << c.detectorChirpMaxDfHz << "\n"
      << "activation_time_s = " << c.detectorChirpActivationSeconds << "\n"
      << "activation_fraction = " << c.detectorChirpActivationFraction << "\n"
      << "lost_s = " << c.detectorChirpLostSeconds << "\n"
      << "min_drift_hz_s = " << c.detectorChirpMinDriftHzS << "\n"
      << "max_drift_hz_s = " << c.detectorChirpMaxDriftHzS << "\n\n[output]\n"
      << "directory = \"" << c.detectorOutputDirectory << "\"\n"
      << "file_prefix = \"" << c.detectorFilePrefix << "\"\n"
      << "hdf5_compression = " << c.detectorCompression << "\n"
      << "swmr = " << (c.outputSwmr ? "true" : "false") << "\n"
      << "swmr_flush_seconds = " << c.outputSwmrFlushSeconds << "\n"
      << "daily_rotate_time = \"" << formatDailyRotateTime(c.outputDailyRotateMinuteUtc) << "\"\n"
      << "max_growth_mb_per_min = " << c.outputMaxGrowthMbPerMin << "\n"
      << "min_free_space_gb = " << c.outputMinFreeGb << "\n";
    return o.str();
}

std::string trim(const std::string &in)
{
    size_t a = 0, b = in.size();
    while (a < b && std::isspace(static_cast<unsigned char>(in[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(in[b - 1]))) --b;
    return in.substr(a, b - a);
}

std::string unquote(const std::string &v)
{
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

bool parseBool(const std::string &v)
{
    if (v == "true") return true;
    if (v == "false") return false;
    throw std::runtime_error("invalid TOML boolean: " + v);
}

int parseDailyRotateTime(const std::string &raw)
{
    const std::string value = unquote(trim(raw));
    if (value.size() != 5 || value[2] != ':' ||
        !std::isdigit(static_cast<unsigned char>(value[0])) ||
        !std::isdigit(static_cast<unsigned char>(value[1])) ||
        !std::isdigit(static_cast<unsigned char>(value[3])) ||
        !std::isdigit(static_cast<unsigned char>(value[4])))
        throw std::runtime_error("output.daily_rotate_time must use HH:MM");
    const int hour = (value[0] - '0') * 10 + (value[1] - '0');
    const int minute = (value[3] - '0') * 10 + (value[4] - '0');
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        throw std::runtime_error("output.daily_rotate_time must be in 00:00..23:59");
    return hour * 60 + minute;
}

int parseLoggingDailyRotateTime(const std::string &raw)
{
    const std::string value = unquote(trim(raw));
    if (value.size() != 5 || value[2] != ':' ||
        !std::isdigit(static_cast<unsigned char>(value[0])) ||
        !std::isdigit(static_cast<unsigned char>(value[1])) ||
        !std::isdigit(static_cast<unsigned char>(value[3])) ||
        !std::isdigit(static_cast<unsigned char>(value[4])))
        throw std::runtime_error("logging.daily_rotate_time must use HH:MM");
    const int hour = (value[0] - '0') * 10 + (value[1] - '0');
    const int minute = (value[3] - '0') * 10 + (value[4] - '0');
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        throw std::runtime_error("logging.daily_rotate_time must be in 00:00..23:59");
    return hour * 60 + minute;
}

void applyTomlValue(Config &c, const std::string &key, const std::string &raw)
{
    const std::string v = trim(raw);
    if (key == "sdr.driver") c.driver = unquote(v);
    else if (key == "sdr.sample_rate") c.sampleRate = std::stod(v);
    else if (key == "sdr.center_frequency") c.centerFrequency = std::stod(v);
    else if (key == "sdr.gain") c.gain = std::stod(v);
    else if (key == "simulator.period_s") c.simulatorPeriodSeconds = std::stod(v);
    else if (key == "simulator.noise_amplitude") c.simulatorNoiseAmplitude = std::stod(v);
    else if (key == "simulator.stationary_enabled") c.simulatorStationaryEnabled = parseBool(v);
    else if (key == "simulator.stationary_start_s") c.simulatorStationaryStartSeconds = std::stod(v);
    else if (key == "simulator.stationary_rise_s") c.simulatorStationaryRiseSeconds = std::stod(v);
    else if (key == "simulator.stationary_hold_s") c.simulatorStationaryHoldSeconds = std::stod(v);
    else if (key == "simulator.stationary_fall_s") c.simulatorStationaryFallSeconds = std::stod(v);
    else if (key == "simulator.stationary_offset_hz") c.simulatorStationaryOffsetHz = std::stod(v);
    else if (key == "simulator.stationary_amplitude") c.simulatorStationaryAmplitude = std::stod(v);
    else if (key == "simulator.chirp_enabled") c.simulatorChirpEnabled = parseBool(v);
    else if (key == "simulator.chirp_start_s") c.simulatorChirpStartSeconds = std::stod(v);
    else if (key == "simulator.chirp_duration_s") c.simulatorChirpDurationSeconds = std::stod(v);
    else if (key == "simulator.chirp_start_offset_hz") c.simulatorChirpStartOffsetHz = std::stod(v);
    else if (key == "simulator.chirp_end_offset_hz") c.simulatorChirpEndOffsetHz = std::stod(v);
    else if (key == "simulator.chirp_amplitude") c.simulatorChirpAmplitude = std::stod(v);
    else if (key == "dsp.shift_hz") c.shiftHz = std::stod(v);
    else if (key == "dsp.decimation_stage1") c.decim1 = static_cast<size_t>(std::stoull(v));
    else if (key == "dsp.decimation_stage2") c.decim2 = static_cast<size_t>(std::stoull(v));
    else if (key == "dsp.bandwidth_hz") c.bandwidth = std::stod(v);
    else if (key == "dsp.attenuation_db") c.attenuationDb = std::stod(v);
    else if (key == "psd.fft_size") c.nfft = static_cast<size_t>(std::stoull(v));
    else if (key == "runtime.daemon") c.daemonMode = parseBool(v);
    else if (key == "runtime.run_seconds") c.runSeconds = std::stod(v);
    else if (key == "runtime.stream_buffers") c.streamBuffers = static_cast<size_t>(std::stoull(v));
    else if (key == "runtime.direct_buffer") c.direct = parseBool(v);
    else if (key == "runtime.fir1_threads") c.fir1Threads = static_cast<size_t>(std::stoull(v));
    else if (key == "logging.level") c.logLevel = unquote(v);
    else if (key == "logging.color") c.logColor = parseBool(v);
    else if (key == "logging.file_enabled") c.logFileEnabled = parseBool(v);
    else if (key == "logging.file") c.logFile = unquote(v);
    else if (key == "logging.file_truncate") c.logFileTruncate = parseBool(v);
    else if (key == "logging.daily_rotation") c.logDailyRotation = parseBool(v);
    else if (key == "logging.daily_rotate_time") c.logDailyRotateMinuteUtc = parseLoggingDailyRotateTime(v);
    else if (key == "agc.enabled") c.agcEnabled = parseBool(v);
    else if (key == "agc.target_saturation_percent") c.agcTargetSaturationPercent = std::stod(v);
    else if (key == "agc.time_constant_s") c.agcTimeConstantSeconds = std::stod(v);
    else if (key == "agc.clip_level") c.agcClipLevel = std::stoi(v);
    else if (key == "agc.min_gain_db") c.agcMinGain = std::stod(v);
    else if (key == "agc.max_gain_db") c.agcMaxGain = std::stod(v);
    else if (key == "agc.max_step_db") c.agcMaxStepDb = std::stod(v);
    else if (key == "recording.mode") c.recordingMode = unquote(v);
    else if (key == "recording.segment_seconds") c.recordingSegmentSeconds = std::stod(v);
    else if (key == "recording.segment_count") c.recordingSegmentCount = std::stoull(v);
    else if (key == "detector.enabled") c.detectorEnabled = parseBool(v);
    else if (key == "detector.plugin") c.detectorPlugin = unquote(v);
    else if (key == "detector.threads") c.detectorThreads = static_cast<unsigned>(std::stoul(v));
    else if (key == "detector.frequency_mean_bins") c.detectorFrequencyMeanBins = static_cast<size_t>(std::stoull(v));
    else if (key == "detector.time_mean_psds") c.detectorTimeMeanPsds = static_cast<size_t>(std::stoull(v));
    else if (key == "detector.peak_threshold_db") c.detectorPeakThresholdDb = std::stod(v);
    else if (key == "detector.min_peak_separation_hz") c.detectorMinPeakSeparationHz = std::stod(v);
    else if (key == "detector.max_peaks_per_psd") c.detectorMaxPeaksPerPsd = static_cast<size_t>(std::stoull(v));
    else if (key == "detector.diagnostic_interval_s") c.detectorDiagnosticIntervalSeconds = std::stod(v);
    else if (key == "detector.pre_context_s") c.detectorPreContextSeconds = std::stod(v);
    else if (key == "detector.post_context_s") c.detectorPostContextSeconds = std::stod(v);
    else if (key == "detector.max_event_seconds") c.detectorMaxEventSeconds = std::stod(v);
    else if (key == "detector.rearm_seconds") c.detectorRearmSeconds = std::stod(v);
    else if (key == "detector.stationary.enabled") c.detectorStationaryEnabled = parseBool(v);
    else if (key == "detector.stationary.min_hz") c.detectorStationaryMinHz = std::stod(v);
    else if (key == "detector.stationary.max_hz") c.detectorStationaryMaxHz = std::stod(v);
    else if (key == "detector.stationary.max_df_hz") c.detectorStationaryMaxDfHz = std::stod(v);
    else if (key == "detector.stationary.activation_time_s") c.detectorStationaryActivationSeconds = std::stod(v);
    else if (key == "detector.stationary.activation_fraction") c.detectorStationaryActivationFraction = std::stod(v);
    else if (key == "detector.stationary.lost_s") c.detectorStationaryLostSeconds = std::stod(v);
    else if (key == "detector.stationary.max_drift_hz_s") c.detectorStationaryMaxDriftHzS = std::stod(v);
    else if (key == "detector.chirp.enabled") c.detectorChirpEnabled = parseBool(v);
    else if (key == "detector.chirp.min_hz") c.detectorChirpMinHz = std::stod(v);
    else if (key == "detector.chirp.max_hz") c.detectorChirpMaxHz = std::stod(v);
    else if (key == "detector.chirp.max_df_hz") c.detectorChirpMaxDfHz = std::stod(v);
    else if (key == "detector.chirp.activation_time_s") c.detectorChirpActivationSeconds = std::stod(v);
    else if (key == "detector.chirp.activation_fraction") c.detectorChirpActivationFraction = std::stod(v);
    else if (key == "detector.chirp.lost_s") c.detectorChirpLostSeconds = std::stod(v);
    else if (key == "detector.chirp.min_drift_hz_s") c.detectorChirpMinDriftHzS = std::stod(v);
    else if (key == "detector.chirp.max_drift_hz_s") c.detectorChirpMaxDriftHzS = std::stod(v);
    else if (key == "output.directory") c.detectorOutputDirectory = unquote(v);
    else if (key == "output.file_prefix") c.detectorFilePrefix = unquote(v);
    else if (key == "output.hdf5_compression") c.detectorCompression = std::stoi(v);
    else if (key == "output.swmr") c.outputSwmr = parseBool(v);
    else if (key == "output.swmr_flush_seconds") c.outputSwmrFlushSeconds = std::stod(v);
    else if (key == "output.daily_rotate_time") c.outputDailyRotateMinuteUtc = parseDailyRotateTime(v);
    else if (key == "output.max_growth_mb_per_min") c.outputMaxGrowthMbPerMin = std::stod(v);
    else if (key == "output.min_free_space_gb") c.outputMinFreeGb = std::stod(v);
    else throw std::runtime_error("unknown TOML key: " + key);
}

void loadToml(Config &c, const std::string &path)
{
    std::ifstream f(path.c_str());
    if (!f) throw std::runtime_error("cannot open config file: " + path);
    std::string section, line;
    size_t lineNo = 0;
    while (std::getline(f, line))
    {
        ++lineNo;
        // Strip comments outside quoted strings.
        bool quoted = false;
        size_t comment = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) quoted = !quoted;
            if (line[i] == '#' && !quoted) { comment = i; break; }
        }
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": expected key = value");
        const std::string key = trim(line.substr(0, eq));
        const std::string full = section.empty() ? key : section + "." + key;
        try { applyTomlValue(c, full, line.substr(eq + 1)); }
        catch (const std::exception &e)
        { throw std::runtime_error(path + ":" + std::to_string(lineNo) + ": " + e.what()); }
    }
}

void daemonizeProcess()
{
    pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("first fork failed: " + std::string(std::strerror(errno)));
    if (pid > 0) std::exit(EXIT_SUCCESS);

    if (::setsid() < 0)
        throw std::runtime_error("setsid failed: " + std::string(std::strerror(errno)));

    pid = ::fork();
    if (pid < 0) throw std::runtime_error("second fork failed: " + std::string(std::strerror(errno)));
    if (pid > 0) std::exit(EXIT_SUCCESS);

    // Keep the launch working directory so relative [output] paths retain
    // exactly the same meaning as in foreground mode.
    ::umask(027);

    const int nullFd = ::open("/dev/null", O_RDWR);
    if (nullFd < 0)
        throw std::runtime_error("cannot open /dev/null: " + std::string(std::strerror(errno)));
    if (::dup2(nullFd, STDIN_FILENO) < 0 ||
        ::dup2(nullFd, STDOUT_FILENO) < 0 ||
        ::dup2(nullFd, STDERR_FILENO) < 0)
    {
        const int e = errno;
        if (nullFd > STDERR_FILENO) ::close(nullFd);
        throw std::runtime_error("dup2(/dev/null) failed: " + std::string(std::strerror(e)));
    }
    if (nullFd > STDERR_FILENO) ::close(nullFd);

}


std::string datedLogPath(const std::string &basePath, const std::string &day)
{
    const size_t slash = basePath.find_last_of('/');
    const size_t dot = basePath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return basePath.substr(0, dot) + "_" + day + basePath.substr(dot);
    return basePath + "_" + day;
}

std::string parentDirectory(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

class DailyUtcFileSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    DailyUtcFileSink(std::string basePath, const int rotateMinuteUtc,
                     const bool truncateOnStartup)
        : _basePath(std::move(basePath)),
          _rotateMinuteUtc(rotateMinuteUtc),
          _truncateOnStartup(truncateOnStartup)
    {
        if (_basePath.empty())
            throw std::runtime_error("logging.file must not be empty");
        const std::string parent = parentDirectory(_basePath);
        if (!parent.empty()) ensureDirectory(parent);
    }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override
    {
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                msg.time.time_since_epoch()).count());
        const std::string day = rotationDayUtc(ns, _rotateMinuteUtc);
        if (!_stream.is_open() || day != _day)
            openDay(day);

        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        _stream.write(formatted.data(),
                      static_cast<std::streamsize>(formatted.size()));
        if (!_stream)
            throw spdlog::spdlog_ex("failed writing Meteoris log file: " + _path);
    }

    void flush_() override
    {
        if (_stream.is_open()) _stream.flush();
    }

private:
    void openDay(const std::string &day)
    {
        if (_stream.is_open())
        {
            _stream.flush();
            _stream.close();
        }

        _day = day;
        _path = datedLogPath(_basePath, day);
        const bool firstOpen = !_openedOnce;
        const bool truncate = firstOpen && _truncateOnStartup;
        std::ios_base::openmode mode = std::ios::out;
        mode |= truncate ? std::ios::trunc : std::ios::app;
        _stream.open(_path.c_str(), mode);
        if (!_stream)
            throw spdlog::spdlog_ex("cannot open Meteoris log file: " + _path);
        _openedOnce = true;
    }

    std::string _basePath;
    int _rotateMinuteUtc = 0;
    bool _truncateOnStartup = false;
    bool _openedOnce = false;
    std::string _day;
    std::string _path;
    std::ofstream _stream;
};

void configureLogging(const Config &cfg)
{
    const spdlog::level::level_enum level = parseLogLevel(cfg.logLevel);
    std::vector<spdlog::sink_ptr> sinks;

    // Foreground mode logs to the colored terminal and to the text file.
    // Daemon mode has no console sink because stdio is redirected to /dev/null;
    // it logs to the same text-file sink only. Syslog is intentionally unused.
    if (!cfg.daemonMode)
    {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>(
            cfg.logColor ? spdlog::color_mode::automatic : spdlog::color_mode::never);
        console->set_color(spdlog::level::trace, console->white);
        console->set_color(spdlog::level::debug, console->white);
        console->set_color(spdlog::level::info, console->green);
        console->set_color(spdlog::level::warn, console->yellow);
        console->set_color(spdlog::level::err, console->red);
        console->set_color(spdlog::level::critical, console->red_bold);
        // Color only the complete formatted console line.
        console->set_pattern("%^%E.%e [%l] %v%$");
        sinks.push_back(console);
    }

    if (cfg.logFileEnabled)
    {
        if (cfg.logFile.empty())
            throw std::runtime_error("logging.file must not be empty when file logging is enabled");

        spdlog::sink_ptr file;
        if (cfg.logDailyRotation)
        {
            file = std::make_shared<DailyUtcFileSink>(
                cfg.logFile, cfg.logDailyRotateMinuteUtc, cfg.logFileTruncate);
        }
        else
        {
            const std::string parent = parentDirectory(cfg.logFile);
            if (!parent.empty()) ensureDirectory(parent);
            file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                cfg.logFile, cfg.logFileTruncate);
        }

        // Text logs never contain ANSI color escape sequences.
        file->set_pattern("%E.%e [%l] %v");
        sinks.push_back(file);
    }

    if (sinks.empty())
        throw std::runtime_error("no logging sink enabled");

    auto logger = std::make_shared<spdlog::logger>(
        "meteoris", sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

SoapySDR::Device *getDevice(const Config &cfg)
{
    SoapySDR::Kwargs args = {{"driver", cfg.driver}};
    if (cfg.driver == "meteoris_sim")
    {
        args["period_s"] = std::to_string(cfg.simulatorPeriodSeconds);
        args["noise_amplitude"] = std::to_string(cfg.simulatorNoiseAmplitude);
        args["stationary_enabled"] = cfg.simulatorStationaryEnabled ? "true" : "false";
        args["stationary_start_s"] = std::to_string(cfg.simulatorStationaryStartSeconds);
        args["stationary_rise_s"] = std::to_string(cfg.simulatorStationaryRiseSeconds);
        args["stationary_hold_s"] = std::to_string(cfg.simulatorStationaryHoldSeconds);
        args["stationary_fall_s"] = std::to_string(cfg.simulatorStationaryFallSeconds);
        args["stationary_offset_hz"] = std::to_string(cfg.simulatorStationaryOffsetHz);
        args["stationary_amplitude"] = std::to_string(cfg.simulatorStationaryAmplitude);
        args["chirp_enabled"] = cfg.simulatorChirpEnabled ? "true" : "false";
        args["chirp_start_s"] = std::to_string(cfg.simulatorChirpStartSeconds);
        args["chirp_duration_s"] = std::to_string(cfg.simulatorChirpDurationSeconds);
        args["chirp_start_offset_hz"] = std::to_string(cfg.simulatorChirpStartOffsetHz);
        args["chirp_end_offset_hz"] = std::to_string(cfg.simulatorChirpEndOffsetHz);
        args["chirp_amplitude"] = std::to_string(cfg.simulatorChirpAmplitude);
    }

    const auto results = SoapySDR::Device::enumerate(args);
    LOG_INFO_STREAM("enumerate(driver=" << cfg.driver << ") -> "
                    << results.size() << " result(s)");
    if (results.empty()) throw std::runtime_error("No matching SDR devices found");

    // Enumeration results do not necessarily preserve arbitrary user kwargs.
    // Merge the selected hardware identity with the requested simulator args.
    SoapySDR::Kwargs makeArgs = results.front();
    for (const auto &kv : args) makeArgs[kv.first] = kv.second;
    return SoapySDR::Device::make(makeArgs);
}

void printUsage(const char *argv0)
{
    std::cout << "Usage: " << argv0 << " [--config meteoris.toml] [overrides...]\n"
              << "  --version\n"
              << "  --sample-rate HZ --center-frequency HZ --gain DB\n"
              << "  --run-seconds S --shift-hz HZ --decim1 N --decim2 N --bandwidth HZ\n"
              << "  --nfft N --stream-buffers N --direct | --no-direct --fir1-threads 1|2\n"
              << "  --daemon | --no-daemon --log-level LEVEL --log-color | --no-log-color\n"
              << "  --log-file PATH --log-file-enabled | --no-log-file --log-file-truncate | --log-file-append\n"
              << "  --log-daily-rotation | --no-log-daily-rotation --log-daily-rotate-time HH:MM\n"
              << "  --agc | --no-agc --agc-target PERCENT --agc-time-constant S\n"
              << "  --agc-clip-level N --agc-min-gain DB --agc-max-gain DB --agc-max-step DB\n"
              << "  --record N --record-mode triggered|continuous --record-segment-seconds S\n"
              << "  --detector | --no-detector --detector-plugin NAME --detector-threads N\n"
              << "  --detector-frequency-mean-bins N --detector-time-mean-psds N\n"
              << "  --detector-peak-threshold DB --detector-min-peak-separation HZ --detector-max-peaks N\n"
              << "  --detector-diagnostic-interval S --detector-pre-context S --detector-post-context S\n"
              << "  --detector-max-event-seconds S --detector-rearm-seconds S\n"
              << "  --stationary | --no-stationary --stationary-min HZ --stationary-max HZ --stationary-max-df HZ\n"
              << "  --stationary-activation-time S --stationary-activation-fraction F --stationary-lost S --stationary-max-drift HZ_S\n"
              << "  --chirp | --no-chirp --chirp-min HZ --chirp-max HZ --chirp-max-df HZ\n"
              << "  --chirp-activation-time S --chirp-activation-fraction F --chirp-lost S\n"
              << "  --chirp-min-drift HZ_S --chirp-max-drift HZ_S\n"
              << "  --output-directory DIR --swmr | --no-swmr --swmr-flush-seconds S\n"
              << "Precedence: defaults < TOML config < command-line overrides\n";
}

Config parseArgs(const int argc, char **argv)
{
    Config c;
    // Pass 1: locate an explicit config. If none is given, automatically use
    // ./meteoris.toml when it exists. CLI values are applied afterwards.
    bool explicitConfig = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a(argv[i]);
        if (a == "--config")
        {
            if (i + 1 >= argc) throw std::runtime_error("missing value after --config");
            c.configPath = argv[++i];
            explicitConfig = true;
        }
    }
    if (!explicitConfig)
    {
        std::ifstream probe("meteoris.toml");
        if (probe.good()) c.configPath = "meteoris.toml";
    }
    if (!c.configPath.empty()) loadToml(c, c.configPath);

    // Pass 2: command-line values override both defaults and TOML.
    for (int i = 1; i < argc; ++i)
    {
        const std::string a(argv[i]);
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
            return argv[++i];
        };
        if (a == "--config") { ++i; }
        else if (a == "--sample-rate") c.sampleRate = std::stod(next());
        else if (a == "--center-frequency") c.centerFrequency = std::stod(next());
        else if (a == "--gain") c.gain = std::stod(next());
        else if (a == "--run-seconds") c.runSeconds = std::stod(next());
        else if (a == "--shift-hz") c.shiftHz = std::stod(next());
        else if (a == "--decim1") c.decim1 = static_cast<size_t>(std::stoull(next()));
        else if (a == "--decim2") c.decim2 = static_cast<size_t>(std::stoull(next()));
        else if (a == "--bandwidth") c.bandwidth = std::stod(next());
        else if (a == "--nfft") c.nfft = static_cast<size_t>(std::stoull(next()));
        else if (a == "--stream-buffers") c.streamBuffers = static_cast<size_t>(std::stoull(next()));
        else if (a == "--direct") c.direct = true;
        else if (a == "--no-direct") c.direct = false;
        else if (a == "--fir1-threads") c.fir1Threads = static_cast<size_t>(std::stoull(next()));
        else if (a == "--daemon") c.daemonMode = true;
        else if (a == "--no-daemon") c.daemonMode = false;
        else if (a == "--log-level") c.logLevel = next();
        else if (a == "--log-color") c.logColor = true;
        else if (a == "--no-log-color") c.logColor = false;
        else if (a == "--log-file") { c.logFile = next(); c.logFileEnabled = true; }
        else if (a == "--log-file-enabled") c.logFileEnabled = true;
        else if (a == "--no-log-file") c.logFileEnabled = false;
        else if (a == "--log-file-truncate") c.logFileTruncate = true;
        else if (a == "--log-file-append") c.logFileTruncate = false;
        else if (a == "--log-daily-rotation") c.logDailyRotation = true;
        else if (a == "--no-log-daily-rotation") c.logDailyRotation = false;
        else if (a == "--log-daily-rotate-time") c.logDailyRotateMinuteUtc = parseLoggingDailyRotateTime(next());
        else if (a == "--agc") c.agcEnabled = true;
        else if (a == "--no-agc") c.agcEnabled = false;
        else if (a == "--agc-target") c.agcTargetSaturationPercent = std::stod(next());
        else if (a == "--agc-time-constant") c.agcTimeConstantSeconds = std::stod(next());
        else if (a == "--agc-clip-level") c.agcClipLevel = std::stoi(next());
        else if (a == "--agc-min-gain") c.agcMinGain = std::stod(next());
        else if (a == "--agc-max-gain") c.agcMaxGain = std::stod(next());
        else if (a == "--agc-max-step") c.agcMaxStepDb = std::stod(next());
        else if (a == "--record") { c.recordingMode = "continuous"; c.recordingSegmentCount = std::stoull(next()); }
        else if (a == "--record-mode") c.recordingMode = next();
        else if (a == "--record-segment-seconds") c.recordingSegmentSeconds = std::stod(next());
        else if (a == "--detector") c.detectorEnabled = true;
        else if (a == "--no-detector") c.detectorEnabled = false;
        else if (a == "--detector-plugin") c.detectorPlugin = next();
        else if (a == "--detector-threads") c.detectorThreads = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--detector-frequency-mean-bins") c.detectorFrequencyMeanBins = static_cast<size_t>(std::stoull(next()));
        else if (a == "--detector-time-mean-psds") c.detectorTimeMeanPsds = static_cast<size_t>(std::stoull(next()));
        else if (a == "--detector-peak-threshold") c.detectorPeakThresholdDb = std::stod(next());
        else if (a == "--detector-min-peak-separation") c.detectorMinPeakSeparationHz = std::stod(next());
        else if (a == "--detector-max-peaks") c.detectorMaxPeaksPerPsd = static_cast<size_t>(std::stoull(next()));
        else if (a == "--detector-diagnostic-interval") c.detectorDiagnosticIntervalSeconds = std::stod(next());
        else if (a == "--detector-pre-context") c.detectorPreContextSeconds = std::stod(next());
        else if (a == "--detector-post-context") c.detectorPostContextSeconds = std::stod(next());
        else if (a == "--detector-max-event-seconds") c.detectorMaxEventSeconds = std::stod(next());
        else if (a == "--detector-rearm-seconds") c.detectorRearmSeconds = std::stod(next());
        else if (a == "--stationary") c.detectorStationaryEnabled = true;
        else if (a == "--no-stationary") c.detectorStationaryEnabled = false;
        else if (a == "--stationary-min") c.detectorStationaryMinHz = std::stod(next());
        else if (a == "--stationary-max") c.detectorStationaryMaxHz = std::stod(next());
        else if (a == "--stationary-max-df") c.detectorStationaryMaxDfHz = std::stod(next());
        else if (a == "--stationary-activation-time") c.detectorStationaryActivationSeconds = std::stod(next());
        else if (a == "--stationary-activation-fraction") c.detectorStationaryActivationFraction = std::stod(next());
        else if (a == "--stationary-lost") c.detectorStationaryLostSeconds = std::stod(next());
        else if (a == "--stationary-max-drift") c.detectorStationaryMaxDriftHzS = std::stod(next());
        else if (a == "--chirp") c.detectorChirpEnabled = true;
        else if (a == "--no-chirp") c.detectorChirpEnabled = false;
        else if (a == "--chirp-min") c.detectorChirpMinHz = std::stod(next());
        else if (a == "--chirp-max") c.detectorChirpMaxHz = std::stod(next());
        else if (a == "--chirp-max-df") c.detectorChirpMaxDfHz = std::stod(next());
        else if (a == "--chirp-activation-time") c.detectorChirpActivationSeconds = std::stod(next());
        else if (a == "--chirp-activation-fraction") c.detectorChirpActivationFraction = std::stod(next());
        else if (a == "--chirp-lost") c.detectorChirpLostSeconds = std::stod(next());
        else if (a == "--chirp-min-drift") c.detectorChirpMinDriftHzS = std::stod(next());
        else if (a == "--chirp-max-drift") c.detectorChirpMaxDriftHzS = std::stod(next());
        else if (a == "--output-directory") c.detectorOutputDirectory = next();
        else if (a == "--swmr") c.outputSwmr = true;
        else if (a == "--no-swmr") c.outputSwmr = false;
        else if (a == "--swmr-flush-seconds") c.outputSwmrFlushSeconds = std::stod(next());
        else if (a == "--version")
        {
            std::cout << "meteoris " << METEORIS_VERSION << "\n"
                      << METEORIS_AUTHOR << std::endl;
            std::exit(EXIT_SUCCESS);
        }
        else if (a == "--help") { printUsage(argv[0]); std::exit(EXIT_SUCCESS); }
        else throw std::runtime_error("unknown argument: " + a);
    }

    if (c.sampleRate <= 0.0 || c.runSeconds < 0.0 || c.streamBuffers == 0 ||
        c.decim1 == 0 || c.decim2 == 0 || c.bandwidth <= 0.0)
        throw std::runtime_error("invalid zero/negative configuration value");
    if (c.nfft == 0 || (c.nfft & (c.nfft - 1)) != 0)
        throw std::runtime_error("psd.fft_size/--nfft must be a power of two");
    if (c.fir1Threads != 1 && c.fir1Threads != 2)
        throw std::runtime_error("runtime.fir1_threads/--fir1-threads must be 1 or 2");
    (void)parseLogLevel(c.logLevel);
    const double outputRate = c.sampleRate / double(c.decim1 * c.decim2);
    if (c.bandwidth >= outputRate)
        throw std::runtime_error("bandwidth must be smaller than final output sample rate");
    if (c.agcEnabled)
    {
        if (!(c.agcTargetSaturationPercent > 0.0 && c.agcTargetSaturationPercent <= 100.0))
            throw std::runtime_error("agc.target_saturation_percent must be in (0,100]");
        if (c.agcTimeConstantSeconds <= 0.0) throw std::runtime_error("agc.time_constant_s must be > 0");
        if (c.agcClipLevel < 1 || c.agcClipLevel > 128) throw std::runtime_error("agc.clip_level must be in [1,128]");
        if (c.agcMaxStepDb <= 0.0) throw std::runtime_error("agc.max_step_db must be > 0");
        if (c.agcMinGain >= 0.0 && c.agcMaxGain >= 0.0 && c.agcMinGain > c.agcMaxGain)
            throw std::runtime_error("agc.min_gain_db must be <= agc.max_gain_db");
    }
    if (c.driver == "meteoris_sim")
    {
        if (!(c.simulatorPeriodSeconds > 0.0))
            throw std::runtime_error("simulator.period_s must be > 0");
        if (c.simulatorNoiseAmplitude < 0.0 || c.simulatorStationaryAmplitude < 0.0 ||
            c.simulatorChirpAmplitude < 0.0)
            throw std::runtime_error("simulator amplitudes must be >= 0");
        if (c.simulatorStationaryStartSeconds < 0.0 ||
            c.simulatorStationaryRiseSeconds < 0.0 ||
            c.simulatorStationaryHoldSeconds < 0.0 ||
            c.simulatorStationaryFallSeconds < 0.0)
            throw std::runtime_error("simulator stationary timing must be >= 0");
        const double stationaryEnd = c.simulatorStationaryStartSeconds +
            c.simulatorStationaryRiseSeconds + c.simulatorStationaryHoldSeconds +
            c.simulatorStationaryFallSeconds;
        if (c.simulatorStationaryEnabled && stationaryEnd > c.simulatorPeriodSeconds)
            throw std::runtime_error("simulator stationary event exceeds period_s");
        if (c.simulatorChirpStartSeconds < 0.0 || !(c.simulatorChirpDurationSeconds > 0.0))
            throw std::runtime_error("simulator chirp timing is invalid");
        if (c.simulatorChirpEnabled &&
            c.simulatorChirpStartSeconds + c.simulatorChirpDurationSeconds > c.simulatorPeriodSeconds)
            throw std::runtime_error("simulator chirp event exceeds period_s");
    }

    if (c.recordingMode != "triggered" && c.recordingMode != "continuous")
        throw std::runtime_error("recording.mode must be triggered or continuous");
    if (!(c.recordingSegmentSeconds > 0.0))
        throw std::runtime_error("recording.segment_seconds must be > 0");
    if (c.recordingMode == "triggered" && !c.detectorEnabled)
        throw std::runtime_error("recording.mode=triggered requires detector.enabled=true");

    if (c.detectorEnabled)
    {
        if (c.detectorFrequencyMeanBins == 0 || (c.detectorFrequencyMeanBins & 1u) == 0)
            throw std::runtime_error("detector.frequency_mean_bins must be an odd positive integer");
        if (c.detectorTimeMeanPsds == 0)
            throw std::runtime_error("detector.time_mean_psds must be >= 1");
        if (!(c.detectorPeakThresholdDb > 0.0))
            throw std::runtime_error("detector.peak_threshold_db must be > 0");
        if (c.detectorMinPeakSeparationHz < 0.0)
            throw std::runtime_error("detector.min_peak_separation_hz must be >= 0");
        if (c.detectorMaxPeaksPerPsd == 0)
            throw std::runtime_error("detector.max_peaks_per_psd must be >= 1");
        if (c.detectorDiagnosticIntervalSeconds < 0.0)
            throw std::runtime_error("detector.diagnostic_interval_s must be >= 0");
        if (c.detectorPreContextSeconds < 0.0 || c.detectorPostContextSeconds < 0.0)
            throw std::runtime_error("detector pre/post context must be >= 0");
        if (c.detectorMaxEventSeconds < 0.0)
            throw std::runtime_error("detector.max_event_seconds must be >= 0");
        if (c.detectorRearmSeconds < 0.0)
            throw std::runtime_error("detector.rearm_seconds must be >= 0");
        if (!c.detectorStationaryEnabled && !c.detectorChirpEnabled)
            throw std::runtime_error("detector enabled but stationary and chirp tracking are both disabled");

        const double halfBand = c.bandwidth / 2.0;
        auto validateRange = [&](const char *name, bool enabled, double minHz, double maxHz,
                                 double maxDf, double activationTime, double activationFraction,
                                 double lostSeconds) {
            if (!enabled) return;
            if (!(minHz < maxHz && minHz >= -halfBand && maxHz <= halfBand))
                throw std::runtime_error(std::string(name) + " frequency range must lie inside +/-bandwidth/2");
            if (!(maxDf > 0.0) || activationTime < 0.0 ||
                !(activationFraction > 0.0 && activationFraction <= 1.0) ||
                lostSeconds < 0.0)
                throw std::runtime_error(std::string("invalid ") + name + " tracking parameters");
        };
        validateRange("detector.stationary", c.detectorStationaryEnabled,
                      c.detectorStationaryMinHz, c.detectorStationaryMaxHz,
                      c.detectorStationaryMaxDfHz, c.detectorStationaryActivationSeconds,
                      c.detectorStationaryActivationFraction, c.detectorStationaryLostSeconds);
        if (c.detectorStationaryEnabled && !(c.detectorStationaryMaxDriftHzS >= 0.0))
            throw std::runtime_error("detector.stationary.max_drift_hz_s must be >= 0");

        validateRange("detector.chirp", c.detectorChirpEnabled,
                      c.detectorChirpMinHz, c.detectorChirpMaxHz,
                      c.detectorChirpMaxDfHz, c.detectorChirpActivationSeconds,
                      c.detectorChirpActivationFraction, c.detectorChirpLostSeconds);
        if (c.detectorChirpEnabled &&
            !(c.detectorChirpMinDriftHzS >= 0.0 &&
              c.detectorChirpMinDriftHzS < c.detectorChirpMaxDriftHzS))
            throw std::runtime_error("detector.chirp drift limits must satisfy 0 <= min < max");

        if (c.detectorCompression < 0 || c.detectorCompression > 9)
            throw std::runtime_error("output.hdf5_compression must be in [0,9]");
        if (c.outputSwmrFlushSeconds < 0.0)
            throw std::runtime_error("output.swmr_flush_seconds must be >= 0");
        if (c.outputMaxGrowthMbPerMin < 0.0)
            throw std::runtime_error("output.max_growth_mb_per_min must be >= 0");
        if (c.outputMinFreeGb < 0.0)
            throw std::runtime_error("output.min_free_space_gb must be >= 0");
    }
    return c;
}
} // namespace

int main(int argc, char **argv)
{
    SoapySDR::Device *dev = nullptr;
    SoapySDR::Stream *rxStream = nullptr;
    bool activated = false;
    bool loggingConfigured = false;

    try
    {
        const Config cfg = parseArgs(argc, argv);
        if (cfg.daemonMode)
        {
            daemonizeProcess();
        }
        configureLogging(cfg);
        loggingConfigured = true;
        spdlog::info("daemon={}", cfg.daemonMode ? "ON" : "OFF");

        // SIGINT/SIGTERM only request shutdown.  The main loop performs all
        // non-async-signal-safe cleanup (release buffer, stop Soapy, flush and
        // close HDF5) in normal process context.
        installSignalHandlers();

        const double outputRate = cfg.sampleRate / double(cfg.decim1 * cfg.decim2);
        const double fs1 = cfg.sampleRate / double(cfg.decim1);
        const double passEdge = cfg.bandwidth / 2.0;
        // Stage 1 stop edge protects the /decim1 Nyquist boundary while leaving
        // a useful transition band. Stage 2 completes the channel selection.
        const double stop1 = 0.96 * (fs1 / 2.0);
        if (!(passEdge < stop1)) throw std::runtime_error("stage-1 transition band is invalid");
        const auto taps1 = designLowpass(cfg.sampleRate, passEdge, stop1, cfg.attenuationDb);
        const auto taps2 = designLowpass(fs1, passEdge, outputRate / 2.0, cfg.attenuationDb);

        IntegratedDspChain dsp(taps1, taps2, cfg.shiftHz, cfg.sampleRate,
                               cfg.decim1, cfg.decim2, cfg.bandwidth, cfg.nfft, cfg.fir1Threads);

        dev = getDevice(cfg);
        dev->setSampleRate(SOAPY_SDR_RX, 0, cfg.sampleRate);
        if (cfg.centerFrequency > 0.0) dev->setFrequency(SOAPY_SDR_RX, 0, cfg.centerFrequency);
        if (cfg.gain >= 0.0) dev->setGain(SOAPY_SDR_RX, 0, cfg.gain);
        const std::vector<size_t> channels = {0};
        const SoapySDR::Kwargs streamArgs = {{"buffers", std::to_string(cfg.streamBuffers)}};
        rxStream = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, channels, streamArgs);
        const size_t streamMtu = dev->getStreamMTU(rxStream);
        size_t directBuffers = 0;
        bool useDirect = cfg.direct;
        if (useDirect)
        {
            try
            {
                directBuffers = dev->getNumDirectAccessBuffers(rxStream);
                if (directBuffers == 0) useDirect = false;
            }
            catch (...)
            {
                useDirect = false;
            }
        }
        // Used only if direct buffer acquisition is disabled/not supported.
        std::vector<int8_t> fallbackIq(useDirect ? 0 : 2 * streamMtu);

        const bool finiteRun = cfg.runSeconds > 0.0;
        const size_t targetInput = finiteRun
            ? static_cast<size_t>(std::llround(cfg.runSeconds * cfg.sampleRate))
            : std::numeric_limits<size_t>::max();
        size_t totalInput = 0;
        size_t totalOutput = 0;
        size_t readCalls = 0;
        size_t timeouts = 0;
        size_t overflows = 0;
        size_t blockCount = 0;
        size_t minInputBlockSamples = std::numeric_limits<size_t>::max();
        size_t maxInputBlockSamples = 0;
        size_t firstInputBlockSamples = 0;
        bool loggedFirstInputBlock = false;
        size_t deadlineMisses = 0;
        std::vector<double> blockDspTimes;
        double dspSeconds = 0.0;
        double maxBlockDsp = 0.0;

        double currentBandwidth = 0.0;
        try { currentBandwidth = dev->getBandwidth(SOAPY_SDR_RX, 0); } catch (...) {}
        double currentFrequency = cfg.centerFrequency;
        try { currentFrequency = dev->getFrequency(SOAPY_SDR_RX, 0); } catch (...) {}
        double currentGain = cfg.gain;
        try { currentGain = dev->getGain(SOAPY_SDR_RX, 0); } catch (...) {}
        double deviceMinGain = 0.0, deviceMaxGain = 100.0;
        try
        {
            const auto gr = dev->getGainRange(SOAPY_SDR_RX, 0);
            deviceMinGain = gr.minimum();
            deviceMaxGain = gr.maximum();
        }
        catch (...) {}
        ClippingAgc agc(cfg, dev, currentGain, deviceMinGain, deviceMaxGain);
        currentGain = agc.gain();
        std::ostringstream hw;
        try
        {
            const auto info = dev->getHardwareInfo();
            bool first = true;
            for (const auto &kv : info) { if (!first) hw << "; "; hw << kv.first << "=" << kv.second; first = false; }
        } catch (...) {}

        spdlog::info("=== Full DSP-chain benchmark: C++ ===");
        LOG_INFO_STREAM("meteoris version=" << METEORIS_VERSION
                        << " author=\"" << METEORIS_AUTHOR << "\"");
        LOG_INFO_STREAM("config=" << (cfg.configPath.empty() ? "<defaults+CLI>" : cfg.configPath));
        LOG_INFO_STREAM("input=" << cfg.sampleRate << " sps shift=" << std::showpos
                        << cfg.shiftHz << std::noshowpos << " Hz decimation="
                        << cfg.decim1 << "x" << cfg.decim2 << "=" << (cfg.decim1 * cfg.decim2)
                        << " output=" << outputRate << " sps band=" << cfg.bandwidth << " Hz");
        LOG_INFO_STREAM("FIR1=" << taps1.size() << " taps @ " << cfg.sampleRate / 1e6
                        << " Msps, FIR2=" << taps2.size() << " taps @ " << fs1 / 1e6
                        << " Msps, PSD=" << cfg.nfft << " point / 50% overlap");
        LOG_INFO_STREAM("SDR_stream_MTU=" << streamMtu << " complex_samples ("
                        << std::fixed << std::setprecision(3)
                        << (1000.0 * double(streamMtu) / cfg.sampleRate)
                        << " ms), stream_buffers=" << cfg.streamBuffers
                        << ", direct_buffers=" << directBuffers
                        << "; actual DSP block size is the sample count returned by each SDR read");
        LOG_INFO_STREAM("RX_format=CS8 native, RX_path="
                        << (useDirect ? "acquireReadBuffer-zero-copy" : "readStream-copy-fallback"));
#if defined(__AVX2__)
        const char *simdName = "AVX2";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        const char *simdName = "NEON";
#else
        const char *simdName = "scalar/auto";
#endif
        LOG_INFO_STREAM("NCO=" << (dsp.ncoUsesLut() ? "exact-LUT" : "recursive")
                        << (dsp.ncoUsesLut() ? (" period=" + std::to_string(dsp.ncoLutPeriod())) : std::string())
                        << ", FIR1=block-polyphase SIMD=" << simdName
                        << " threads=" << dsp.fir1Threads());
        if (cfg.detectorEnabled)
        {
            LOG_INFO_STREAM("detector=ON plugin=" << cfg.detectorPlugin
                            << " threads=" << cfg.detectorThreads
                            << " freq_mean_bins=" << cfg.detectorFrequencyMeanBins
                            << " time_mean_psds=" << cfg.detectorTimeMeanPsds
                            << " peak_threshold=" << cfg.detectorPeakThresholdDb
                            << " dB-above-median"
                            << " min_peak_separation=" << cfg.detectorMinPeakSeparationHz
                            << " Hz max_peaks=" << cfg.detectorMaxPeaksPerPsd
                            << " pre_context=" << cfg.detectorPreContextSeconds
                            << " s post_context=" << cfg.detectorPostContextSeconds
                            << " s max_event=" << cfg.detectorMaxEventSeconds
                            << " s rearm=" << cfg.detectorRearmSeconds << " s");
        }
        else
            spdlog::info("detector=OFF");

        if (cfg.detectorEnabled)
        {
            LOG_INFO_STREAM("HDF5_output_directory=" << cfg.detectorOutputDirectory
                            << " prefix=" << cfg.detectorFilePrefix);
            LOG_INFO_STREAM("HDF5_SWMR=" << (cfg.outputSwmr ? "ON" : "OFF")
                            << " flush_interval=" << cfg.outputSwmrFlushSeconds << " s"
                            << " daily_rotate_utc=" << formatDailyRotateTime(cfg.outputDailyRotateMinuteUtc));
        }
        if (cfg.agcEnabled)
            LOG_INFO_STREAM("AGC=ON source=CS8_clipping target=" << cfg.agcTargetSaturationPercent
                            << "% tau=" << cfg.agcTimeConstantSeconds << " s clip_level=" << cfg.agcClipLevel
                            << " max_step=" << cfg.agcMaxStepDb << " dB gain_range=["
                            << deviceMinGain << "," << deviceMaxGain << "] dB");
        else
            spdlog::info("AGC=OFF");

        const uint64_t acquisitionStartNs = systemNowNs();
        dsp.psd().setStartTimestampNs(acquisitionStartNs);
        std::unique_ptr<PsdDetectorRecorder> detector;
        std::unique_ptr<ContinuousPsdRecorder> continuousRecorder;

        RecorderMetadata md;
        md.tomlText = effectiveToml(cfg);
        md.hostname = hostnameString();
        md.driver = cfg.driver;
        md.hardwareInfo = hw.str();
        md.acquisitionStartNs = acquisitionStartNs;
        md.sampleRate = cfg.sampleRate;
        md.centerFrequency = currentFrequency;
        md.sdrBandwidth = currentBandwidth;
        md.gain = currentGain;
        md.shiftHz = cfg.shiftHz;
        md.outputRate = outputRate;
        md.psdBandwidth = cfg.bandwidth;
        md.decim1 = cfg.decim1;
        md.decim2 = cfg.decim2;
        md.nfft = cfg.nfft;

        if (cfg.recordingMode == "continuous")
        {
            continuousRecorder.reset(new ContinuousPsdRecorder(
                cfg, dsp.psd().frequencies(), md));
            dsp.psd().setFrameCallback(
                [&continuousRecorder](const PsdFrame &f) {
                    continuousRecorder->consume(f);
                });
        }
        else
        {
            detector.reset(new PsdDetectorRecorder(
                cfg, dsp.psd().frequencies(), md));
            dsp.psd().setFrameCallback(
                [&detector](const PsdFrame &f) { detector->consume(f); });
        }

        const int actRet = dev->activateStream(rxStream);
        if (actRet != 0)
            throw std::runtime_error(std::string("activateStream failed: ") + SoapySDR::errToStr(actRet));
        activated = true;
        const auto wall0 = std::chrono::steady_clock::now();

        bool shutdownWaitAnnounced = false;
        auto eventInProgress = [&detector]() -> bool {
            return detector && detector->eventInProgress();
        };
        auto gracefulStopMayExit = [&eventInProgress]() -> bool {
            return gStopRequested && (!eventInProgress() || gForceStopRequested);
        };

        while ((!finiteRun || totalInput < targetInput) &&
               !(continuousRecorder && continuousRecorder->finished()) &&
               !gracefulStopMayExit())
        {
            if (gStopRequested && eventInProgress() && !shutdownWaitAnnounced)
            {
                shutdownWaitAnnounced = true;
                LOG_INFO_STREAM("shutdown requested by signal " << int(gStopSignal)
                                << ": waiting for current event and post-trigger context; "
                                << "press Ctrl-C again to force stop");
            }
            const size_t remaining = finiteRun ? (targetInput - totalInput) : streamMtu;
            const int8_t *iq = nullptr;
            size_t got = 0;
            size_t handle = 0;
            bool heldDirect = false;

            if (useDirect)
            {
                const void *buffs[1] = {nullptr};
                int flags = 0;
                long long timeNs = 0;
                const int ret = dev->acquireReadBuffer(
                    rxStream, handle, buffs, flags, timeNs, 250000);
                ++readCalls;
                if (ret > 0)
                {
                    iq = static_cast<const int8_t *>(buffs[0]);
                    got = std::min<size_t>(static_cast<size_t>(ret), remaining);
                    heldDirect = true;
                }
                else if (gracefulStopMayExit())
                {
                    break;
                }
                else if (ret == SOAPY_SDR_OVERFLOW)
                {
                    ++overflows;
                    LOG_ERROR_STREAM("SDR overflow on acquireReadBuffer; total_overflows="
                                     << overflows << "; continuing acquisition");
                    continue;
                }
                else if (ret == SOAPY_SDR_TIMEOUT || ret == 0)
                {
                    ++timeouts;
                    continue;
                }
                else
                {
                    throw std::runtime_error(std::string("acquireReadBuffer failed: ") + SoapySDR::errToStr(ret));
                }
            }
            else
            {
                void *buffs[] = {fallbackIq.data()};
                int flags = 0;
                long long timeNs = 0;
                const size_t want = std::min(streamMtu, remaining);
                const int ret = dev->readStream(rxStream, buffs, want, flags, timeNs, 250000);
                ++readCalls;
                if (ret > 0)
                {
                    iq = fallbackIq.data();
                    got = static_cast<size_t>(ret);
                }
                else if (gracefulStopMayExit())
                {
                    break;
                }
                else if (ret == SOAPY_SDR_OVERFLOW)
                {
                    ++overflows;
                    LOG_ERROR_STREAM("SDR overflow on readStream; total_overflows="
                                     << overflows << "; continuing acquisition");
                    continue;
                }
                else if (ret == SOAPY_SDR_TIMEOUT || ret == 0)
                {
                    ++timeouts;
                    continue;
                }
                else
                {
                    throw std::runtime_error(std::string("readStream failed: ") + SoapySDR::errToStr(ret));
                }
            }

            if (!loggedFirstInputBlock)
            {
                firstInputBlockSamples = got;
                loggedFirstInputBlock = true;
                LOG_INFO_STREAM("effective_acquisition_DSP_block=" << got
                                << " complex_samples ("
                                << std::fixed << std::setprecision(3)
                                << (1000.0 * double(got) / cfg.sampleRate)
                                << " ms) on first successful SDR read"
                                << "; source="
                                << (useDirect ? "acquireReadBuffer" : "readStream"));
            }
            minInputBlockSamples = std::min(minInputBlockSamples, got);
            maxInputBlockSamples = std::max(maxInputBlockSamples, got);

            const auto d0 = std::chrono::steady_clock::now();

            // Native CS8 is consumed directly from the driver's ring buffer.
            // AGC clipping count is folded into this same pass (no extra scan).
            dsp.psd().setCurrentGainDb(currentGain);
            uint64_t clippedComplex = 0;
            const size_t produced = dsp.processCs8(iq, got, cfg.agcEnabled ? &clippedComplex : nullptr, cfg.agcClipLevel);

            const auto d1 = std::chrono::steady_clock::now();
            if (heldDirect) dev->releaseReadBuffer(rxStream, handle);

            const double td = std::chrono::duration<double>(d1 - d0).count();
            if (cfg.agcEnabled)
                currentGain = agc.update(clippedComplex, got, double(got) / cfg.sampleRate);
            dspSeconds += td;
            maxBlockDsp = std::max(maxBlockDsp, td);
            blockDspTimes.push_back(td);
            if (td > double(got) / cfg.sampleRate) ++deadlineMisses;

            totalInput += got;
            totalOutput += produced;
            ++blockCount;
        }

        const auto wall1 = std::chrono::steady_clock::now();
        const double wallElapsed = std::chrono::duration<double>(wall1 - wall0).count();

        dev->deactivateStream(rxStream);
        activated = false;
        dev->closeStream(rxStream);
        rxStream = nullptr;

        // Close/flush the recorder before printing final statistics or exiting.
        // This is especially important after Ctrl-C/SIGTERM.
        if (detector)
        {
            detector->printSummary();
            detector.reset();
        }
        if (continuousRecorder)
        {
            continuousRecorder->printSummary();
            continuousRecorder.reset();
        }

        if (gStopRequested)
        {
            LOG_INFO_STREAM("shutdown_signal=" << int(gStopSignal)
                            << (gForceStopRequested
                                    ? " (forced)"
                                    : " (graceful, current event complete)"));
        }

        const double realtimeDuration = double(totalInput) / cfg.sampleRate;
        const double dspUtil = realtimeDuration > 0.0 ? dspSeconds / realtimeDuration : 0.0;
        const double endToEndRate = wallElapsed > 0.0 ? double(totalInput) / wallElapsed : 0.0;
        const double expectedOutput = double(totalInput) / double(cfg.decim1 * cfg.decim2);

        spdlog::info("=== Result ===");
        if (blockCount != 0)
        {
            LOG_INFO_STREAM("acquisition_DSP_blocks=" << blockCount
                            << " first=" << firstInputBlockSamples
                            << " min=" << minInputBlockSamples
                            << " max=" << maxInputBlockSamples
                            << " complex_samples"
                            << " first_period_ms=" << std::fixed << std::setprecision(3)
                            << (1000.0 * double(firstInputBlockSamples) / cfg.sampleRate));
        }
        LOG_INFO_STREAM("input_samples=" << totalInput << " output_samples=" << totalOutput
                        << " expected_output~=" << std::fixed << std::setprecision(0)
                        << expectedOutput);
        LOG_INFO_STREAM(std::setprecision(6) << "stream_duration=" << realtimeDuration
                        << " s wall_elapsed=" << wallElapsed << " s");
        LOG_INFO_STREAM(std::setprecision(3) << "end_to_end_input_rate="
                        << endToEndRate / 1e6 << " Msps");
        LOG_INFO_STREAM(std::setprecision(6) << "DSP_time=" << dspSeconds
                        << " s DSP_CPU_budget=" << std::setprecision(1) << (100.0 * dspUtil) << "%");
        LOG_INFO_STREAM(std::setprecision(6) << "DSP_breakdown_wall: FIR1_NCO="
                        << dsp.fir1WallSeconds() << " s post_FIR1="
                        << dsp.postFir1WallSeconds() << " s");
        LOG_INFO_STREAM(std::setprecision(6) << "max_DSP_block_time=" << maxBlockDsp
                        << " s nominal_buffer_budget=" << double(streamMtu) / cfg.sampleRate << " s");
        if (!blockDspTimes.empty())
        {
            std::sort(blockDspTimes.begin(), blockDspTimes.end());
            const auto percentile = [&](double q) -> double
            {
                const double pos = q * double(blockDspTimes.size() - 1);
                const size_t lo = static_cast<size_t>(std::floor(pos));
                const size_t hi = static_cast<size_t>(std::ceil(pos));
                const double frac = pos - double(lo);
                return blockDspTimes[lo] + frac * (blockDspTimes[hi] - blockDspTimes[lo]);
            };
            LOG_INFO_STREAM("DSP_block_time_percentiles: p50=" << percentile(0.50)
                            << " s p95=" << percentile(0.95)
                            << " s p99=" << percentile(0.99)
                            << " s p99.9=" << percentile(0.999) << " s");
        }
        const double missPct = blockCount > 0 ? 100.0 * double(deadlineMisses) / double(blockCount) : 0.0;
        LOG_INFO_STREAM("DSP_deadline_misses=" << deadlineMisses << "/" << blockCount
                        << " (" << std::setprecision(3) << missPct << "%)");
        LOG_INFO_STREAM((useDirect ? "acquire_calls=" : "read_calls=") << readCalls
                        << " timeouts=" << timeouts << " overflows=" << overflows);
        dsp.psd().printSummary();
        if (cfg.agcEnabled)
            LOG_INFO_STREAM("AGC_final_gain=" << currentGain << " dB smoothed_saturation="
                            << agc.smoothedPercent() << "% gain_changes=" << agc.gainChanges());
        const bool sustainableStreaming = overflows == 0 && timeouts == 0 &&
                                          dspUtil < 1.0 && endToEndRate >= 0.99 * cfg.sampleRate;
        const bool hardRealtime = sustainableStreaming && deadlineMisses == 0;
        spdlog::info("sustainable_streaming={}", sustainableStreaming ? "YES" : "NO");
        spdlog::info("hard_realtime={}", hardRealtime ? "YES" : "NO");


        SoapySDR::Device::unmake(dev);
        spdlog::info("stopped: sustainable_streaming={}", sustainableStreaming ? "YES" : "NO");
        spdlog::shutdown();
        return sustainableStreaming ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch (const std::exception &ex)
    {
        if (dev != nullptr)
        {
            if (rxStream != nullptr)
            {
                if (activated)
                {
                    try { dev->deactivateStream(rxStream); } catch (...) {}
                }
                try { dev->closeStream(rxStream); } catch (...) {}
            }
            SoapySDR::Device::unmake(dev);
        }
        if (loggingConfigured)
        {
            spdlog::error("{}", ex.what());
            spdlog::shutdown();
        }
        else
        {
            std::cerr << "ERROR: " << ex.what() << std::endl;
        }
        return EXIT_FAILURE;
    }
}
