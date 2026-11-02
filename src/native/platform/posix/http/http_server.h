#pragma once

#include "ear_tuner/control_state.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct PosixHttpServerState {
    std::atomic<int> mServerFd{-1};
    std::atomic<bool> mStopRequested{false};
    uint16_t mPort = 19285;
    std::function<std::string()> mDiagnostics;
};

void RunHttpServer(EarTunerControlState &controlState, PosixHttpServerState &serverState);

void StopHttpServer(PosixHttpServerState &serverState);