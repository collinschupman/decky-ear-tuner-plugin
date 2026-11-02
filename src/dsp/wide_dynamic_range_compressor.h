#pragma once

#include <algorithm>
#include <cmath>

/**
 * Stateful wide-dynamic-range compressor with level-dependent gain.
 *
 * Low-level signals approach the configured maximum gain. As the tracked
 * amplitude envelope exceeds the threshold, the gain boost is reduced, but
 * never below unity. Attack and release smoothing are applied to the envelope
 * and gain.
 */
class WideDynamicRangeCompressor {
   public:
    /**
     * Creates a compressor with configurable envelope/gain time constants.
     *
     * @param attackSeconds Attack time in seconds.
     * @param releaseSeconds Release time in seconds.
     * @param minimumThreshold Lower bound used when setting the threshold.
     */
    explicit WideDynamicRangeCompressor(float attackSeconds = 0.005f, float releaseSeconds = 0.1f,
                                        float minimumThreshold = 0.001f)
        : mAttackSeconds(attackSeconds)
        , mReleaseSeconds(releaseSeconds)
        , mMinimumThreshold(minimumThreshold)
    {
    }

    /**
     * Calculates attack and release steps for the supplied sample rate.
     *
     * Invalid sample rates are ignored, leaving the previous rates unchanged.
     *
     * @param sampleRate Audio sample rate in samples per second.
     */
    void SetSampleRate(float sampleRate) noexcept
    {
        if (sampleRate <= 0.0f || !std::isfinite(sampleRate)) {
            return;
        }
        mAttackRate = 1.0f / (sampleRate * mAttackSeconds);
        mReleaseRate = 1.0f / (sampleRate * mReleaseSeconds);
    }

    /**
     * Sets the maximum gain boost and the amplitude threshold.
     *
     * The threshold is clamped to [minimumThreshold, 1]. A maximum gain at or
     * below 0 dB disables gain boosting.
     *
     * @param maxGainDb Maximum gain in decibels.
     * @param threshold Threshold as a linear signal amplitude.
     */
    void SetParameters(float maxGainDb, float threshold) noexcept
    {
        threshold = std::clamp(threshold, mMinimumThreshold, 1.0f);
        if (maxGainDb == mMaxGainDb && threshold == mThreshold) {
            return;
        }
        mMaxGainDb = maxGainDb;
        mMaxGain = DbToLinear(maxGainDb);
        mThreshold = threshold;
    }

    /**
     * Compresses one input sample and updates envelope and gain state.
     *
     * Non-finite input is treated as silence and returns zero.
     *
     * @param input Input sample.
     * @return Processed sample.
     */
    float Process(float input) noexcept
    {
        if (!std::isfinite(input)) {
            return 0.0f;
        }

        const float magnitude = std::abs(input);
        const float envelopeAttack = static_cast<float>(magnitude > mEnvelope);
        const float envelopeRate = mReleaseRate + (mAttackRate - mReleaseRate) * envelopeAttack;
        mEnvelope += (magnitude - mEnvelope) * envelopeRate;

        const float compressionAmount = std::sqrt(mThreshold / std::max(mEnvelope, mThreshold));
        const float compressedGain = std::max(1.0f, mMaxGain * compressionAmount);
        const float compressionEnabled = static_cast<float>(mMaxGain > 1.0f);
        const float targetGain = mMaxGain + (compressedGain - mMaxGain) * compressionEnabled;
        const float gainAttack = static_cast<float>(targetGain < mGain);
        const float gainRate = mReleaseRate + (mAttackRate - mReleaseRate) * gainAttack;
        mGain += (targetGain - mGain) * gainRate;
        return input * mGain;
    }

   private:
    static float DbToLinear(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

    float mEnvelope = 0.0f;
    float mGain = 1.0f;
    float mMaxGainDb = 0.0f;
    float mMaxGain = 1.0f;
    float mThreshold = 0.125f;
    float mAttackRate = 0.0f;
    float mReleaseRate = 0.0f;
    float mAttackSeconds;
    float mReleaseSeconds;
    float mMinimumThreshold;
};