#include "platform/macos/audio_callback.h"

#include "ear_tuner/audio_block_processor.h"

#include <cstring>

namespace {

struct Channel {
    float *mSamples = nullptr;
    uint32_t mStride = 0;
    uint32_t mFrames = 0;
};

Channel FindChannel(const AudioBufferList *buffers, uint32_t channel)
{
    if (!buffers) {
        return {};
    }
    for (uint32_t index = 0; index < buffers->mNumberBuffers; ++index) {
        const auto &buffer = buffers->mBuffers[index];
        if (channel < buffer.mNumberChannels) {
            if (!buffer.mData || buffer.mDataByteSize % sizeof(float) != 0) {
                return {};
            }
            const auto samples = buffer.mDataByteSize / sizeof(float);
            if (samples % buffer.mNumberChannels != 0 || samples < buffer.mNumberChannels) {
                return {};
            }
            return {static_cast<float *>(buffer.mData) + channel, buffer.mNumberChannels,
                    static_cast<uint32_t>(samples / buffer.mNumberChannels)};
        }
        channel -= buffer.mNumberChannels;
    }
    return {};
}

}  // namespace

OSStatus ProcessCoreAudio(AudioDeviceID, const AudioTimeStamp *, const AudioBufferList *input,
                          const AudioTimeStamp *, AudioBufferList *output, const AudioTimeStamp *,
                          void *context)
{
    auto &state = *static_cast<CoreAudioState *>(context);
    // Never feed processed audio back into BlackHole's output channels.
    if (output) {
        for (uint32_t index = 0; index < output->mNumberBuffers; ++index) {
            const auto &buffer = output->mBuffers[index];
            if (buffer.mData) {
                std::memset(buffer.mData, 0, buffer.mDataByteSize);
            }
        }
        if (state.mBufferError.load(std::memory_order_relaxed)) {
            return noErr;
        }
    }
    const auto leftIn = FindChannel(input, 0);
    const auto rightIn = FindChannel(input, 1);
    const auto leftOut = FindChannel(output, state.mOutputChannelOffset);
    const auto rightOut = FindChannel(output, state.mOutputChannelOffset + 1);
    const auto frames = leftOut.mFrames;
    if (!leftIn.mSamples || !rightIn.mSamples || !leftOut.mSamples || !rightOut.mSamples ||
        !frames || frames > CoreAudioState::kMaximumFrames || rightOut.mFrames != frames ||
        leftIn.mFrames != frames || rightIn.mFrames != frames) {
        state.mBufferError.store(true, std::memory_order_relaxed);
        return noErr;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        state.mLeft[frame] = leftIn.mSamples[frame * leftIn.mStride];
        state.mRight[frame] = rightIn.mSamples[frame * rightIn.mStride];
    }
    ProcessAudioBlock(state.mControl, state.mProcessor, state.mLeft.data(), state.mRight.data(),
                      state.mLeft.data(), state.mRight.data(), frames,
                      static_cast<float>(state.mSampleRate));
    for (uint32_t frame = 0; frame < frames; ++frame) {
        leftOut.mSamples[frame * leftOut.mStride] = state.mLeft[frame];
        rightOut.mSamples[frame * rightOut.mStride] = state.mRight[frame];
    }
    state.mCallbacks.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}
