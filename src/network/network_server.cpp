#include "network/network_server.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t invalid_socket = -1;
#endif

namespace
{

void closeSocket(socket_t s)
{
    if (s == invalid_socket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void setNonBlocking(socket_t s)
{
#if defined(_WIN32)
    u_long one = 1;
    ::ioctlsocket(s, FIONBIO, &one);
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

void setBlocking(socket_t s)
{
#if defined(_WIN32)
    u_long zero = 0;
    ::ioctlsocket(s, FIONBIO, &zero);
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) ::fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

bool wouldBlock()
{
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

socket_t makeListener(const std::string &address, uint16_t port)
{
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == invalid_socket) throw std::runtime_error("cannot create TCP socket");

    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&one), sizeof(one));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &a.sin_addr) != 1)
    {
        closeSocket(s);
        throw std::runtime_error("invalid network.bind_address: " + address);
    }
    if (::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
    {
        closeSocket(s);
        throw std::runtime_error("cannot bind " + address + ":" + std::to_string(port));
    }
    if (::listen(s, 1) != 0)
    {
        closeSocket(s);
        throw std::runtime_error("cannot listen on " + address + ":" + std::to_string(port));
    }
    setNonBlocking(s);
    return s;
}

bool sendAll(socket_t s, const void *data, size_t bytes)
{
    const char *p = static_cast<const char *>(data);
    while (bytes != 0)
    {
#if defined(_WIN32)
        const int chunk = bytes > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(bytes);
        const int n = ::send(s, p, chunk, 0);
#else
        const ssize_t n = ::send(s, p, bytes, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<size_t>(n);
    }
    return true;
}

bool sendLine(socket_t s, const std::string &line)
{
    return sendAll(s, line.data(), line.size());
}

bool recvLine(socket_t s, std::string &buffer, std::string &line,
              const std::atomic<bool> &stop)
{
    for (;;)
    {
        const size_t nl = buffer.find('\n');
        if (nl != std::string::npos)
        {
            line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (stop.load(std::memory_order_acquire)) return false;

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
#if defined(_WIN32)
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(s + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready == 0) continue;
        if (ready < 0) return false;

        char tmp[1024];
#if defined(_WIN32)
        const int n = ::recv(s, tmp, sizeof(tmp), 0);
#else
        const ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
#endif
        if (n > 0)
        {
            buffer.append(tmp, static_cast<size_t>(n));
            if (buffer.size() > 65536) return false;
            continue;
        }
        return false;
    }
}

void setSendTimeout(socket_t s)
{
#if defined(_WIN32)
    const DWORD timeoutMs = 500;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

#pragma pack(push, 1)
struct PsdWireHeader
{
    char magic[4];            // MPSD
    uint16_t version;         // 1
    uint16_t headerBytes;
    uint32_t payloadBytes;
    uint32_t binCount;
    uint64_t frameIndex;
    uint64_t timestampNs;
    uint64_t centerSampleIndex;
    float gainDb;
    double centerFrequencyHz;
    double frequencyStartHz;
    double frequencyStepHz;
};
#pragma pack(pop)

static_assert(sizeof(PsdWireHeader) == 68, "unexpected PSD wire header layout");

} // namespace

namespace meteoris_network
{

struct Server::PendingControl
{
    ControlCommand command;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string message;
    double appliedValue = 0.0;
};

Server::Server(Config config,
               double frequencyStartHz,
               double frequencyStepHz,
               std::string effectiveToml)
    : _config(std::move(config)),
      _frequencyStartHz(frequencyStartHz),
      _frequencyStepHz(frequencyStepHz),
      _effectiveToml(std::move(effectiveToml))
{
    if (!_config.enabled) return;
    if (_config.queueFrames == 0) _config.queueFrames = 1;
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("WSAStartup failed");
#endif
    _psdThread = std::thread(&Server::psdLoop, this);
    _controlThread = std::thread(&Server::controlLoop, this);
    spdlog::info("network=ON bind={} psd_port={} control_port={} queue_frames={}",
                 _config.bindAddress, _config.psdPort, _config.controlPort,
                 _config.queueFrames);
}

Server::~Server()
{
    _stop.store(true, std::memory_order_release);
    _psdCv.notify_all();
    if (_psdThread.joinable()) _psdThread.join();
    if (_controlThread.joinable()) _controlThread.join();
#if defined(_WIN32)
    if (_config.enabled) WSACleanup();
#endif
}

void Server::setRuntimeState(double centerFrequencyHz, double gainDb)
{
    _centerFrequencyHz.store(centerFrequencyHz, std::memory_order_release);
    _gainDb.store(gainDb, std::memory_order_release);
}

void Server::publish(const PsdFramePtr &frame)
{
    if (!_config.enabled || !_psdConnected.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(_psdMutex);
        while (_psdQueue.size() >= _config.queueFrames)
        {
            _psdQueue.pop_front();
            _framesDropped.fetch_add(1, std::memory_order_relaxed);
        }
        _psdQueue.push_back(frame);
    }
    _psdCv.notify_one();
}

bool Server::tryPopControl(ControlCommand &command)
{
    std::lock_guard<std::mutex> lock(_controlMutex);
    if (_controlQueue.empty()) return false;
    command = _controlQueue.front()->command;
    _controlQueue.pop_front();
    return true;
}

void Server::completeControl(uint64_t id, bool ok, const std::string &message,
                             double appliedValue)
{
    std::shared_ptr<PendingControl> pending;
    {
        std::lock_guard<std::mutex> lock(_controlMutex);
        const auto it = _controlPending.find(id);
        if (it == _controlPending.end()) return;
        pending = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->ok = ok;
        pending->message = message;
        pending->appliedValue = appliedValue;
        pending->done = true;
    }
    pending->cv.notify_one();
}

void Server::psdLoop()
{
    socket_t listener = invalid_socket;
    socket_t client = invalid_socket;
    try
    {
        listener = makeListener(_config.bindAddress, _config.psdPort);
        while (!_stop.load(std::memory_order_acquire))
        {
            if (client == invalid_socket)
            {
                client = ::accept(listener, nullptr, nullptr);
                if (client == invalid_socket)
                {
                    if (!wouldBlock())
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                setBlocking(client);
                setSendTimeout(client);
                _psdConnected.store(true, std::memory_order_release);
                spdlog::info("meteoris_web PSD client connected");
            }

            PsdFramePtr frame;
            {
                std::unique_lock<std::mutex> lock(_psdMutex);
                _psdCv.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return _stop.load(std::memory_order_acquire) || !_psdQueue.empty();
                });
                if (_stop.load(std::memory_order_acquire)) break;
                if (_psdQueue.empty()) continue;
                frame = _psdQueue.front();
                _psdQueue.pop_front();
            }

            PsdWireHeader h{};
            std::memcpy(h.magic, "MPSD", 4);
            h.version = 1;
            h.headerBytes = static_cast<uint16_t>(sizeof(h));
            h.payloadBytes = static_cast<uint32_t>(frame->powerDensity.size() * sizeof(float));
            h.binCount = static_cast<uint32_t>(frame->powerDensity.size());
            h.frameIndex = frame->frameIndex;
            h.timestampNs = frame->timestampNs;
            h.centerSampleIndex = frame->centerSampleIndex;
            h.gainDb = frame->gainDb;
            h.centerFrequencyHz = _centerFrequencyHz.load(std::memory_order_acquire);
            h.frequencyStartHz = _frequencyStartHz;
            h.frequencyStepHz = _frequencyStepHz;

            if (!sendAll(client, &h, sizeof(h)) ||
                !sendAll(client, frame->powerDensity.data(), h.payloadBytes))
            {
                closeSocket(client);
                client = invalid_socket;
                _psdConnected.store(false, std::memory_order_release);
                std::lock_guard<std::mutex> lock(_psdMutex);
                _psdQueue.clear();
                spdlog::info("meteoris_web PSD client disconnected");
                continue;
            }
            _framesSent.fetch_add(1, std::memory_order_relaxed);
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("PSD network thread: {}", e.what());
    }
    _psdConnected.store(false, std::memory_order_release);
    closeSocket(client);
    closeSocket(listener);
}

void Server::controlLoop()
{
    socket_t listener = invalid_socket;
    socket_t client = invalid_socket;
    try
    {
        listener = makeListener(_config.bindAddress, _config.controlPort);
        while (!_stop.load(std::memory_order_acquire))
        {
            if (client == invalid_socket)
            {
                client = ::accept(listener, nullptr, nullptr);
                if (client == invalid_socket)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                setBlocking(client);
                setSendTimeout(client);
                _controlConnected.store(true, std::memory_order_release);
                spdlog::info("meteoris_web control client connected");
                if (!sendLine(client, "HELLO METEORIS 1\n"))
                {
                    closeSocket(client);
                    client = invalid_socket;
                    _controlConnected.store(false, std::memory_order_release);
                    continue;
                }
            }

            std::string input;
            std::string line;
            while (!_stop.load(std::memory_order_acquire) && recvLine(client, input, line, _stop))
            {
                if (line == "PING")
                {
                    if (!sendLine(client, "OK PONG\n")) break;
                }
                else if (line == "GET_CONFIG")
                {
                    std::ostringstream h;
                    h << "CONFIG " << _effectiveToml.size() << "\n";
                    if (!sendLine(client, h.str()) ||
                        !sendAll(client, _effectiveToml.data(), _effectiveToml.size())) break;
                }
                else if (line == "GET_STATUS")
                {
                    std::ostringstream s;
                    s << std::setprecision(15)
                      << "STATUS {\"center_frequency\":" << _centerFrequencyHz.load()
                      << ",\"gain_db\":" << _gainDb.load()
                      << ",\"psd_connected\":" << (psdConnected() ? "true" : "false")
                      << ",\"frames_sent\":" << framesSent()
                      << ",\"frames_dropped\":" << framesDropped()
                      << "}\n";
                    if (!sendLine(client, s.str())) break;
                }
                else if (line.compare(0, 4, "SET ") == 0)
                {
                    std::istringstream in(line.substr(4));
                    std::string parameter;
                    double value = 0.0;
                    if (!(in >> parameter >> value))
                    {
                        if (!sendLine(client, "ERR invalid SET syntax\n")) break;
                        continue;
                    }
                    if (parameter != "sdr.center_frequency" && parameter != "sdr.gain")
                    {
                        if (!sendLine(client, "ERR restart_required_or_read_only\n")) break;
                        continue;
                    }

                    auto pending = std::make_shared<PendingControl>();
                    {
                        std::lock_guard<std::mutex> lock(_controlMutex);
                        pending->command.id = _nextControlId++;
                        pending->command.parameter = parameter;
                        pending->command.value = value;
                        _controlQueue.push_back(pending);
                        _controlPending[pending->command.id] = pending;
                    }

                    std::unique_lock<std::mutex> lock(pending->mutex);
                    const bool completed = pending->cv.wait_for(
                        lock, std::chrono::seconds(2), [&pending] { return pending->done; });
                    lock.unlock();
                    {
                        std::lock_guard<std::mutex> ctl(_controlMutex);
                        _controlPending.erase(pending->command.id);
                    }
                    if (!completed)
                    {
                        if (!sendLine(client, "ERR control_timeout\n")) break;
                    }
                    else if (pending->ok)
                    {
                        std::ostringstream reply;
                        reply << std::setprecision(15) << "OK " << parameter << ' '
                              << pending->appliedValue << "\n";
                        if (!sendLine(client, reply.str())) break;
                    }
                    else
                    {
                        if (!sendLine(client, "ERR " + pending->message + "\n")) break;
                    }
                }
                else
                {
                    if (!sendLine(client, "ERR unknown_command\n")) break;
                }
            }

            closeSocket(client);
            client = invalid_socket;
            _controlConnected.store(false, std::memory_order_release);
            spdlog::info("meteoris_web control client disconnected");
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("control network thread: {}", e.what());
    }
    _controlConnected.store(false, std::memory_order_release);
    closeSocket(client);
    closeSocket(listener);
}

} // namespace meteoris_network
