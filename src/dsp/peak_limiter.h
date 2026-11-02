#pragma once

#include <algorithm>
#include <cmath>

#if defined(__GNUC__) || defined(__clang__)
/**
 * Stateful peak limiter that caps finite samples at a fixed ceiling.
 *
 * Gain reduction is applied immediately; gain then recovers toward unity using
 * a fixed release time. Non-finite input samples are replaced with silence.
 */
class PeakLimiter {
   public:
    /**
     * Sets the sample rate used to calculate the gain-release step.
     *
     * Invalid sample rates are ignored, leaving the previous release rate.
     *
     * @param sampleRate Audio sample rate in samples per second.
     */
    void SetSampleRate(float sampleRate) noexcept
    {
        if (sampleRate <= 0.0f || !std::isfinite(sampleRate)) {
            return;
        }
        mReleaseRate = 1.0f / (sampleRate * kReleaseSeconds);
    }

    /**
     * Limits one input sample and updates the limiter's gain state.
     *
     * @param input Input sample; NaN and infinity are treated as zero.
     * @return Output sample whose magnitude does not exceed Ceiling().
     */
    float Process(float input) noexcept
    {
        input = std::isfinite(input) ? input : 0.0f;
        const float magnitude = std::abs(input);
        const float targetGain = kCeiling / std::max(magnitude, kCeiling);
        const float releasedGain = mGain + (targetGain - mGain) * mReleaseRate;
        mGain = std::min(targetGain, releasedGain);

        return std::clamp(input * mGain, -kCeiling, kCeiling);
    }

    /** Returns the maximum output magnitude as a linear amplitude. */
    static constexpr float Ceiling() { return kCeiling; }

   private:
    static constexpr float kCeiling = 0.89125094f;
    static constexpr float kReleaseSeconds = 0.1f;

    float mGain = 1.0f;
    float mReleaseRate = 0.0f;
};
#endif