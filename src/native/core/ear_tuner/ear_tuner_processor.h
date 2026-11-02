#pragma once

#include "dsp/multi_band_splitter.h"
#include "dsp/multi_band_wide_dynamic_range_compressor.h"
#include "dsp/one_pole_low_pass.h"
#include "dsp/peak_limiter.h"
#include "dsp/stereo_frame.h"
#include "dsp/wide_dynamic_range_compressor.h"
#include "ear_tuner/focus_types.h"

/**
 * Processes stereo samples with independent focus settings for each channel.
 *
 * Focus compression, smoothed output gain, and per-ear limiting run before a
 * smoothed wet/dry mix. Configure this processor and process samples on the audio
 * thread; its DSP state is mutable and is not internally synchronized.
 */
class EarTunerProcessor {
   public:
    /**
     * Configures per-channel focus processing and output gain.
     *
     * The sample rate is forwarded to the compressors and limiters whenever it
     * changes. Focus and output-gain changes are ramped during Process().
     *
     * @param leftMode Focus mode for the left channel.
     * @param leftLevel Focus strength for the left channel; levels are clamped
     *                  by FocusCompressionGains.
     * @param rightMode Focus mode for the right channel.
     * @param rightLevel Focus strength for the right channel.
     * @param sampleRate Audio sample rate in samples per second.
     * @param tuning Frequency-band gains and compression threshold.
     * @param outputGain Linear output gain, clamped to [0, 1.5].
     */
    void Configure(FocusMode leftMode, uint8_t leftLevel, FocusMode rightMode, uint8_t rightLevel,
                   float sampleRate, const DspTuning &tuning = {}, float outputGain = 1.0f)
    {
        if (sampleRate != mSampleRate) {
            mLeftCompressor.SetSampleRate(sampleRate);
            mRightCompressor.SetSampleRate(sampleRate);
            mLeftOutputLimiter.SetSampleRate(sampleRate);
            mRightOutputLimiter.SetSampleRate(sampleRate);
            mSampleRate = sampleRate;
            mTransitionRate = 1.0f / (sampleRate * kTransitionSeconds);
        }
        mTargetOutputGain = std::clamp(outputGain, 0.0f, kMaximumOutputGain);
        if (!mGainInitialized) {
            mOutputGain = mTargetOutputGain;
            mGainInitialized = true;
        }
        const bool focusEnabled = leftMode != FocusMode::Off || rightMode != FocusMode::Off;
        SetFocusEnabled(focusEnabled);
        if (focusEnabled) {
            mLeftMode = leftMode;
            mRightMode = rightMode;
            const auto leftGains = FocusCompressionGains(leftMode, leftLevel, tuning);
            const auto rightGains = FocusCompressionGains(rightMode, rightLevel, tuning);
            mLeftCompressor.SetParameters(
                leftGains, EffectiveCompressionThreshold(leftGains, tuning.mCompressionThreshold));
            mRightCompressor.SetParameters(
                rightGains,
                EffectiveCompressionThreshold(rightGains, tuning.mCompressionThreshold));
        }
    }

    /**
     * Sets the target wet/dry mix for focus processing.
     *
     * The transition is smoothed over approximately 20 ms by Process().
     *
     * @param enabled True to enable processed audio; false to select dry audio.
     */
    void SetFocusEnabled(bool enabled) { mTargetWetMix = enabled ? 1.0f : 0.0f; }

    /**
     * Processes one stereo frame in place.
     *
     * Output gain is smoothed and applied before limiting focused channels.
     * Off channels skip limiting. The result is blended with the gain-adjusted
     * original frame.
     *
     * @param input Frame to process; replaced with the output samples.
     */
    void Process(StereoFrame &input)
    {
        const StereoFrame dry = input;
        ApplyFocus(input);

        if (mOutputGain < mTargetOutputGain) {
            mOutputGain = std::min(mTargetOutputGain, mOutputGain + mTransitionRate);
        } else if (mOutputGain > mTargetOutputGain) {
            mOutputGain = std::max(mTargetOutputGain, mOutputGain - mTransitionRate);
        }
        input.SetChannels(input.Left() * mOutputGain, input.Right() * mOutputGain);
        ApplyLimiters(input);

        if (mWetMix < mTargetWetMix) {
            mWetMix = std::min(mTargetWetMix, mWetMix + mTransitionRate);
        } else if (mWetMix > mTargetWetMix) {
            mWetMix = std::max(mTargetWetMix, mWetMix - mTransitionRate);
        }
        const float dryLeft = dry.Left() * mOutputGain;
        const float dryRight = dry.Right() * mOutputGain;
        if (mWetMix < 1.0f) {
            input.SetChannels(dryLeft + (input.Left() - dryLeft) * mWetMix,
                              dryRight + (input.Right() - dryRight) * mWetMix);
        }
    }

   private:
    static constexpr float kTransitionSeconds = 0.02f;
    static constexpr float kMaximumOutputGain = 1.5f;

    /** Applies the configured compressor to each channel, preserving Off. */
    void ApplyFocus(StereoFrame &input)
    {
        const float focusedLeft = mLeftCompressor.Process(input.Left());
        const float focusedRight = mRightCompressor.Process(input.Right());
        input.SetChannels(SelectFocusOutput(mLeftMode, input.Left(), focusedLeft),
                          SelectFocusOutput(mRightMode, input.Right(), focusedRight));
    }

    /** Keeps limiters warm without altering Off channels. */
    void ApplyLimiters(StereoFrame &input)
    {
        const float limitedLeft = mLeftOutputLimiter.Process(input.Left());
        const float limitedRight = mRightOutputLimiter.Process(input.Right());
        input.SetChannels(SelectFocusOutput(mLeftMode, input.Left(), limitedLeft),
                          SelectFocusOutput(mRightMode, input.Right(), limitedRight));
    }

    FocusMode mLeftMode = FocusMode::Off;
    FocusMode mRightMode = FocusMode::Off;
    float mSampleRate = 0.0f;
    float mWetMix = 0.0f;
    float mTargetWetMix = 0.0f;
    float mTransitionRate = 0.0f;
    float mOutputGain = 1.0f;
    float mTargetOutputGain = 1.0f;
    bool mGainInitialized = false;
    FourBandWideDynamicRangeCompressor<FourBandSplitter<OnePoleLowPass>, WideDynamicRangeCompressor>
        mLeftCompressor;
    FourBandWideDynamicRangeCompressor<FourBandSplitter<OnePoleLowPass>, WideDynamicRangeCompressor>
        mRightCompressor;
    PeakLimiter mLeftOutputLimiter;
    PeakLimiter mRightOutputLimiter;
};