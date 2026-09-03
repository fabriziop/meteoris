#include "detector/detector.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

using meteoris::detector::Config;
using meteoris::detector::Environment;
using meteoris::detector::Frame;
using meteoris::detector::IDetector;
using meteoris::detector::ProcessOptions;
using meteoris::detector::ProcessingMode;
using meteoris::detector::Selection;
using meteoris::detector::State;

namespace {

Frame frame(const std::vector<float> &psd, const uint64_t index)
{
    Frame f;
    f.timestampNs = index * UINT64_C(100000000);
    f.frameIndex = index;
    f.psd = psd.data();
    f.bins = psd.size();
    f.frequencyStartHz = -4.0;
    f.frequencyStepHz = 1.0;
    f.framePeriodSeconds = 0.1;
    return f;
}

std::unique_ptr<IDetector> detector(Config cfg)
{
    Environment environment;
    environment.bins = 8;
    environment.frequencyStartHz = -4.0;
    environment.frequencyStepHz = 1.0;
    environment.framePeriodSeconds = 0.1;
    Selection selection;
    selection.plugin = "echoes_automatic";
    selection.config = cfg;
    return meteoris::detector::create(selection, environment);
}

void testAbsoluteHysteresisAndJoin()
{
    Config cfg;
    cfg.set("echoes.threshold_mode", "\"absolute\"");
    cfg.set("echoes.absolute_lower_db_hz", "5.0");
    cfg.set("echoes.absolute_upper_db_hz", "10.0");
    cfg.set("echoes.join_events_closer_than_s", "0.2");
    std::unique_ptr<IDetector> d = detector(cfg);
    const std::vector<float> quiet(8, 1.0f);
    std::vector<float> peak(8, 1.0f);
    peak[4] = 100.0f;

    assert(d->process(frame(quiet, 1)).state == State::Idle);
    assert(d->process(frame(peak, 2)).state == State::Active);
    assert(d->process(frame(quiet, 3)).state == State::Active);
    assert(d->process(frame(quiet, 4)).state == State::Active);
    assert(d->process(frame(quiet, 5)).state == State::Idle);
}

void testDifferentialAndInterval()
{
    Config cfg;
    cfg.set("echoes.threshold_mode", "\"differential\"");
    cfg.set("echoes.differential_lower_db", "2.0");
    cfg.set("echoes.differential_upper_db", "4.0");
    cfg.set("echoes.detection_center_hz", "0.0");
    cfg.set("echoes.detection_width_hz", "2.0");
    cfg.set("echoes.join_events_closer_than_s", "0.0");
    std::unique_ptr<IDetector> d = detector(cfg);
    std::vector<float> outsidePeak(8, 1.0f);
    outsidePeak[0] = 1000.0f;
    std::vector<float> insidePeak(8, 1.0f);
    insidePeak[4] = 100.0f;

    assert(d->process(frame(outsidePeak, 1)).state == State::Idle);
    assert(d->process(frame(insidePeak, 2)).state == State::Active);
}

void testAutomaticWarmupDelayAndDebugSampling()
{
    Config cfg;
    cfg.set("echoes.threshold_mode", "\"automatic\"");
    cfg.set("echoes.automatic_warmup_s", "0.2");
    cfg.set("echoes.automatic_lower_offset_db", "2.0");
    cfg.set("echoes.automatic_upper_delta_db", "2.0");
    cfg.set("echoes.automatic_stddev_window_s", "0.1");
    cfg.set("echoes.delay_before_trigger_s", "0.2");
    cfg.set("echoes.join_events_closer_than_s", "0.0");
    std::unique_ptr<IDetector> d = detector(cfg);
    const std::vector<float> quiet(8, 1.0f);
    std::vector<float> peak(8, 1.0f);
    peak[4] = 100.0f;

    assert(d->process(frame(quiet, 1)).state == State::WarmingUp);
    assert(d->process(frame(quiet, 2)).state == State::WarmingUp);
    assert(d->process(frame(quiet, 3)).state == State::Idle);

    const auto pending = d->process(frame(peak, 4));
    assert(pending.state == State::Idle);
    assert(pending.debug.metricCount == 0);
    assert(d->process(frame(quiet, 5)).state == State::Idle);

    ProcessOptions debug;
    debug.collectDebug = true;
    const auto active = d->process(frame(quiet, 6), debug);
    assert(active.state == State::Active);
    assert(active.debug.metricCount != 0);
}

void testPreprocessOnlyNeverActivates()
{
    Config cfg;
    cfg.set("echoes.threshold_mode", "\"differential\"");
    cfg.set("echoes.differential_lower_db", "2.0");
    cfg.set("echoes.differential_upper_db", "4.0");
    std::unique_ptr<IDetector> d = detector(cfg);
    std::vector<float> peak(8, 1.0f);
    peak[4] = 100.0f;
    ProcessOptions options;
    options.mode = ProcessingMode::PreprocessOnly;
    options.collectDebug = true;
    const auto result = d->process(frame(peak, 1), options);
    assert(result.state == State::Idle);
    assert(result.debug.metricCount == 0);
}

} // namespace

int main()
{
    testAbsoluteHysteresisAndJoin();
    testDifferentialAndInterval();
    testAutomaticWarmupDelayAndDebugSampling();
    testPreprocessOnlyNeverActivates();
    return 0;
}
