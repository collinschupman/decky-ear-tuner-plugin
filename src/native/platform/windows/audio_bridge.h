#pragma once

#include "ear_tuner/audio_block_processor.h"

#include <array>
#include <atomic>
#include <cstdint>

class WindowsAudioBridge {
   public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kMaxFrames = 16384;
    static constexpr uint32_t kCapacity = kMaxFrames * 2;
    static constexpr uint32_t kTargetFrames = 960;

    EarTunerControlState mControl;
    std::atomic<uint32_t> mCapturePackets{0};
    std::atomic<uint32_t> mRenderPackets{0};
    std::atomic<uint32_t> mBufferedFrames{0};
    std::atomic<uint32_t> mUnderruns{0};
    std::atomic<uint32_t> mOverflows{0};
    std::atomic<uint32_t> mDiscontinuities{0};
    std::atomic<bool> mBufferError{false};
    std::atomic<float> mRenderRate{static_cast<float>(kSampleRate)};

    // Called only by the audio thread. A null capture buffer represents WASAPI silence.
    bool Capture(const float *interleaved, uint32_t frames) EAR_TUNER_NONBLOCKING;
    bool Render(float *interleaved, uint32_t frames) EAR_TUNER_NONBLOCKING;
    void Discontinuity() EAR_TUNER_NONBLOCKING;

   private:
    void ClearQueue() EAR_TUNER_NONBLOCKING;
    EarTunerProcessor mProcessor;
    std::array<float, kMaxFrames> mLeft{};
    std::array<float, kMaxFrames> mRight{};
    std::array<std::array<float, 2>, kCapacity> mRing{};
    uint32_t mRead = 0;
    uint32_t mQueued = 0;
    bool mPrimed = false;
};
