#include "platform/posix/http/http_server.h"

#include "ear_tuner/focus_types.h"
#include "http/http_parse.h"
#include "platform/posix/http/control_token.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kClientTimeout = std::chrono::milliseconds(500);
constexpr int kStopPollMilliseconds = 50;
constexpr size_t kMaximumRequestBytes = 4096;

bool Authenticated(const std::string &request, const std::string &token)
{
    size_t position = request.find("\r\n");
    bool authorized = false;
    while (position != std::string::npos) {
        position += 2;
        const auto end = request.find("\r\n", position);
        if (end == position) {
            return authorized;
        }
        const auto separator = request.find(':', position);
        if (end == std::string::npos || separator == std::string::npos || separator >= end) {
            return false;
        }
        std::string name = request.substr(position, separator - position);
        if (name.empty() ||
            name.find_first_not_of(
                "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") !=
                std::string::npos) {
            return false;
        }
        for (auto &ch : name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (name == "origin") {
            return false;
        }
        if (name == "authorization") {
            if (authorized) {
                return false;
            }
            const auto first = request.find_first_not_of(" \t", separator + 1);
            const auto last = request.find_last_not_of(" \t", end - 1);
            if (first == std::string::npos || last == std::string::npos || first > last ||
                last - first + 1 != 7 + token.size()) {
                return false;
            }
            std::string scheme = request.substr(first, 7);
            for (auto &ch : scheme) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (scheme != "bearer ") {
                return false;
            }
            unsigned char difference = 0;
            for (size_t i = 0; i < token.size(); ++i) {
                difference |= static_cast<unsigned char>(request[first + 7 + i] ^ token[i]);
            }
            if (difference != 0) {
                return false;
            }
            authorized = true;
        }
        position = end;
    }
    return false;
}

bool IsDiagnosticsRequest(const std::string &request)
{
    constexpr char prefix[] = "GET /api/v1/diagnostics HTTP/";
    return request.compare(0, sizeof(prefix) - 1, prefix) == 0;
}

std::string DiagnosticsResponse(const std::string &body)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) +
           "\r\nX-Ear-Tuner-API-Version: 1\r\nX-Ear-Tuner-Auth: bearer\r\n\r\n" + body;
}

bool MakeNonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "native http nonblocking setup failed: " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool WaitForClient(int fd, short events, const PosixHttpServerState &state,
                   Clock::time_point deadline)
{
    while (!state.mStopRequested.load(std::memory_order_relaxed)) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            std::cerr << "native http client deadline exceeded\n";
            return false;
        }
        pollfd descriptor{fd, events, 0};
        const int result =
            poll(&descriptor, 1,
                 static_cast<int>(std::min<int64_t>(remaining.count(), kStopPollMilliseconds)));
        if (result > 0) {
            if (descriptor.revents & events) {
                return true;
            }
            std::cerr << "native http client disconnected\n";
            return false;
        }
        if (result < 0 && errno != EINTR) {
            std::cerr << "native http client poll failed: " << std::strerror(errno) << "\n";
            return false;
        }
    }
    return false;
}

bool ReadRequest(int fd, const PosixHttpServerState &state, Clock::time_point deadline,
                 std::string &request)
{
    while (request.size() < kMaximumRequestBytes) {
        if (!WaitForClient(fd, POLLIN, state, deadline)) {
            return false;
        }
        char buffer[512];
        const ssize_t received =
            recv(fd, buffer, std::min(sizeof(buffer), kMaximumRequestBytes - request.size()), 0);
        if (received > 0) {
            request.append(buffer, static_cast<size_t>(received));
            if (request.find("\r\n\r\n") != std::string::npos) {
                return true;
            }
        } else if (received == 0) {
            std::cerr << "native http incomplete request\n";
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "native http receive failed: " << std::strerror(errno) << "\n";
            return false;
        }
    }
    std::cerr << "native http request exceeds size limit\n";
    return false;
}

void SendResponse(int fd, const char *response, size_t size, const PosixHttpServerState &state,
                  Clock::time_point deadline)
{
    size_t offset = 0;
    while (offset < size && WaitForClient(fd, POLLOUT, state, deadline)) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t sent = send(fd, response + offset, size - offset, flags);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
        } else if (sent == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            std::cerr << "native http send failed: " << std::strerror(errno) << "\n";
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
    if (!ControlToken::Valid(serverState.mToken)) {
        std::cerr << "native http requires valid control credentials\n";
        return;
    }
    const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "native http socket failed: " << std::strerror(errno) << "\n";
        return;
    }
    int reuseAddress = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(serverState.mPort);

    if (!MakeNonblocking(serverFd) ||
        bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(serverFd, 5) < 0) {
        std::cerr << "native http setup failed: " << std::strerror(errno) << "\n";
        close(serverFd);
        serverState.mServerFd.store(-1, std::memory_order_relaxed);
        return;
    }
    serverState.mServerFd.store(serverFd, std::memory_order_relaxed);

    std::cout << "native http: listening on 127.0.0.1:" << serverState.mPort << std::endl;
    constexpr char kOkResponse[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "X-Ear-Tuner-API-Version: 1\r\n"
        "X-Ear-Tuner-Auth: bearer\r\n\r\nOK";
    constexpr char kBadResponse[] =
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n"
        "X-Ear-Tuner-API-Version: 1\r\n"
        "\r\nBad Request";
    constexpr char kUnauthorizedResponse[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n"
        "WWW-Authenticate: Bearer realm=\"ear-tuner\"\r\n\r\nUnauthorized";

    while (!serverState.mStopRequested.load(std::memory_order_relaxed)) {
        pollfd listener{serverFd, POLLIN, 0};
        const int ready = poll(&listener, 1, kStopPollMilliseconds);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0 || !(listener.revents & POLLIN)) {
            std::cerr << "native http listener poll failed\n";
            break;
        }
        const int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            std::cerr << "native http accept failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (!MakeNonblocking(clientFd)) {
            close(clientFd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int noSigpipe = 1;
        if (setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe)) < 0) {
            std::cerr << "native http SIGPIPE setup failed: " << std::strerror(errno) << "\n";
            close(clientFd);
            continue;
        }
#endif

        const auto deadline = Clock::now() + kClientTimeout;
        std::string request;
        bool valid = false;
        std::string dynamicResponse;
        if (ReadRequest(clientFd, serverState, deadline, request)) {
            if (!Authenticated(request, serverState.mToken)) {
                SendResponse(clientFd, kUnauthorizedResponse, sizeof(kUnauthorizedResponse) - 1,
                             serverState, deadline);
                close(clientFd);
                continue;
            }
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
        close(clientFd);
    }
    serverState.mServerFd.store(-1, std::memory_order_relaxed);
    close(serverFd);
}