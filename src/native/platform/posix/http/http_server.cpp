#include "platform/posix/http/http_server.h"

#include "ear_tuner/focus_types.h"
#include "http/http_parse.h"

#include <cerrno>
#include <cstring>
#include <iostream>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kClientTimeout = std::chrono::milliseconds(500);
constexpr int kStopPollMilliseconds = 50;
constexpr size_t kMaximumRequestBytes = 4096;

bool IsDiagnosticsRequest(const std::string &request)
{
    constexpr char prefix[] = "GET /api/v1/diagnostics HTTP/";
    return request.compare(0, sizeof(prefix) - 1, prefix) == 0;
}

std::string DiagnosticsResponse(const std::string &body)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) +
           "\r\nX-Ear-Tuner-API-Version: 1\r\nAccess-Control-Allow-Origin: *\r\n\r\n" + body;
}

bool MakeNonblocking(HttpSocket::Handle fd)
{
    if (!HttpSocket::Nonblocking(fd)) {
        std::cerr << "native http nonblocking setup failed: " << HttpSocket::ErrorMessage() << "\n";
        return false;
    }
    return true;
}

bool WaitForClient(HttpSocket::Handle fd, short events, const PosixHttpServerState &state,
                   Clock::time_point deadline)
{
    while (!state.mStopRequested.load(std::memory_order_relaxed)) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            std::cerr << "native http client deadline exceeded\n";
            return false;
        }
        HttpSocket::PollDescriptor descriptor{fd, events, 0};
        const int result = HttpSocket::Poll(
            &descriptor,
            static_cast<int>(std::min<int64_t>(remaining.count(), kStopPollMilliseconds)));
        if (result > 0) {
            if (descriptor.revents & events) {
                return true;
            }
            std::cerr << "native http client disconnected\n";
            return false;
        }
        if (result < 0 && !HttpSocket::Retryable()) {
            std::cerr << "native http client poll failed: " << HttpSocket::ErrorMessage() << "\n";
            return false;
        }
    }
    return false;
}

bool ReadRequest(HttpSocket::Handle fd, const PosixHttpServerState &state,
                 Clock::time_point deadline, std::string &request)
{
    while (request.size() < kMaximumRequestBytes) {
        if (!WaitForClient(fd, POLLIN, state, deadline)) {
            return false;
        }
        char buffer[512];
        const auto received = recv(
            fd, buffer,
            static_cast<int>(std::min(sizeof(buffer), kMaximumRequestBytes - request.size())), 0);
        if (received > 0) {
            request.append(buffer, static_cast<size_t>(received));
            if (request.find("\r\n\r\n") != std::string::npos) {
                return true;
            }
        } else if (received == 0) {
            std::cerr << "native http incomplete request\n";
            return false;
        } else if (!HttpSocket::Retryable()) {
            std::cerr << "native http receive failed: " << HttpSocket::ErrorMessage() << "\n";
            return false;
        }
    }
    std::cerr << "native http request exceeds size limit\n";
    return false;
}

void SendResponse(HttpSocket::Handle fd, const char *response, size_t size,
                  const PosixHttpServerState &state, Clock::time_point deadline)
{
    size_t offset = 0;
    while (offset < size && WaitForClient(fd, POLLOUT, state, deadline)) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto sent =
            send(fd, response + offset,
                 static_cast<int>(
                     std::min(size - offset, static_cast<size_t>(std::numeric_limits<int>::max()))),
                 flags);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
        } else if (sent == 0 || !HttpSocket::Retryable()) {
            std::cerr << "native http send failed: " << HttpSocket::ErrorMessage() << "\n";
            return;
        }
    }
}

}  // namespace

void StopHttpServer(PosixHttpServerState &serverState)
{
    serverState.mStopRequested.store(true, std::memory_order_relaxed);
}

void RunHttpServer(EarTunerControlState &controlState, PosixHttpServerState &serverState)
{
    if (serverState.mStopRequested.load(std::memory_order_relaxed)) {
        return;
    }
    HttpSocket::Runtime runtime;
    if (runtime.Error() != 0) {
        std::cerr << "native http initialization failed: " << runtime.Error() << "\n";
        return;
    }
    const auto serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd == HttpSocket::kInvalid) {
        std::cerr << "native http socket failed: " << HttpSocket::ErrorMessage() << "\n";
        return;
    }
    int reuseAddress = 1;
#ifdef _WIN32
    constexpr int reuseOption = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int reuseOption = SO_REUSEADDR;
