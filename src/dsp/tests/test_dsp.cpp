#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/multi_band_splitter.h"
#include "dsp/multi_band_wide_dynamic_range_compressor.h"
#include "dsp/one_pole_low_pass.h"
#include "dsp/peak_limiter.h"
#include "dsp/wide_dynamic_range_compressor.h"

#include <array>
#include <cmath>
#include <limits>

using Catch::Approx;

namespace {

constexpr float kSampleRate = 48000.0f;
using DefaultSplitter = FourBandSplitter<OnePoleLowPass>;
using DefaultCompressor =
    FourBandWideDynamicRangeCompressor<DefaultSplitter, WideDynamicRangeCompressor>;
using FiveBandSplitter = MultiBandSplitter<5, OnePoleLowPass>;
using FiveBandCompressor =
    MultiBandWideDynamicRangeCompressor<5, FiveBandSplitter, WideDynamicRangeCompressor>;

float MeasureCompressionGain(float maxGainDb, float threshold, float amplitude)
{
    WideDynamicRangeCompressor compressor;
    compressor.SetSampleRate(kSampleRate);
    compressor.SetParameters(maxGainDb, threshold);
    float output = 0.0f;
    for (int sample = 0; sample < 48000; ++sample) {
        output = compressor.Process(amplitude);
    }
    return output / amplitude;
}

}  // namespace

TEST_CASE("wide dynamic range compressor favors low-level input")
{
    const float quietGain = MeasureCompressionGain(6.0f, 0.125f, 0.01f);
    const float loudGain = MeasureCompressionGain(6.0f, 0.125f, 0.8f);

    REQUIRE(quietGain == Approx(1.995262f).margin(0.01f));
    REQUIRE(loudGain < quietGain);
    REQUIRE(loudGain >= 1.0f);
    REQUIRE(loudGain < 1.05f);
}

TEST_CASE("wide dynamic range compressor contains invalid input")
{
    WideDynamicRangeCompressor compressor;
    compressor.SetSampleRate(kSampleRate);
    compressor.SetParameters(6.0f, 0.125f);

    REQUIRE(compressor.Process(std::numeric_limits<float>::infinity()) == 0.0f);
    REQUIRE(compressor.Process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("four-band compressor reconstructs transparently at unity gain")
{
    DefaultCompressor compressor;
    compressor.SetSampleRate(kSampleRate);
    compressor.SetParameters({}, 0.125f);

    for (int sample = 0; sample < 10000; ++sample) {
        const float input = std::sin(sample * 0.071f) * 0.7f;
        REQUIRE(compressor.Process(input) == Approx(input).margin(1e-6f));
    }
}

TEST_CASE("four-band splitter accepts custom crossover frequencies")
{
    DefaultSplitter splitter({120.0f, 800.0f, 3200.0f});
    splitter.SetSampleRate(44100.0f);

    const std::array<float, 4> bands = splitter.Process(0.5f);
    REQUIRE(std::isfinite(bands[0]));
    REQUIRE(std::isfinite(bands[1]));
    REQUIRE(std::isfinite(bands[2]));
    REQUIRE(std::isfinite(bands[3]));
}

TEST_CASE("multi-band compressor accepts a compile-time band count")
{
    FiveBandCompressor compressor({100.0f, 400.0f, 1600.0f, 6400.0f});
    compressor.SetSampleRate(kSampleRate);
    compressor.SetParameters({}, 0.125f);

    const auto bands = FiveBandSplitter({100.0f, 400.0f, 1600.0f, 6400.0f}).Process(0.5f);
    REQUIRE(bands.size() == 5);
    REQUIRE(std::isfinite(compressor.Process(0.5f)));
}

TEST_CASE("one-pole low pass updates its coefficient when the sample rate changes")
{
    OnePoleLowPass filter(1000.0f);
    filter.SetSampleRate(48000.0f);
    const float outputAt48000 = filter.Process(1.0f);

    filter.SetSampleRate(24000.0f);
    const float outputAt24000 = filter.Process(1.0f);

    REQUIRE(outputAt24000 > outputAt48000);
}

TEST_CASE("peak limiter caps peaks and recovers smoothly")
{
    PeakLimiter limiter;
    limiter.SetSampleRate(kSampleRate);

    REQUIRE(limiter.Process(1.0f) == Approx(PeakLimiter::Ceiling()));
    REQUIRE(std::abs(limiter.Process(-2.0f)) <= PeakLimiter::Ceiling());

    float recovered = limiter.Process(0.5f);
    for (int sample = 0; sample < 48000; ++sample) {
        recovered = limiter.Process(0.5f);
    }
    REQUIRE(recovered == Approx(0.5f).margin(0.0001f));
}
