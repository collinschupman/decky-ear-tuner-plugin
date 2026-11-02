#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

/** Available frequency focus modes for a channel. */
enum class FocusMode : uint8_t {
    Off = 0,
    Low = 1,
    Voices = 2,
    HighPitch = 3,
    QuietSounds = 4,
};

/** User-configurable maximum gains and compression threshold for focus DSP. */
struct DspTuning {
    /** Four-band maximum gains in dB for Low focus. */
    std::array<float, 4> mLowGainsDb{3.0f, 1.5f, 0.0f, 0.0f};
    /** Four-band maximum gains in dB for Voices focus. */
    std::array<float, 4> mVoiceGainsDb{0.0f, 1.5f, 3.0f, 0.0f};
    /** Four-band maximum gains in dB for HighPitch focus. */
    std::array<float, 4> mHighGainsDb{0.0f, 0.0f, 1.5f, 3.0f};
    /** Maximum gain in dB applied across bands for QuietSounds focus. */
    float mQuietMaxGainDb = 3.0f;
    /** Compression threshold as a linear signal amplitude. */
    float mCompressionThreshold = 0.125f;
};

/**
 * Resolves a selected mode against the global enabled flag.
 *
 * @return selectedMode when enabled, otherwise FocusMode::Off.
 */
inline FocusMode EffectiveFocusMode(bool enabled, FocusMode selectedMode)
{
    return enabled ? selectedMode : FocusMode::Off;
}

/** Returns true only when focus is enabled and a feature was requested. */
inline bool ActiveWithEarTuner(bool focusEnabled, bool requested)
{
    return focusEnabled && requested;
}

/**
 * Selects processed audio for active modes and the original audio for Off.
 *
 * @param mode Selected focus mode.
 * @param dry Original sample.
 * @param processed Sample after focus DSP.
 */
inline float SelectFocusOutput(FocusMode mode, float dry, float processed)
{
    return mode == FocusMode::Off ? dry : processed;
}

/**
 * Computes per-band compressor maximum gains for a focus mode and level.
 *
 * Levels are clamped to [0, 3] and scale configured gains using strengths
 * {0, 1, 2.25, 3.5}. QuietSounds applies the scaled quiet gain equally to all
 * four bands; Off returns unity-gain settings (0 dB in every band).
 *
 * @param mode Focus mode whose gain preset is selected.
 * @param level Requested focus level.
 * @param tuning User-configurable gains; defaults to the built-in tuning.
 * @return Maximum compressor gains in dB for the four bands.
 */
inline std::array<float, 4> FocusCompressionGains(FocusMode mode, uint8_t level,
                                                  const DspTuning &tuning = {})
{
    constexpr std::array<float, 4> kStrengthByLevel{0.0f, 1.0f, 2.25f, 3.5f};
    const float strength = kStrengthByLevel[std::clamp<uint8_t>(level, 0, 3)];
    const std::array<float, 4> *gains = nullptr;
    switch (mode) {
        case FocusMode::Low:
            gains = &tuning.mLowGainsDb;
            break;
        case FocusMode::Voices:
            gains = &tuning.mVoiceGainsDb;
            break;
        case FocusMode::HighPitch:
            gains = &tuning.mHighGainsDb;
            break;
        case FocusMode::QuietSounds:
            return {tuning.mQuietMaxGainDb * strength, tuning.mQuietMaxGainDb * strength,
                    tuning.mQuietMaxGainDb * strength, tuning.mQuietMaxGainDb * strength};
        case FocusMode::Off:
            return {};
    }
    std::array<float, 4> scaledGains{};
    for (size_t band = 0; band < scaledGains.size(); ++band) {
        scaledGains[band] = (*gains)[band] * strength;
    }
    return scaledGains;
}

/**
 * Selects a compression threshold that accounts for the maximum band boost.
 *
 * The requested maximum threshold is reduced as needed so the strongest
 * configured gain reaches unity output at the threshold, then clamped to
 * [0.001, 1].
 *
 * @param gainsDb Maximum gains in dB for each band.
 * @param maximumThreshold Requested upper bound as a linear amplitude.
 * @return Effective threshold as a linear amplitude.
 */
inline float EffectiveCompressionThreshold(const std::array<float, 4> &gainsDb,
                                           float maximumThreshold)
{
    constexpr float kMinimumThreshold = 0.001f;
    float maximumGainDb = 0.0f;
    for (const float gainDb : gainsDb) {
        maximumGainDb = std::max(maximumGainDb, gainDb);
    }
    const float unityGainThreshold = std::pow(10.0f, -maximumGainDb / 10.0f);
    return std::clamp(std::min(maximumThreshold, unityGainThreshold), kMinimumThreshold, 1.0f);
}