#endif
    if (setsockopt(serverFd, SOL_SOCKET, reuseOption, reinterpret_cast<const char *>(&reuseAddress),
                   sizeof(reuseAddress)) != 0) {
        std::cerr << "native http address option failed: " << HttpSocket::ErrorMessage() << "\n";
        HttpSocket::Close(serverFd);
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(serverState.mPort);

    if (!MakeNonblocking(serverFd) ||
        bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(serverFd, 5) < 0) {
        std::cerr << "native http setup failed: " << HttpSocket::ErrorMessage() << "\n";
        HttpSocket::Close(serverFd);
        serverState.mServerFd.store(HttpSocket::kInvalid, std::memory_order_relaxed);
        return;
    }
    serverState.mServerFd.store(serverFd, std::memory_order_relaxed);

    std::cout << "native http: listening on 127.0.0.1:" << serverState.mPort << std::endl;
    constexpr char kOkResponse[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "X-Ear-Tuner-API-Version: 1\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\nOK";
    constexpr char kBadResponse[] =
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n"
        "X-Ear-Tuner-API-Version: 1\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\nBad Request";

    while (!serverState.mStopRequested.load(std::memory_order_relaxed)) {
        HttpSocket::PollDescriptor listener{serverFd, POLLIN, 0};
        const int ready = HttpSocket::Poll(&listener, kStopPollMilliseconds);
        if (ready == 0 || (ready < 0 && HttpSocket::Retryable())) {
            continue;
        }
        if (ready < 0 || !(listener.revents & POLLIN)) {
            std::cerr << "native http listener poll failed\n";
            break;
        }
        const auto clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd == HttpSocket::kInvalid) {
            if (HttpSocket::Retryable()) {
                continue;
            }
            std::cerr << "native http accept failed: " << HttpSocket::ErrorMessage() << "\n";
            break;
        }
        if (!MakeNonblocking(clientFd)) {
            HttpSocket::Close(clientFd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int noSigpipe = 1;
        if (setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe)) < 0) {
            std::cerr << "native http SIGPIPE setup failed: " << std::strerror(errno) << "\n";
            HttpSocket::Close(clientFd);
            continue;
        }
#endif

        const auto deadline = Clock::now() + kClientTimeout;
        std::string request;
        bool valid = false;
        std::string dynamicResponse;
        if (ReadRequest(clientFd, serverState, deadline, request)) {
            std::string query;
            if (IsDiagnosticsRequest(request) && serverState.mDiagnostics) {
                dynamicResponse = DiagnosticsResponse(serverState.mDiagnostics());
                valid = true;
            } else if (ParseControlQuery(request, query)) {
                double leftDb = 0.0;
                double rightDb = 0.0;
                double focusEnabled = 0.0;
                double focusBypass = 0.0;
                double outputGain = 0.0;
                double leftMode = 0.0;
                double rightMode = 0.0;
                double leftLevel = 0.0;
                double rightLevel = 0.0;
                const bool hasGains =
                    ParseParam(query, "left", leftDb) && ParseParam(query, "right", rightDb);
                const bool hasFocusEnabled = ParseParam(query, "focus_enabled", focusEnabled);
                const bool hasFocusBypass = ParseParam(query, "focus_bypass", focusBypass);
                const bool hasOutputGain = ParseParam(query, "output_gain", outputGain);
                const bool hasFocus = ParseFocus(query, "left", leftMode, leftLevel) &&
                                      ParseFocus(query, "right", rightMode, rightLevel);
                const bool hasTuning = UpdateDspTuning(controlState, query);
                valid = hasGains || hasFocusEnabled || hasFocusBypass || hasOutputGain ||
                        hasFocus || hasTuning;
                if (hasFocusEnabled) {
                    const bool enabled = focusEnabled != 0.0;
                    controlState.mFocusEnabled.store(enabled, std::memory_order_relaxed);
                    if (!enabled) {
                        controlState.mFocusBypassed.store(false, std::memory_order_relaxed);
                    }
                }
                if (hasFocusBypass) {
                    controlState.mFocusBypassed.store(
                        ActiveWithEarTuner(
                            controlState.mFocusEnabled.load(std::memory_order_relaxed),
                            focusBypass != 0.0),
                        std::memory_order_relaxed);
                }
                if (hasOutputGain) {
                    controlState.mOutputGain.store(
                        std::clamp(static_cast<float>(outputGain), 0.0f, 1.5f),
                        std::memory_order_relaxed);
                }
                if (hasFocus) {
                    controlState.mLeftFocusMode.store(
                        static_cast<uint8_t>(std::clamp(leftMode, 0.0, 4.0)),
                        std::memory_order_relaxed);
                    controlState.mRightFocusMode.store(
                        static_cast<uint8_t>(std::clamp(rightMode, 0.0, 4.0)),
                        std::memory_order_relaxed);
                    controlState.mLeftFocusLevel.store(
                        static_cast<uint8_t>(std::clamp(leftLevel, 1.0, 3.0)),
                        std::memory_order_relaxed);
                    controlState.mRightFocusLevel.store(
                        static_cast<uint8_t>(std::clamp(rightLevel, 1.0, 3.0)),
                        std::memory_order_relaxed);
                }
            }
        }

        const char *response =
            dynamicResponse.empty() ? (valid ? kOkResponse : kBadResponse) : dynamicResponse.data();
        const size_t responseSize =
            dynamicResponse.empty() ? (valid ? sizeof(kOkResponse) - 1 : sizeof(kBadResponse) - 1)
                                    : dynamicResponse.size();
        SendResponse(clientFd, response, responseSize, serverState, deadline);
        HttpSocket::Close(clientFd);
    }
    serverState.mServerFd.store(HttpSocket::kInvalid, std::memory_order_relaxed);
    HttpSocket::Close(serverFd);
}