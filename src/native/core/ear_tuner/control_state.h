#pragma once

#include "ear_tuner/focus_types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

/**
 * Atomically shared control values consumed by the audio processing thread.
 *
 * Each field is independently atomic; reading a set of fields does not
 * provide a transactional snapshot across concurrent updates.
 */
struct EarTunerControlState {
    /** Whether focus processing is enabled. */
    std::atomic<bool> mFocusEnabled{false};
    /** Numeric FocusMode value for the left channel. */
    std::atomic<uint8_t> mLeftFocusMode{2};
    /** Numeric FocusMode value for the right channel. */
    std::atomic<uint8_t> mRightFocusMode{2};
    /** Focus strength level for the left channel. */
    std::atomic<uint8_t> mLeftFocusLevel{2};
    /** Focus strength level for the right channel. */
    std::atomic<uint8_t> mRightFocusLevel{2};
    /** Whether focus processing is bypassed by the host. */
    std::atomic<bool> mFocusBypassed{false};
    /** Linear output gain; EarTunerProcessor clamps it to its supported range. */
    std::atomic<float> mOutputGain{1.0f};
    /** Per-band maximum gains in dB for Low focus mode. */
    std::array<std::atomic<float>, 4> mLowGainsDb{3.0f, 1.5f, 0.0f, 0.0f};
    /** Per-band maximum gains in dB for Voices focus mode. */
    std::array<std::atomic<float>, 4> mVoiceGainsDb{0.0f, 1.5f, 3.0f, 0.0f};
    /** Per-band maximum gains in dB for HighPitch focus mode. */
    std::array<std::atomic<float>, 4> mHighGainsDb{0.0f, 0.0f, 1.5f, 3.0f};
    /** Maximum gain in dB for QuietSounds focus mode. */
    std::atomic<float> mQuietMaxGainDb{3.0f};
    /** Linear signal-amplitude threshold used for compression. */
    std::atomic<float> mCompressionThreshold{0.125f};
};

/**
 * Reads a channel's focus mode and level from a query string.
 *
 * Looks for `<channel>_mode` and `<channel>_level` parameters. Both must parse
 * successfully for the function to return true.
 *
 * @param query Query string containing the parameters.
 * @param channel Channel prefix, such as "left" or "right".
 * @param mode Receives the parsed mode value.
 * @param level Receives the parsed level value.
 * @return True when both parameters are present and numeric.
 */
bool ParseFocus(const std::string &query, const std::string &channel, double &mode, double &level);

/**
 * Loads the current tuning values from atomic control state.
 *
 * Values are loaded independently with relaxed ordering, so concurrent updates
 * may result in a snapshot containing values from different update moments.
 *
 * @param state Control state to read.
 * @return A non-atomic copy suitable for configuring DSP processing.
 */
DspTuning SnapshotDspTuning(const EarTunerControlState &state);

/**
 * Parses and applies tuning parameters present in a query string.
 *
 * Band gains are clamped to [-12, 12] dB, quiet maximum gain to [0, 12] dB,
 * and compression threshold to [0.01, 1]. Unspecified parameters are unchanged.
 *
 * @param state Control state to update.
 * @param query Query string containing tuning parameters.
 * @return True if at least one recognized tuning parameter was updated.
 */
bool UpdateDspTuning(EarTunerControlState &state, const std::string &query);