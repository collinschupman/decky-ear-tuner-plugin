#pragma once

#include <cassert>
#include <cmath>

/**
 * Stateful first-order low-pass filter.
 *
 * The output follows the input according to a one-pole exponential smoother.
 * The coefficient is calculated from the cutoff and sample rate when the
 * sample rate is set.
 */
class OnePoleLowPass {
   public:
    /**
     * Creates a filter with a fixed cutoff frequency.
     *
     * @param cutoffHz Cutoff frequency in Hz.
     */
    explicit OnePoleLowPass(float cutoffHz) noexcept
        : mCutoffHz(cutoffHz)
    {
    }

    /**
     * Calculates the filter coefficient for the given sample rate.
     *
     * @param sampleRate Audio sample rate in samples per second; must be
     *                   positive and finite.
     */
    void SetSampleRate(float sampleRate) noexcept
    {
        assert(sampleRate > 0.0f && std::isfinite(sampleRate));
        mCoefficient = 1.0f - std::exp(-2.0f * kPi * mCutoffHz / sampleRate);
    }

    /**
     * Filters one sample and updates the filter's internal state.
     *
     * @param input Input sample.
     * @return Low-pass filtered sample.
     */
    float Process(float input) noexcept
    {
        mState += (input - mState) * mCoefficient;
        return mState;
    }

   private:
    static constexpr float kPi = 3.14159265358979323846f;

    float mCutoffHz;
    float mCoefficient = 0.0f;
    float mState = 0.0f;
};