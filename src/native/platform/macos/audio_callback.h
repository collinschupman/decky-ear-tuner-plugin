#pragma once

#include "ear_tuner/control_state.h"
#include "ear_tuner/ear_tuner_processor.h"
#include "ear_tuner/realtime.h"

#include <CoreAudio/CoreAudio.h>

#include <array>
#include <atomic>
#include <cstdint>

struct CoreAudioState {
    static constexpr uint32_t kMaximumFrames = 16384;
    EarTunerControlState mControl;
    EarTunerProcessor mProcessor;
    double mSampleRate = 48000;
    std::array<float, kMaximumFrames> mLeft{};
    std::array<float, kMaximumFrames> mRight{};
    std::atomic<uint32_t> mCallbacks{0};
    // A fatal buffer error remains latched until the runtime is recreated.
    std::atomic<bool> mBufferError{false};
};

OSStatus ProcessCoreAudio(AudioDeviceID device, const AudioTimeStamp *now,
                          const AudioBufferList *input, const AudioTimeStamp *inputTime,
                          AudioBufferList *output, const AudioTimeStamp *outputTime,
                          void *context) EAR_TUNER_NONBLOCKING;
