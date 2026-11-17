#include "platform/windows/audio_bridge.h"

#include <algorithm>

void WindowsAudioBridge::ClearQueue() EAR_TUNER_NONBLOCKING
{
    mRead = 0;
    mQueued = 0;
    mRenderRate.store(static_cast<float>(kSampleRate), std::memory_order_relaxed);
    mPrimed = false;
    mBufferedFrames.store(0, std::memory_order_relaxed);
}

void WindowsAudioBridge::Discontinuity() EAR_TUNER_NONBLOCKING
{
    ClearQueue();
    mDiscontinuities.fetch_add(1, std::memory_order_relaxed);
}

bool WindowsAudioBridge::Capture(const float *input, uint32_t frames) EAR_TUNER_NONBLOCKING
{
    if (frames == 0 || frames > kMaxFrames || mBufferError.load(std::memory_order_relaxed)) {
        mBufferError.store(true, std::memory_order_relaxed);
        return false;
    }
    if (mQueued + frames > kCapacity) {
        ClearQueue();
        mOverflows.fetch_add(1, std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < frames; ++i) {
        mLeft[i] = input ? input[i * 2] : 0.0f;
        mRight[i] = input ? input[i * 2 + 1] : 0.0f;
    }
    ProcessAudioBlock(mControl, mProcessor, mLeft.data(), mRight.data(), mLeft.data(),
                      mRight.data(), frames, static_cast<float>(kSampleRate));
    for (uint32_t i = 0; i < frames; ++i) {
        mRing[(mRead + mQueued + i) % kCapacity] = {mLeft[i], mRight[i]};
    }
    mQueued += frames;
    mBufferedFrames.store(mQueued, std::memory_order_relaxed);
    mCapturePackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool WindowsAudioBridge::Render(float *output, uint32_t frames) EAR_TUNER_NONBLOCKING
{
    if (!output || frames == 0 || frames > kMaxFrames) {
        mBufferError.store(true, std::memory_order_relaxed);
        return false;
    }
    std::fill_n(output, frames * 2, 0.0f);
    if (mBufferError.load(std::memory_order_relaxed)) {
        return false;
    }
    mRenderPackets.fetch_add(1, std::memory_order_relaxed);
    const uint32_t target = kTargetFrames + frames;
    if (!mPrimed) {
        if (mQueued < target) {
            return true;
        }
        mPrimed = true;
    }

    // The non-realtime clock controller applies this rate through WASAPI's
    // high-quality resampler; the audio thread never calls SetSampleRate.
    const double error = (static_cast<double>(mQueued) - target) / target;
    const double step = std::clamp(1.0 + 0.01 * error, 0.995, 1.005);
    mRenderRate.store(static_cast<float>(kSampleRate * step), std::memory_order_relaxed);
    if (mQueued < frames) {
        ClearQueue();
        mUnderruns.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const auto &frame = mRing[(mRead + i) % kCapacity];
        output[i * 2] = frame[0];
        output[i * 2 + 1] = frame[1];
    }
    mRead = (mRead + frames) % kCapacity;
    mQueued -= frames;
    mBufferedFrames.store(mQueued, std::memory_order_relaxed);
    return true;
}
