#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/multi_band_splitter.h"
#include "dsp/multi_band_wide_dynamic_range_compressor.h"
#include "dsp/one_pole_low_pass.h"
#include "dsp/peak_limiter.h"
#include "dsp/wide_dynamic_range_compressor.h"
#include "ear_tuner/audio_block_processor.h"
#include "ear_tuner/control_state.h"
#include "ear_tuner/ear_tuner_processor.h"
#include "ear_tuner/focus_types.h"
#include "http/http_parse.h"

#include <limits>

using Catch::Approx;

constexpr float kTestSampleRate = 48000.0f;

float MeasureCompressionGain(float maxGainDb, float threshold, float amplitude)
{
    WideDynamicRangeCompressor compressor;
    compressor.SetSampleRate(kTestSampleRate);
    compressor.SetParameters(maxGainDb, threshold);
    float output = 0.0f;
    for (int sample = 0; sample < 48000; ++sample) {
        output = compressor.Process(amplitude);
    }
    return output / amplitude;
}

float MeasureBandCompressionGain(const std::array<float, 4> &maxGainsDb, float frequency,
                                 float amplitude)
{
    FourBandWideDynamicRangeCompressor<FourBandSplitter<OnePoleLowPass>, WideDynamicRangeCompressor>
        compressor;
    compressor.SetSampleRate(kTestSampleRate);
    compressor.SetParameters(maxGainsDb, 0.125f);
    constexpr float kSampleRate = 48000.0f;
    constexpr float kPi = 3.14159265358979323846f;
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    for (int sample = 0; sample < 48000; ++sample) {
        const float input = amplitude * std::sin(2.0f * kPi * frequency * sample / kSampleRate);
        const float output = compressor.Process(input);
        if (sample >= 24000) {
            inputEnergy += input * input;
            outputEnergy += output * output;
        }
    }
    return static_cast<float>(std::sqrt(outputEnergy / inputEnergy));
}

TEST_CASE("focus compression presets preserve modes and strength")
{
    REQUIRE(FocusCompressionGains(FocusMode::Off, 3) == std::array<float, 4>{});
    REQUIRE(FocusCompressionGains(FocusMode::Low, 2) ==
            std::array<float, 4>{6.75f, 3.375f, 0.0f, 0.0f});
    REQUIRE(FocusCompressionGains(FocusMode::Voices, 2) ==
            std::array<float, 4>{0.0f, 3.375f, 6.75f, 0.0f});
    REQUIRE(FocusCompressionGains(FocusMode::HighPitch, 2) ==
            std::array<float, 4>{0.0f, 0.0f, 3.375f, 6.75f});
    REQUIRE(FocusCompressionGains(FocusMode::QuietSounds, 2) ==
            std::array<float, 4>{6.75f, 6.75f, 6.75f, 6.75f});
}

TEST_CASE("focus compression levels use a wider strength curve")
{
    REQUIRE(FocusCompressionGains(FocusMode::Low, 1) ==
            std::array<float, 4>{3.0f, 1.5f, 0.0f, 0.0f});
    REQUIRE(FocusCompressionGains(FocusMode::Low, 2) ==
            std::array<float, 4>{6.75f, 3.375f, 0.0f, 0.0f});
    REQUIRE(FocusCompressionGains(FocusMode::Low, 3) ==
            std::array<float, 4>{10.5f, 5.25f, 0.0f, 0.0f});
}

TEST_CASE("compression knee adapts to maximum gain")
{
    const auto weak = FocusCompressionGains(FocusMode::HighPitch, 1);
    const auto medium = FocusCompressionGains(FocusMode::HighPitch, 2);
    const auto strong = FocusCompressionGains(FocusMode::HighPitch, 3);

    REQUIRE(20.0f * std::log10(EffectiveCompressionThreshold(weak, 0.125f)) == Approx(-18.0618f));
    REQUIRE(20.0f * std::log10(EffectiveCompressionThreshold(medium, 0.125f)) == Approx(-18.0618f));
    REQUIRE(20.0f * std::log10(EffectiveCompressionThreshold(strong, 0.125f)) == Approx(-21.0f));
    REQUIRE(EffectiveCompressionThreshold({36.0f, 0.0f, 0.0f, 0.0f}, 0.125f) == Approx(0.001f));
}

