#pragma once

#include "ear_tuner/control_state.h"
#include "ear_tuner/ear_tuner_processor.h"
#include "ear_tuner/realtime.h"

#include <cstdint>

/**
 * Processes a block of interleaved-by-channel (planar) stereo audio.
 *
 * Each control value is loaded atomically once per block, then used to
 * configure the processor before samples are processed. A null input channel
 * is treated as silence; output channels must both be non-null.
 *
 * @param control Shared atomic controls to snapshot for this block.
 * @param processor Stateful processor owned by the audio thread.
 * @param inputLeft Left input samples, or null for silence.
 * @param inputRight Right input samples, or null for silence.
 * @param outputLeft Destination for left output samples.
 * @param outputRight Destination for right output samples.
 * @param sampleCount Number of samples in each channel.
 * @param sampleRate Audio sample rate in samples per second.
 *
 * If either output pointer is null, the function returns without processing or
 * writing output. The function is marked nonblocking when RealtimeSanitizer
 * support is enabled.
 */
inline void ProcessAudioBlock(const EarTunerControlState &control, EarTunerProcessor &processor,
                              const float *inputLeft, const float *inputRight, float *outputLeft,
                              float *outputRight, uint32_t sampleCount,
                              float sampleRate) EAR_TUNER_NONBLOCKING
{
    if (!outputLeft || !outputRight) {
        return;
    }

    const float silenceSample = 0.0f;
    const uint32_t leftInputStride = inputLeft ? 1u : 0u;
    const uint32_t rightInputStride = inputRight ? 1u : 0u;
    inputLeft = inputLeft ? inputLeft : &silenceSample;
    inputRight = inputRight ? inputRight : &silenceSample;

    const bool focusEnabled = control.mFocusEnabled.load(std::memory_order_relaxed) &&
                              !control.mFocusBypassed.load(std::memory_order_relaxed);
    const auto leftMode =
        static_cast<FocusMode>(control.mLeftFocusMode.load(std::memory_order_relaxed));
    const auto rightMode =
        static_cast<FocusMode>(control.mRightFocusMode.load(std::memory_order_relaxed));
    const uint8_t leftLevel = control.mLeftFocusLevel.load(std::memory_order_relaxed);
    const uint8_t rightLevel = control.mRightFocusLevel.load(std::memory_order_relaxed);
    const float outputGain = control.mOutputGain.load(std::memory_order_relaxed);
    const DspTuning tuning = SnapshotDspTuning(control);
    processor.Configure(leftMode, leftLevel, rightMode, rightLevel, sampleRate, tuning, outputGain);
    processor.SetFocusEnabled(focusEnabled &&
                              (leftMode != FocusMode::Off || rightMode != FocusMode::Off));

    for (uint32_t sample = 0; sample < sampleCount; ++sample) {
        StereoFrame frame = StereoFrame::FromChannels(*inputLeft, *inputRight);
        processor.Process(frame);
        outputLeft[sample] = frame.Left();
        outputRight[sample] = frame.Right();
        inputLeft += leftInputStride;
        inputRight += rightInputStride;
    }
}