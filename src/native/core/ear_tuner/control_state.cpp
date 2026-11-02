#include "ear_tuner/control_state.h"

#include "http/http_parse.h"

#include <algorithm>

namespace {

constexpr float kMinEqGainDb = -12.0f;
constexpr float kMaxEqGainDb = 12.0f;
constexpr float kMinQuietGainDb = 0.0f;
constexpr float kMaxQuietGainDb = 12.0f;
constexpr float kMinCompressionThreshold = 0.01f;
constexpr float kMaxCompressionThreshold = 1.0f;
bool UpdateTuningBand(const std::string &query, const std::string &name,
                      std::array<std::atomic<float>, 4> &destination)
{
    bool updated = false;
    for (size_t band = 0; band < destination.size(); ++band) {
        double value = 0.0;
        if (!ParseParam(query, "tune_" + name + "_" + std::to_string(band), value)) {
            continue;
        }
        destination[band].store(std::clamp(static_cast<float>(value), kMinEqGainDb, kMaxEqGainDb),
                                std::memory_order_relaxed);
        updated = true;
    }
    return updated;
}

}  // namespace

static_assert(std::atomic<float>::is_always_lock_free);

bool ParseFocus(const std::string &query, const std::string &channel, double &mode, double &level)
{
    return ParseParam(query, channel + "_mode", mode) &&
           ParseParam(query, channel + "_level", level);
}

DspTuning SnapshotDspTuning(const EarTunerControlState &state)
{
    DspTuning tuning;
    for (size_t band = 0; band < tuning.mLowGainsDb.size(); ++band) {
        tuning.mLowGainsDb[band] = state.mLowGainsDb[band].load(std::memory_order_relaxed);
        tuning.mVoiceGainsDb[band] = state.mVoiceGainsDb[band].load(std::memory_order_relaxed);
        tuning.mHighGainsDb[band] = state.mHighGainsDb[band].load(std::memory_order_relaxed);
    }
    tuning.mQuietMaxGainDb = state.mQuietMaxGainDb.load(std::memory_order_relaxed);
    tuning.mCompressionThreshold = state.mCompressionThreshold.load(std::memory_order_relaxed);
    return tuning;
}

bool UpdateDspTuning(EarTunerControlState &state, const std::string &query)
{
    bool updated = UpdateTuningBand(query, "low", state.mLowGainsDb);
    updated = UpdateTuningBand(query, "voice", state.mVoiceGainsDb) || updated;
    updated = UpdateTuningBand(query, "high", state.mHighGainsDb) || updated;

    double value = 0.0;
    if (ParseParam(query, "tune_quiet_gain", value)) {
        state.mQuietMaxGainDb.store(
            std::clamp(static_cast<float>(value), kMinQuietGainDb, kMaxQuietGainDb),
            std::memory_order_relaxed);
        updated = true;
    }
    if (ParseParam(query, "tune_compression_threshold", value) ||
        ParseParam(query, "tune_quiet_threshold", value)) {
        state.mCompressionThreshold.store(
            std::clamp(static_cast<float>(value), kMinCompressionThreshold,
                       kMaxCompressionThreshold),
            std::memory_order_relaxed);
        updated = true;
    }
    return updated;
}