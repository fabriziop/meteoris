#pragma once

#include "psd_frame.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace meteoris_network
{

struct Config
{
    bool enabled = true;
    std::string bindAddress = "127.0.0.1";
    uint16_t psdPort = 5510;
    uint16_t controlPort = 5511;
    size_t queueFrames = 16;
};

struct ControlCommand
{
    uint64_t id = 0;
    std::string parameter;
    double value = 0.0;
};

class Server
{
public:
    Server(Config config,
           double frequencyStartHz,
           double frequencyStepHz,
           std::string effectiveToml);
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    // Best-effort network consumer. The shared frame is retained, never copied.
    // When meteoris_web is not connected this is a no-op.
    void publish(const PsdFramePtr &frame);

    bool tryPopControl(ControlCommand &command);
    void completeControl(uint64_t id, bool ok, const std::string &message,
                         double appliedValue);

    void setRuntimeState(double centerFrequencyHz, double gainDb);
    bool psdConnected() const { return _psdConnected.load(std::memory_order_acquire); }
    bool controlConnected() const { return _controlConnected.load(std::memory_order_acquire); }
    bool hasClient() const { return psdConnected() || controlConnected(); }
    uint64_t framesSent() const { return _framesSent.load(std::memory_order_relaxed); }
    uint64_t framesDropped() const { return _framesDropped.load(std::memory_order_relaxed); }

private:
    struct PendingControl;
    void psdLoop();
    void controlLoop();

    Config _config;
    double _frequencyStartHz;
    double _frequencyStepHz;
    std::string _effectiveToml;

    std::atomic<bool> _stop{false};
    std::atomic<bool> _psdConnected{false};
    std::atomic<bool> _controlConnected{false};
    std::atomic<uint64_t> _framesSent{0};
    std::atomic<uint64_t> _framesDropped{0};
    std::atomic<double> _centerFrequencyHz{0.0};
    std::atomic<double> _gainDb{0.0};

    std::mutex _psdMutex;
    std::condition_variable _psdCv;
    std::deque<PsdFramePtr> _psdQueue;

    std::mutex _controlMutex;
    std::deque<std::shared_ptr<PendingControl>> _controlQueue;
    std::map<uint64_t, std::shared_ptr<PendingControl>> _controlPending;
    uint64_t _nextControlId = 1;

    std::thread _psdThread;
    std::thread _controlThread;
};

} // namespace meteoris_network
