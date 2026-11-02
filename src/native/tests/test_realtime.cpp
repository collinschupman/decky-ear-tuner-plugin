#include "ear_tuner/audio_block_processor.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

void *volatile allocated = nullptr;

void AllocateInCallback() EAR_TUNER_NONBLOCKING { allocated = std::malloc(64); }

bool CheckProcessing()
{
    constexpr uint32_t capacity = 16384;
    std::array<float, capacity> left{};
    std::array<float, capacity> right{};
    std::array<float, capacity> outputLeft{};
    std::array<float, capacity> outputRight{};
    for (uint32_t sample = 0; sample < capacity; ++sample) {
        left[sample] = 0.5f * std::sin(sample * 0.07f);
        right[sample] = 0.01f * std::cos(sample * 0.13f);
    }
    EarTunerControlState control;
    EarTunerProcessor processor;
    for (uint8_t leftMode = 0; leftMode <= 4; ++leftMode) {
        for (uint8_t rightMode = 0; rightMode <= 4; ++rightMode) {
            for (uint8_t leftLevel = 0; leftLevel <= 3; ++leftLevel) {
                for (uint8_t rightLevel = 0; rightLevel <= 3; ++rightLevel) {
                    control.mLeftFocusMode.store(leftMode);
                    control.mRightFocusMode.store(rightMode);
                    control.mLeftFocusLevel.store(leftLevel);
                    control.mRightFocusLevel.store(rightLevel);
                    for (bool enabled : {false, true}) {
                        for (bool bypassed : {false, true}) {
                            control.mFocusEnabled.store(enabled);
                            control.mFocusBypassed.store(bypassed);
                            for (uint32_t band = 0; band < 4; ++band) {
                                control.mLowGainsDb[band].store(leftLevel * 2.0f + band);
                                control.mVoiceGainsDb[band].store(rightLevel * 2.0f - band);
                                control.mHighGainsDb[band].store(band - leftLevel * 1.0f);
                            }
                            control.mQuietMaxGainDb.store(rightLevel * 3.0f);
                            control.mCompressionThreshold.store(0.01f + rightLevel * 0.1f);
                            control.mOutputGain.store(enabled ? 1.5f : 0.5f);
                            for (float rate : {44100.0f, 48000.0f, 96000.0f}) {
                                for (uint32_t count : {0u, 1u, 64u, 257u, 1024u}) {
                                    ProcessAudioBlock(control, processor, left.data(), right.data(),
                                                      outputLeft.data(), outputRight.data(), count,
                                                      rate);
                                    for (uint32_t sample = 0; sample < count; ++sample) {
                                        if (!std::isfinite(outputLeft[sample]) ||
                                            !std::isfinite(outputRight[sample])) {
                                            return false;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    left[0] = std::numeric_limits<float>::quiet_NaN();
    right[0] = std::numeric_limits<float>::infinity();
    control.mFocusEnabled.store(true);
    control.mFocusBypassed.store(false);
    ProcessAudioBlock(control, processor, left.data(), right.data(), outputLeft.data(),
                      outputRight.data(), capacity, 48000.0f);
    ProcessAudioBlock(control, processor, nullptr, right.data(), outputLeft.data(),
                      outputRight.data(), capacity, 48000.0f);
    ProcessAudioBlock(control, processor, left.data(), nullptr, outputLeft.data(),
                      outputRight.data(), capacity, 48000.0f);
    ProcessAudioBlock(control, processor, nullptr, nullptr, outputLeft.data(), outputRight.data(),
                      capacity, 48000.0f);
    ProcessAudioBlock(control, processor, left.data(), right.data(), nullptr, outputRight.data(),
                      capacity, 48000.0f);
    ProcessAudioBlock(control, processor, left.data(), right.data(), outputLeft.data(), nullptr,
                      capacity, 48000.0f);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--allocate") == 0) {
        AllocateInCallback();
        std::free(allocated);
        std::cerr << "RTSan did not reject the deliberate allocation\n";
        return 1;
    }
    if (!CheckProcessing()) {
        std::cerr << "Audio processing produced non-finite output\n";
        return 1;
    }
    std::cout << "Real-time processing checks passed\n";
    return 0;
}
