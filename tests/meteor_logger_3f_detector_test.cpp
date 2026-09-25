#include "detector/detector.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using meteoris::detector::Config;
using meteoris::detector::Environment;
using meteoris::detector::Frame;
using meteoris::detector::IDetector;
using meteoris::detector::ProcessOptions;
using meteoris::detector::Selection;
using meteoris::detector::State;

namespace {

Environment environment()
{
    Environment e;
    e.bins = 17;
    e.frequencyStartHz = -8.0;
    e.frequencyStepHz = 1.0;
    e.framePeriodSeconds = 0.1;
    return e;
}

Frame frame(const std::vector<float> &psd, const uint64_t index)
{
    Frame f;
    f.timestampNs = index * UINT64_C(100000000);
    f.frameIndex = index;
    f.psd = psd.data();
    f.bins = psd.size();
    f.frequencyStartHz = -8.0;
    f.frequencyStepHz = 1.0;
    f.framePeriodSeconds = 0.1;
    return f;
}

std::vector<float> noise()
{
    std::vector<float> psd(17);
    for (size_t i = 0; i < psd.size(); ++i)
        psd[i] = 1.0f + 0.01f * float((i * 7) % 11);
    return psd;
}

std::vector<float> clustered(const size_t center, const float scale = 1.0f)
{
    std::vector<float> psd = noise();
    psd[center - 1] = 30.0f * scale;
    psd[center] = 50.0f * scale;
    psd[center + 1] = 40.0f * scale;
    return psd;
}

std::unique_ptr<IDetector> detector(Config cfg)
{
    cfg.set("meteor_logger.detection_min_hz", "-8");
    cfg.set("meteor_logger.detection_max_hz", "8");
    cfg.set("meteor_logger.cluster_width_hz", "2.1");
    cfg.set("meteor_logger.sequence_frames", "6");
    cfg.set("meteor_logger.allowed_gaps", "1");
    cfg.set("meteor_logger.max_drift_hz_s", "20");
    cfg.set("meteor_logger.release_s", "0.15");
    cfg.set("meteor_logger.auto_notch_enabled", "false");
    Selection selection;
    selection.plugin = "meteor_logger_3f";
    selection.config = cfg;
    return meteoris::detector::create(selection, environment());
}

void testRobustSequenceAllowsOneGap()
{
    Config cfg;
    std::unique_ptr<IDetector> d = detector(cfg);
    const std::vector<float> hit = clustered(8);
    const std::vector<float> miss = noise();
    assert(d->process(frame(hit, 1)).state == State::Idle);
    assert(d->process(frame(hit, 2)).state == State::Idle);
    assert(d->process(frame(miss, 3)).state == State::Idle);
    assert(d->process(frame(hit, 4)).state == State::Idle);
    assert(d->process(frame(hit, 5)).state == State::Idle);
    assert(d->process(frame(hit, 6)).state == State::Active);
    assert(d->process(frame(miss, 7)).state == State::Active);
    assert(d->process(frame(miss, 8)).state == State::Idle);
}

void testFlatSpectrumAndFrequencyJumpDoNotTrigger()
{
    Config cfg;
    std::unique_ptr<IDetector> d = detector(cfg);
    const std::vector<float> flat(17, 1.0f);
    for (uint64_t i = 1; i <= 8; ++i)
        assert(d->process(frame(flat, i)).state == State::Idle);

    const std::vector<float> low = clustered(3);
    const std::vector<float> high = clustered(13);
    for (uint64_t i = 9; i <= 12; ++i)
        assert(d->process(frame(low, i)).state == State::Idle);
    for (uint64_t i = 13; i <= 16; ++i)
        assert(d->process(frame(high, i)).state == State::Idle);
}

void testDetectionDoesNotDependOnAbsoluteAmplitude()
{
    Config cfg;
    std::unique_ptr<IDetector> d = detector(cfg);
    const std::vector<float> weak = clustered(8, 0.1f);
    for (uint64_t i = 1; i < 6; ++i)
        assert(d->process(frame(weak, i)).state == State::Idle);
    assert(d->process(frame(weak, 6)).state == State::Active);
}

void testPersistentLineIsAutoNotched()
{
    Config cfg;
    cfg.set("meteor_logger.detection_min_hz", "-8");
    cfg.set("meteor_logger.detection_max_hz", "8");
    cfg.set("meteor_logger.cluster_width_hz", "2.1");
    cfg.set("meteor_logger.sequence_frames", "2");
    cfg.set("meteor_logger.allowed_gaps", "0");
    cfg.set("meteor_logger.max_drift_hz_s", "20");
    cfg.set("meteor_logger.release_s", "0.05");
    cfg.set("meteor_logger.auto_notch_enabled", "true");
    cfg.set("meteor_logger.auto_notch_after_s", "0.2");
    cfg.set("meteor_logger.auto_notch_width_hz", "4");
    cfg.set("meteor_logger.max_auto_notches", "1");
    Selection selection;
    selection.plugin = "meteor_logger_3f";
    selection.config = cfg;
    std::unique_ptr<IDetector> d = meteoris::detector::create(selection, environment());
    const std::vector<float> line = clustered(8);
    d->process(frame(line, 1));
    assert(d->process(frame(line, 2)).state == State::Active);
    d->process(frame(line, 3));
    ProcessOptions debug;
    debug.collectDebug = true;
    const auto afterNotch = d->process(frame(line, 4), debug);
    assert(afterNotch.state == State::Idle);
    assert(afterNotch.retainedPeaks == 0);
    bool sawNotch = false;
    for (size_t i = 0; i < afterNotch.debug.metricCount; ++i)
        if (std::string(afterNotch.debug.metrics[i].name) == "auto_notch_count" &&
            afterNotch.debug.metrics[i].value == 1.0)
            sawNotch = true;
    assert(sawNotch);
}

} // namespace

int main()
{
    const std::vector<std::string> names = meteoris::detector::pluginNames();
    assert(std::find(names.begin(), names.end(), "meteor_logger_3f") != names.end());
    testRobustSequenceAllowsOneGap();
    testFlatSpectrumAndFrequencyJumpDoNotTrigger();
    testDetectionDoesNotDependOnAbsoluteAmplitude();
    testPersistentLineIsAutoNotched();
    return 0;
}