TEST_CASE("wide dynamic range compression favors low-level input")
{
    const float quietGain = MeasureCompressionGain(6.0f, 0.125f, 0.01f);
    const float loudGain = MeasureCompressionGain(6.0f, 0.125f, 0.8f);

    REQUIRE(quietGain == Approx(1.995262f).margin(0.01f));
    REQUIRE(loudGain < quietGain);
    REQUIRE(loudGain >= 1.0f);
    REQUIRE(loudGain < 1.05f);
}

TEST_CASE("focus compression modes favor their target frequency regions")
{
    const auto low = FocusCompressionGains(FocusMode::Low, 2);
    const auto high = FocusCompressionGains(FocusMode::HighPitch, 2);

    REQUIRE(MeasureBandCompressionGain(low, 80.0f, 0.01f) >
            MeasureBandCompressionGain(low, 8000.0f, 0.01f) * 1.3f);
    REQUIRE(MeasureBandCompressionGain(high, 8000.0f, 0.01f) >
            MeasureBandCompressionGain(high, 80.0f, 0.01f) * 1.3f);
}

TEST_CASE("wide dynamic range compression contains non-finite input")
{
    WideDynamicRangeCompressor compressor;
    compressor.SetSampleRate(kTestSampleRate);
    compressor.SetParameters(6.0f, 0.125f);

    REQUIRE(compressor.Process(std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE(compressor.Process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("four-band compression reconstructs transparently at unity gain")
{
    FourBandWideDynamicRangeCompressor<FourBandSplitter<OnePoleLowPass>, WideDynamicRangeCompressor>
        compressor;
    compressor.SetSampleRate(kTestSampleRate);
    compressor.SetParameters({}, 0.125f);

    for (int sample = 0; sample < 10000; ++sample) {
        const float input = std::sin(sample * 0.071f) * 0.7f;
        REQUIRE(compressor.Process(input) == Approx(input).margin(1e-6f));
    }
}

TEST_CASE("Ear Tuner toggle bypasses selected modes")
{
    REQUIRE(EffectiveFocusMode(false, FocusMode::Voices) == FocusMode::Off);
    REQUIRE(EffectiveFocusMode(true, FocusMode::Voices) == FocusMode::Voices);
}

TEST_CASE("Ear Tuner gates bypass requests")
{
    REQUIRE_FALSE(ActiveWithEarTuner(false, true));
    REQUIRE_FALSE(ActiveWithEarTuner(true, false));
    REQUIRE(ActiveWithEarTuner(true, true));
}

TEST_CASE("Ear Tuner bypass returns the dry signal exactly")
{
    REQUIRE(SelectFocusOutput(FocusMode::Off, 0.25f, 0.75f) == 0.25f);
    REQUIRE(SelectFocusOutput(FocusMode::Voices, 0.25f, 0.75f) == 0.75f);
}

#if defined(__GNUC__) || defined(__clang__)
TEST_CASE("PeakLimiter is transparent below its ceiling")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kTestSampleRate);
    const float silence = limiter.Process(0.0f);
    const float output = limiter.Process(0.5f);

    REQUIRE(silence == 0.0f);
    REQUIRE(output == Approx(0.5f));
}

TEST_CASE("PeakLimiter caps peaks")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kTestSampleRate);

    REQUIRE(limiter.Process(1.0f) == Approx(PeakLimiter::Ceiling()));
    REQUIRE(limiter.Process(-2.0f) == Approx(-PeakLimiter::Ceiling()));
}

TEST_CASE("PeakLimiter caps every sample during changing overloads")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kTestSampleRate);
    constexpr std::array<float, 6> kInputs{0.5f, 2.0f, -4.0f, 1.25f, -8.0f, 0.25f};

    for (const float input : kInputs) {
        REQUIRE(std::abs(limiter.Process(input)) <= PeakLimiter::Ceiling());
    }
}

TEST_CASE("PeakLimiter releases smoothly after an overload")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kTestSampleRate);
    limiter.Process(2.0f);
    const float firstRecovery = limiter.Process(0.5f);

    float recovered = firstRecovery;
    for (int sample = 0; sample < 48000; ++sample) {
        recovered = limiter.Process(0.5f);
    }

    REQUIRE(firstRecovery < 0.5f);
    REQUIRE(recovered == Approx(0.5f).margin(0.0001f));
}

