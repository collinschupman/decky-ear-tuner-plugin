#pragma once

#include "ear_tuner/control_state.h"
#include "platform/posix/http/socket_api.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct PosixHttpServerState {
    std::atomic<HttpSocket::Handle> mServerFd{HttpSocket::kInvalid};
    std::atomic<bool> mStopRequested{false};
    uint16_t mPort = 19285;
    std::function<std::string()> mDiagnostics;
};

void RunHttpServer(EarTunerControlState &controlState, PosixHttpServerState &serverState);

void StopHttpServer(PosixHttpServerState &serverState);