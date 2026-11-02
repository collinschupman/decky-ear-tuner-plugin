#include "ear_tuner/audio_block_processor.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

/*
 * Microbenchmarks for each DSP module and the full audio block path.
 *
 * Each measurement processes one block of frames, so results can be compared
 * directly against the real-time budget of frames / kSampleRate seconds
 * (5333 us for 256 frames at 48 kHz). Processor state persists between runs,
 * matching the steady-state behaviour of the audio callback.
 */

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr uint32_t kBlockFrames = 256;
constexpr uint32_t kMaxBlockFrames = 1024;
constexpr float kPi = 3.14159265358979323846f;

/**
 * Generates material spanning all four crossover bands, with slow amplitude
 * modulation so compressors and limiters keep changing state.
 */
std::vector<float> MakeSignal(float phaseOffset)
{
    constexpr float kFrequencies[] = {120.0f, 700.0f, 2500.0f, 8000.0f};
    std::vector<float> signal(kMaxBlockFrames);
    for (uint32_t frame = 0; frame < kMaxBlockFrames; ++frame) {
        const float time = static_cast<float>(frame) / kSampleRate;
        float sample = 0.0f;
        for (const float frequency : kFrequencies) {
            sample += std::sin(2.0f * kPi * frequency * time + phaseOffset);
        }
        const float envelope = 0.05f + 0.45f * (0.5f + 0.5f * std::sin(2.0f * kPi * 3.0f * time));
        signal[frame] = 0.25f * envelope * sample;
    }
    return signal;
}

const std::vector<float> &LeftSignal()
{
    static const std::vector<float> signal = MakeSignal(0.0f);
    return signal;
}

const std::vector<float> &RightSignal()
{
    static const std::vector<float> signal = MakeSignal(1.3f);
    return signal;
}

std::string BlockName(const std::string &name, uint32_t frames = kBlockFrames)
{
    return name + " (" + std::to_string(frames) + " frames)";
}

/** Measures a mono module's per-sample Process() over one block. */
template <typename Module>
void BenchmarkMonoModule(Catch::Benchmark::Chronometer meter, Module &module)
{
    const float *input = LeftSignal().data();
    meter.measure([&] {
        float checksum = 0.0f;
        for (uint32_t frame = 0; frame < kBlockFrames; ++frame) {
            checksum += module.Process(input[frame]);
        }
        return checksum;
    });
}

using BandCompressor = FourBandWideDynamicRangeCompressor<FourBandSplitter<OnePoleLowPass>,
                                                          WideDynamicRangeCompressor>;

}  // namespace

TEST_CASE("DSP module benchmarks", "[benchmark][modules]")
{
    BENCHMARK_ADVANCED(BlockName("OnePoleLowPass"))(Catch::Benchmark::Chronometer meter)
    {
        OnePoleLowPass filter(1000.0f);
        filter.SetSampleRate(kSampleRate);
        BenchmarkMonoModule(meter, filter);
    };

    BENCHMARK_ADVANCED(BlockName("FourBandSplitter"))(Catch::Benchmark::Chronometer meter)
    {
        FourBandSplitter<OnePoleLowPass> splitter;
        splitter.SetSampleRate(kSampleRate);
        const float *input = LeftSignal().data();
        meter.measure([&] {
            float checksum = 0.0f;
            for (uint32_t frame = 0; frame < kBlockFrames; ++frame) {
                for (const float band : splitter.Process(input[frame])) {
                    checksum += band;
                }
            }
            return checksum;
        });
    };

    BENCHMARK_ADVANCED(BlockName("WDRC"))
    (Catch::Benchmark::Chronometer meter)
    {
        WideDynamicRangeCompressor compressor;
        compressor.SetSampleRate(kSampleRate);
        compressor.SetParameters(9.0f, 0.125f);
        BenchmarkMonoModule(meter, compressor);
    };

    BENCHMARK_ADVANCED(BlockName("FourBandWDRC"))
    (Catch::Benchmark::Chronometer meter)
    {
        BandCompressor compressor;
        compressor.SetSampleRate(kSampleRate);
        compressor.SetParameters({0.0f, 3.5f, 9.0f, 0.0f}, 0.125f);
        BenchmarkMonoModule(meter, compressor);
    };

    BENCHMARK_ADVANCED(BlockName("PeakLimiter"))(Catch::Benchmark::Chronometer meter)
    {
        PeakLimiter limiter;
        limiter.SetSampleRate(kSampleRate);
        BenchmarkMonoModule(meter, limiter);
    };
}

TEST_CASE("EarTunerProcessor benchmarks", "[benchmark][processor]")
{
    const auto benchmarkProcessor = [](Catch::Benchmark::Chronometer meter, FocusMode mode) {
        EarTunerProcessor processor;
        processor.Configure(mode, 3, mode, 3, kSampleRate);
        const float *left = LeftSignal().data();
        const float *right = RightSignal().data();
        meter.measure([&] {
            float checksum = 0.0f;
            for (uint32_t frame = 0; frame < kBlockFrames; ++frame) {
                StereoFrame stereo = StereoFrame::FromChannels(left[frame], right[frame]);
                processor.Process(stereo);
                checksum += stereo.Left() + stereo.Right();
            }
            return checksum;
        });
    };

    BENCHMARK_ADVANCED(BlockName("Processor, focus off"))
    (Catch::Benchmark::Chronometer meter) { benchmarkProcessor(meter, FocusMode::Off); };

    BENCHMARK_ADVANCED(BlockName("Processor, focus on"))
    (Catch::Benchmark::Chronometer meter) { benchmarkProcessor(meter, FocusMode::Voices); };
}

TEST_CASE("ProcessAudioBlock benchmarks", "[benchmark][block]")
{
    const auto benchmarkBlock = [](Catch::Benchmark::Chronometer meter, bool focusEnabled,
                                   uint32_t frames, bool silentInput) {
        EarTunerControlState control;
        control.mFocusEnabled.store(focusEnabled);
        control.mLeftFocusMode.store(static_cast<uint8_t>(FocusMode::Voices));
        control.mRightFocusMode.store(static_cast<uint8_t>(FocusMode::QuietSounds));
        control.mLeftFocusLevel.store(3);
        control.mRightFocusLevel.store(3);
        EarTunerProcessor processor;
        std::vector<float> outputLeft(frames);
        std::vector<float> outputRight(frames);
        const float *inputLeft = silentInput ? nullptr : LeftSignal().data();
        const float *inputRight = silentInput ? nullptr : RightSignal().data();
        meter.measure([&] {
            ProcessAudioBlock(control, processor, inputLeft, inputRight, outputLeft.data(),
                              outputRight.data(), frames, kSampleRate);
            return outputLeft[frames - 1] + outputRight[frames - 1];
        });
    };

    for (const uint32_t frames : {64u, kBlockFrames, kMaxBlockFrames}) {
        BENCHMARK_ADVANCED(BlockName("Block, focus off", frames))
        (Catch::Benchmark::Chronometer meter) { benchmarkBlock(meter, false, frames, false); };

        BENCHMARK_ADVANCED(BlockName("Block, focus on", frames))
        (Catch::Benchmark::Chronometer meter) { benchmarkBlock(meter, true, frames, false); };
    }

    BENCHMARK_ADVANCED(BlockName("Block, silence"))
    (Catch::Benchmark::Chronometer meter) { benchmarkBlock(meter, true, kBlockFrames, true); };
}