TEST_CASE("PeakLimiter contains non-finite samples")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kTestSampleRate);

    REQUIRE(limiter.Process(std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE(limiter.Process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("EarTunerProcessor preserves its dry path")
{
    EarTunerProcessor processor;
    processor.Configure(FocusMode::Off, 2, FocusMode::Off, 2, kTestSampleRate, {});

    StereoFrame output = StereoFrame::FromChannels(0.5f, -0.25f);
    processor.Process(output);

    REQUIRE(output.Left() == Approx(0.5f));
    REQUIRE(output.Right() == Approx(-0.25f));
}

TEST_CASE("EarTunerProcessor ramps processing changes")
{
    EarTunerProcessor processor;
    processor.Configure(FocusMode::Low, 3, FocusMode::Low, 3, kTestSampleRate, {});

    StereoFrame first = StereoFrame::FromChannels(0.5f, 0.5f);
    processor.Process(first);
    REQUIRE(first.Left() == Approx(0.5f).margin(0.001f));

    StereoFrame focused = first;
    for (int sample = 0; sample < 2000; ++sample) {
        focused = StereoFrame::FromChannels(0.5f, 0.5f);
        processor.Process(focused);
    }
    REQUIRE(std::abs(focused.Left() - 0.5f) > 0.05f);

    processor.Configure(FocusMode::Off, 3, FocusMode::Off, 3, kTestSampleRate, {});
    StereoFrame firstBypassed = StereoFrame::FromChannels(0.5f, 0.5f);
    processor.Process(firstBypassed);
    REQUIRE(firstBypassed.Left() == Approx(focused.Left()).margin(0.001f));

    StereoFrame dry = firstBypassed;
    for (int sample = 0; sample < 2000; ++sample) {
        dry = StereoFrame::FromChannels(0.5f, 0.5f);
        processor.Process(dry);
    }
    REQUIRE(dry.Left() == Approx(0.5f));
}

TEST_CASE("EarTunerProcessor keeps its wet chain stable while fading to dry")
{
    EarTunerProcessor fadingProcessor;
    EarTunerProcessor wetReference;
    fadingProcessor.Configure(FocusMode::Low, 3, FocusMode::Low, 3, kTestSampleRate, {});
    wetReference.Configure(FocusMode::Low, 3, FocusMode::Low, 3, kTestSampleRate, {});

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kFrequency = 80.0f;
    for (int sample = 0; sample < 4000; ++sample) {
        const float input = 0.08f * std::sin(2.0f * kPi * kFrequency * sample / kTestSampleRate);
        StereoFrame fading = StereoFrame::FromChannels(input, input);
        StereoFrame reference = fading;
        fadingProcessor.Process(fading);
        wetReference.Process(reference);
    }

    fadingProcessor.Configure(FocusMode::Off, 3, FocusMode::Off, 3, kTestSampleRate, {});
    constexpr float kFadeSamples = kTestSampleRate * 0.02f;
    for (int sample = 0; sample < 200; ++sample) {
        const float input =
            0.08f * std::sin(2.0f * kPi * kFrequency * (sample + 4000) / kTestSampleRate);
        StereoFrame fading = StereoFrame::FromChannels(input, input);
        StereoFrame reference = fading;
        fadingProcessor.Process(fading);
        wetReference.Process(reference);

        const float wetMix = 1.0f - static_cast<float>(sample + 1) / kFadeSamples;
        const float expected = input + (reference.Left() - input) * wetMix;
        REQUIRE(fading.Left() == Approx(expected).margin(1e-5f));
    }
}

TEST_CASE("EarTunerProcessor keeps the wet chain warm while bypassed")
{
    EarTunerProcessor processor;
    EarTunerProcessor wetReference;
    processor.Configure(FocusMode::Low, 3, FocusMode::Low, 3, kTestSampleRate, {});
    wetReference.Configure(FocusMode::Low, 3, FocusMode::Low, 3, kTestSampleRate, {});
    processor.SetFocusEnabled(false);

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kFrequency = 80.0f;
    for (int sample = 0; sample < 4000; ++sample) {
        const float input = 0.08f * std::sin(2.0f * kPi * kFrequency * sample / kTestSampleRate);
        StereoFrame bypassed = StereoFrame::FromChannels(input, input);
        StereoFrame reference = bypassed;
        processor.Process(bypassed);
        wetReference.Process(reference);
        if (sample >= 1000) {
            REQUIRE(bypassed.Left() == Approx(input).margin(1e-7f));
        }
    }

    processor.SetFocusEnabled(true);
    const float input = 0.08f * std::sin(2.0f * kPi * kFrequency * 4000 / kTestSampleRate);
    StereoFrame enabled = StereoFrame::FromChannels(input, input);
    StereoFrame reference = enabled;
    processor.Process(enabled);
    wetReference.Process(reference);

    constexpr float kFirstWetMix = 1.0f / (kTestSampleRate * 0.02f);
    const float expected = input + (reference.Left() - input) * kFirstWetMix;
    REQUIRE(enabled.Left() == Approx(expected).margin(1e-6f));
}

TEST_CASE("EarTunerProcessor initializes and ramps output gain")
{
    EarTunerProcessor processor;
    processor.Configure(FocusMode::Off, 2, FocusMode::Off, 2, kTestSampleRate, {}, 0.5f);

    StereoFrame initial = StereoFrame::FromChannels(0.8f, -0.4f);
    processor.Process(initial);
    REQUIRE(initial.Left() == Approx(0.4f));
    REQUIRE(initial.Right() == Approx(-0.2f));

    processor.Configure(FocusMode::Off, 2, FocusMode::Off, 2, kTestSampleRate, {}, 0.0f);
    StereoFrame firstRampSample = StereoFrame::FromChannels(0.8f, -0.4f);
    processor.Process(firstRampSample);
    REQUIRE(firstRampSample.Left() < 0.4f);
    REQUIRE(firstRampSample.Left() > 0.39f);

    StereoFrame silent = firstRampSample;
    for (int sample = 0; sample < 1000; ++sample) {
        silent = StereoFrame::FromChannels(0.8f, -0.4f);
        processor.Process(silent);
    }
    REQUIRE(silent.Left() == Approx(0.0f));
    REQUIRE(silent.Right() == Approx(0.0f));
}

TEST_CASE("EarTunerProcessor keeps dry and focused ears independent")
{
    EarTunerProcessor processor;
    processor.Configure(FocusMode::Low, 2, FocusMode::Off, 2, kTestSampleRate, {});
    constexpr float kAmplitude = 0.01f;
    constexpr float kFrequency = 80.0f;
    constexpr float kSampleRate = 48000.0f;
    constexpr float kPi = 3.14159265358979323846f;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;

    for (int sample = 0; sample < 48000; ++sample) {
        const float input = kAmplitude * std::sin(2.0f * kPi * kFrequency * sample / kSampleRate);
        StereoFrame output = StereoFrame::FromChannels(input, input);
        processor.Process(output);
        if (sample >= 24000) {
            leftEnergy += output.Left() * output.Left();
            rightEnergy += output.Right() * output.Right();
            REQUIRE(output.Right() == Approx(input).margin(1e-7f));
        }
    }

    REQUIRE(leftEnergy > rightEnergy * 2.0);
}

TEST_CASE("EarTunerProcessor does not limit the dry ear")
{
    EarTunerProcessor processor;
    processor.Configure(FocusMode::Low, 3, FocusMode::Off, 0, kTestSampleRate, {});

    StereoFrame output;
    for (int sample = 0; sample < 2000; ++sample) {
        output = StereoFrame::FromChannels(2.0f, 0.5f);
        processor.Process(output);
    }

    REQUIRE(output.Left() <= PeakLimiter::Ceiling());
    REQUIRE(output.Right() == Approx(0.5f));
}

#endif

TEST_CASE("ParseControlQuery accepts the versioned control endpoint")
{
    std::string query;
    REQUIRE(ParseControlQuery("GET /api/v1/control?focus_enabled=1 HTTP/1.1\r\n", query));
    REQUIRE(query == "focus_enabled=1");
}

TEST_CASE("ParseControlQuery keeps the legacy endpoint compatible")
{
    std::string query;
    REQUIRE(ParseControlQuery("GET /?legacy=1 HTTP/1.1\r\n", query));
    REQUIRE(query == "legacy=1");
}

TEST_CASE("ParseControlQuery rejects unknown API versions and paths")
{
    std::string query;
    REQUIRE_FALSE(ParseControlQuery("GET /api/v2/control?focus_enabled=1 HTTP/1.1\r\n", query));
    REQUIRE_FALSE(ParseControlQuery("GET /other?legacy=1 HTTP/1.1\r\n", query));
}

TEST_CASE("ParseParam - finds value")
{
    double v;
    REQUIRE(ParseParam("left=-6&right=3", "left", v));
    REQUIRE(v == Approx(-6.0));
}

TEST_CASE("ParseParam - finds second parameter")
{
    double v;
    REQUIRE(ParseParam("left=-6&right=3", "right", v));
    REQUIRE(v == Approx(3.0));
}

TEST_CASE("ParseParam - returns false for missing key")
{
    double v = 0.0;
    REQUIRE_FALSE(ParseParam("right=3", "left", v));
}

TEST_CASE("ParseParam - handles floating point values")
{
    double v;
    REQUIRE(ParseParam("left=-6.5", "left", v));
    REQUIRE(v == Approx(-6.5));
}

TEST_CASE("ParseParam - handles positive values")
{
    double v;
    REQUIRE(ParseParam("left=12", "left", v));
    REQUIRE(v == Approx(12.0));
}

TEST_CASE("ParseParam - empty query string returns false")
{
    double v = 0.0;
    REQUIRE_FALSE(ParseParam("", "left", v));
}

TEST_CASE("ParseFocus reads both values for the requested channel")
{
    double mode = -1.0;
    double level = -1.0;

    REQUIRE(ParseFocus("left_mode=4&left_level=3", "left", mode, level));
    REQUIRE(mode == Approx(4.0));
    REQUIRE(level == Approx(3.0));
    REQUIRE_FALSE(ParseFocus("left_mode=4", "left", mode, level));
}

TEST_CASE("SnapshotDspTuning reads the default tuning state")
{
    const EarTunerControlState state;
    const DspTuning tuning = SnapshotDspTuning(state);

    REQUIRE(tuning.mLowGainsDb == std::array<float, 4>{3.0f, 1.5f, 0.0f, 0.0f});
    REQUIRE(tuning.mVoiceGainsDb == std::array<float, 4>{0.0f, 1.5f, 3.0f, 0.0f});
    REQUIRE(tuning.mHighGainsDb == std::array<float, 4>{0.0f, 0.0f, 1.5f, 3.0f});
    REQUIRE(tuning.mQuietMaxGainDb == Approx(3.0f));
    REQUIRE(tuning.mCompressionThreshold == Approx(0.125f));
}

TEST_CASE("UpdateDspTuning updates supplied values and clamps their ranges")
{
    EarTunerControlState state;

    REQUIRE(UpdateDspTuning(state,
                            "tune_low_0=-20&tune_voice_3=20&tune_high_2=6&tune_quiet_gain=20&"
                            "tune_compression_threshold=0.001"));

    const DspTuning tuning = SnapshotDspTuning(state);
    REQUIRE(tuning.mLowGainsDb[0] == Approx(-12.0f));
    REQUIRE(tuning.mVoiceGainsDb[3] == Approx(12.0f));
    REQUIRE(tuning.mHighGainsDb[2] == Approx(6.0f));
    REQUIRE(tuning.mQuietMaxGainDb == Approx(12.0f));
    REQUIRE(tuning.mCompressionThreshold == Approx(0.01f));
}

TEST_CASE("UpdateDspTuning accepts the quiet threshold alias")
{
    EarTunerControlState state;

    REQUIRE(UpdateDspTuning(state, "tune_quiet_threshold=2"));
    REQUIRE(SnapshotDspTuning(state).mCompressionThreshold == Approx(1.0f));
    REQUIRE_FALSE(UpdateDspTuning(state, "unrecognized=2"));
}

TEST_CASE("ProcessAudioBlock copies dry stereo input")
{
    EarTunerControlState control;
    EarTunerProcessor processor;
    const std::array<float, 4> inputLeft{-0.5f, 0.25f, 0.75f, -1.0f};
    const std::array<float, 4> inputRight{0.5f, -0.25f, -0.75f, 1.0f};
    std::array<float, 4> outputLeft{};
    std::array<float, 4> outputRight{};

    ProcessAudioBlock(control, processor, inputLeft.data(), inputRight.data(), outputLeft.data(),
                      outputRight.data(), inputLeft.size(), kTestSampleRate);

    REQUIRE(outputLeft == inputLeft);
    REQUIRE(outputRight == inputRight);
}

TEST_CASE("ProcessAudioBlock treats missing channels as silence")
{
    EarTunerControlState control;
    EarTunerProcessor processor;
    std::array<float, 4> outputLeft{};
    std::array<float, 4> outputRight{};

    ProcessAudioBlock(control, processor, nullptr, nullptr, outputLeft.data(), outputRight.data(),
                      outputLeft.size(), kTestSampleRate);

    REQUIRE(outputLeft == std::array<float, 4>{});
    REQUIRE(outputRight == std::array<float, 4>{});
}

TEST_CASE("ProcessAudioBlock bypass preserves the original stereo signal")
{
    EarTunerControlState control;
    control.mFocusEnabled.store(true);
    control.mFocusBypassed.store(true);
    control.mLeftFocusMode.store(static_cast<uint8_t>(FocusMode::Low));
    control.mRightFocusMode.store(static_cast<uint8_t>(FocusMode::HighPitch));
    EarTunerProcessor processor;
    const std::array<float, 4> inputLeft{-0.5f, 0.25f, 0.75f, -1.0f};
    const std::array<float, 4> inputRight{0.5f, -0.25f, -0.75f, 1.0f};
    std::array<float, 4> outputLeft{};
    std::array<float, 4> outputRight{};

    ProcessAudioBlock(control, processor, inputLeft.data(), inputRight.data(), outputLeft.data(),
                      outputRight.data(), inputLeft.size(), kTestSampleRate);

    REQUIRE(outputLeft == inputLeft);
    REQUIRE(outputRight == inputRight);
}

TEST_CASE("ProcessAudioBlock fades active modes to None at every supported test rate")
{
    for (float rate : {44100.0f, 48000.0f, 96000.0f}) {
        for (uint32_t count : {1u, 64u, 257u, 1024u}) {
            CAPTURE(rate, count);
            EarTunerControlState control;
            EarTunerProcessor processor;
            control.mFocusEnabled.store(true);
            control.mLeftFocusMode.store(4);
            control.mRightFocusMode.store(4);
            std::array<float, 1024> input{};
            std::array<float, 1024> left{};
            std::array<float, 1024> right{};
            input.fill(0.05f);
            const auto process = [&] {
                for (uint32_t sample = 0; sample < static_cast<uint32_t>(rate); sample += count) {
                    ProcessAudioBlock(control, processor, input.data(), input.data(), left.data(),
                                      right.data(), count, rate);
                }
            };
            process();
            REQUIRE(left[0] > input[0] * 1.1f);
            const float previous = left[count - 1];
            control.mLeftFocusMode.store(0);
            control.mRightFocusMode.store(0);
            ProcessAudioBlock(control, processor, input.data(), input.data(), left.data(),
                              right.data(), 1, rate);
            REQUIRE(left[0] == Approx(previous).margin(0.001f));
            process();
            for (uint32_t sample = 0; sample < count; ++sample) {
                REQUIRE(left[sample] == input[sample]);
                REQUIRE(right[sample] == input[sample]);
            }
        }
    }
}

TEST_CASE("ProcessAudioBlock leaves either None ear unchanged through overload and recovery")
{
    for (bool leftOff : {false, true}) {
        CAPTURE(leftOff);
        EarTunerControlState control;
        EarTunerProcessor processor;
        control.mFocusEnabled.store(true);
        control.mLeftFocusMode.store(leftOff ? 0 : 1);
        control.mRightFocusMode.store(leftOff ? 1 : 0);
        float left = 0.0f;
        float right = 0.0f;
        for (float dry : {1.0f, -1.0f, 0.5f, -0.5f}) {
            const float inputLeft = leftOff ? dry : 0.05f;
            const float inputRight = leftOff ? 0.05f : dry;
            for (int sample = 0; sample < 2000; ++sample) {
                ProcessAudioBlock(control, processor, &inputLeft, &inputRight, &left, &right, 1,
                                  kTestSampleRate);
                REQUIRE((leftOff ? left : right) == dry);
            }
        }
    }
}

TEST_CASE("ProcessAudioBlock limits focused output after applying output gain")
{
    EarTunerControlState control;
    EarTunerProcessor processor;
    control.mFocusEnabled.store(true);
    control.mLeftFocusMode.store(4);
    control.mRightFocusMode.store(4);
    control.mOutputGain.store(1.5f);
    float left = 0.0f;
    float right = 0.0f;
    for (int sample = 0; sample < 2000; ++sample) {
        const float input = 0.0f;
        ProcessAudioBlock(control, processor, &input, &input, &left, &right, 1, kTestSampleRate);
    }
    for (float gain : {1.5f, 0.5f, 1.5f}) {
        control.mOutputGain.store(gain);
        for (int sample = 0; sample < 4000; ++sample) {
            const float input = sample % 2 == 0 ? 1.0f : -1.0f;
            ProcessAudioBlock(control, processor, &input, &input, &left, &right, 1,
                              kTestSampleRate);
            REQUIRE(std::abs(left) <= PeakLimiter::Ceiling());
            REQUIRE(std::abs(right) <= PeakLimiter::Ceiling());
        }
    }
}
