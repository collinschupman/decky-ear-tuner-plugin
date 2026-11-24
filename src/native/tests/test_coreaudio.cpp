#include "platform/macos/audio_callback.h"

#include "ear_tuner/audio_block_processor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

template <size_t Count>
struct Buffers {
    UInt32 mNumberBuffers = Count;
    AudioBuffer mBuffers[Count]{};

    AudioBufferList *List() { return reinterpret_cast<AudioBufferList *>(this); }
};

}  // namespace

TEST_CASE("Core Audio sends stereo DSP output to the selected endpoint", "[coreaudio]")
{
    auto state = std::make_unique<CoreAudioState>();
    state->mControl.mFocusEnabled.store(false);
    std::array<float, 4> input{0.1f, 0.2f, -0.3f, 0.4f};
    Buffers<1> source;
    source.mBuffers[0] = {2, sizeof(input), input.data()};
    std::array<float, 4> output{};
    Buffers<1> destination;
    destination.mBuffers[0] = {2, sizeof(output), output.data()};
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK_FALSE(state->mBufferError.load());
    CHECK(state->mCallbacks.load() == 1);
    CHECK(output == input);
}

TEST_CASE("Core Audio handles planar input and output buffers", "[coreaudio]")
{
    auto state = std::make_unique<CoreAudioState>();
    state->mControl.mFocusEnabled.store(false);
    std::array<float, 2> left{0.15f, 0.25f};
    std::array<float, 2> right{-0.2f, 0.3f};
    std::array<float, 2> outputLeft{};
    std::array<float, 2> outputRight{};
    Buffers<2> source;
    source.mBuffers[0] = {1, sizeof(left), left.data()};
    source.mBuffers[1] = {1, sizeof(right), right.data()};
    Buffers<2> destination;
    destination.mBuffers[0] = {1, sizeof(outputLeft), outputLeft.data()};
    destination.mBuffers[1] = {1, sizeof(outputRight), outputRight.data()};
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK_FALSE(state->mBufferError.load());
    CHECK(outputLeft == left);
    CHECK(outputRight == right);
}

TEST_CASE("Core Audio applies independent ear controls through the shared DSP", "[coreaudio]")
{
    auto state = std::make_unique<CoreAudioState>();
    state->mControl.mFocusEnabled.store(true);
    state->mControl.mLeftFocusMode.store(static_cast<uint8_t>(FocusMode::QuietSounds));
    state->mControl.mRightFocusMode.store(static_cast<uint8_t>(FocusMode::Off));
    std::array<float, 1024> input;
    input.fill(0.01f);
    std::array<float, 1024> left{};
    std::array<float, 1024> right{};
    Buffers<2> source;
    source.mBuffers[0] = {1, sizeof(input), input.data()};
    source.mBuffers[1] = {1, sizeof(input), input.data()};
    Buffers<2> destination;
    destination.mBuffers[0] = {1, sizeof(left), left.data()};
    destination.mBuffers[1] = {1, sizeof(right), right.data()};
    for (int block = 0; block < 10; ++block) {
        ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr,
                         state.get());
    }
    CHECK_FALSE(state->mBufferError.load());
    CHECK(left.back() > input.back());
    CHECK(right == input);
}

TEST_CASE("Core Audio silences invalid or oversized buffers without processing", "[coreaudio]")
{
    auto state = std::make_unique<CoreAudioState>();
    std::vector<float> physical(4, 1);
    Buffers<1> source;
    Buffers<1> destination;
    SECTION("missing input") {}
    SECTION("oversized input and output")
    {
        physical.resize((CoreAudioState::kMaximumFrames + 1) * 2, 1);
        source.mBuffers[0] = {2, static_cast<UInt32>(physical.size() * sizeof(float)),
                              physical.data()};
    }
    SECTION("mismatched frame counts")
    {
        source.mBuffers[0] = {2, sizeof(float) * 2, physical.data()};
    }
    destination.mBuffers[0] = {2, static_cast<UInt32>(physical.size() * sizeof(float)),
                               physical.data()};
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK(state->mBufferError.load());
    CHECK(state->mCallbacks.load() == 0);
    CHECK(std::all_of(physical.begin(), physical.end(), [](float value) { return value == 0; }));
}

TEST_CASE("Core Audio rejects partial PCM frames", "[coreaudio][validation]")
{
    auto state = std::make_unique<CoreAudioState>();
    std::array<float, 5> input{0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    std::array<float, 5> output{1, 1, 1, 1, 1};
    Buffers<1> source;
    source.mBuffers[0] = {2, sizeof(float) * 4, input.data()};
    Buffers<1> destination;
    destination.mBuffers[0] = {2, sizeof(float) * 4, output.data()};
    SECTION("input has a trailing byte") { ++source.mBuffers[0].mDataByteSize; }
    SECTION("output has a trailing byte") { ++destination.mBuffers[0].mDataByteSize; }
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK(state->mBufferError.load());
    CHECK(state->mCallbacks.load() == 0);
    CHECK(output[0] == 0);
    CHECK(output[3] == 0);
    CHECK(output[4] == 1);
}

TEST_CASE("Core Audio latches fatal buffer errors until restart", "[coreaudio][validation]")
{
    auto state = std::make_unique<CoreAudioState>();
    std::array<float, 4> input{0.1f, 0.2f, 0.3f, 0.4f};
    std::array<float, 4> output{1, 1, 1, 1};
    Buffers<1> source;
    source.mBuffers[0] = {2, sizeof(input), input.data()};
    Buffers<1> destination;
    destination.mBuffers[0] = {2, sizeof(output), output.data()};
    ProcessCoreAudio(0, nullptr, nullptr, nullptr, destination.List(), nullptr, state.get());
    REQUIRE(state->mBufferError.load());
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK(state->mCallbacks.load() == 0);
    CHECK(output == std::array<float, 4>{0, 0, 0, 0});
}

TEST_CASE("Core Audio rejects malformed channel counts without arithmetic overflow",
          "[coreaudio][validation]")
{
    auto state = std::make_unique<CoreAudioState>();
    std::array<float, 4> input{};
    std::array<float, 4> output{1, 1, 1, 1};
    Buffers<1> source;
    source.mBuffers[0] = {1u << 30, 0, input.data()};
    Buffers<1> destination;
    destination.mBuffers[0] = {2, sizeof(output), output.data()};
    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr, state.get());
    CHECK(state->mBufferError.load());
    CHECK(state->mCallbacks.load() == 0);
    CHECK(output == std::array<float, 4>{0, 0, 0, 0});
}

TEST_CASE("Core Audio remains stereo-isolated during concurrent control updates",
          "[coreaudio][stress]")
{
    auto state = std::make_unique<CoreAudioState>();
    state->mControl.mRightFocusMode.store(static_cast<uint8_t>(FocusMode::Off));
    std::array<float, 128> left, right, outputLeft{}, outputRight{};
    left.fill(0.01f);
    right.fill(0.02f);
    Buffers<2> source;
    source.mBuffers[0] = {1, sizeof(left), left.data()};
    source.mBuffers[1] = {1, sizeof(right), right.data()};
    Buffers<2> destination;
    destination.mBuffers[0] = {1, sizeof(outputLeft), outputLeft.data()};
    destination.mBuffers[1] = {1, sizeof(outputRight), outputRight.data()};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint32_t iteration = 0;
        while (!stop.load()) {
            state->mControl.mFocusEnabled.store(iteration % 2);
            state->mControl.mLeftFocusMode.store(iteration % 5);
            state->mControl.mLeftFocusLevel.store(1 + iteration % 3);
            ++iteration;
            std::this_thread::yield();
        }
    });
    bool valid = true;
    for (int block = 0; block < 5000; ++block) {
        ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(), nullptr,
                         state.get());
        valid = valid && outputRight == right &&
                std::all_of(outputLeft.begin(), outputLeft.end(),
                            [](float value) { return std::isfinite(value); });
    }
    stop.store(true);
    writer.join();
    CHECK(valid);
    CHECK_FALSE(state->mBufferError.load());
    CHECK(state->mCallbacks.load() == 5000);
}

TEST_CASE("Core Audio matches the portable processor across layouts rates and block boundaries",
          "[coreaudio][stress]")
{
    std::mt19937 random(0xEAA012);
    std::uniform_real_distribution<float> sample(-0.9f, 0.9f);
    for (bool planarInput : {false, true}) {
        for (bool planarOutput : {false, true}) {
            for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
                CAPTURE(planarInput, planarOutput, rate);
                auto state = std::make_unique<CoreAudioState>();
                state->mSampleRate = rate;
                EarTunerProcessor referenceProcessor;
                for (uint32_t frames :
                     {1u, 2u, 7u, 128u, 255u, 256u, 1024u, 4096u, 16383u, 16384u}) {
                    CAPTURE(frames);
                    state->mControl.mFocusEnabled.store(random() % 2);
                    state->mControl.mFocusBypassed.store(random() % 2);
                    state->mControl.mLeftFocusMode.store(random() % 5);
                    state->mControl.mRightFocusMode.store(random() % 5);
                    state->mControl.mLeftFocusLevel.store(1 + random() % 3);
                    state->mControl.mRightFocusLevel.store(1 + random() % 3);
                    std::vector<float> left(frames), right(frames), interleaved(frames * 2);
                    for (uint32_t frame = 0; frame < frames; ++frame) {
                        left[frame] = interleaved[frame * 2] = sample(random);
                        right[frame] = interleaved[frame * 2 + 1] = sample(random);
                    }

                    const auto originalInput = interleaved;
                    std::vector<float> expectedLeft(frames), expectedRight(frames);
                    constexpr uint32_t sampleBytes = sizeof(float);
                    ProcessAudioBlock(state->mControl, referenceProcessor, left.data(),
                                      right.data(), expectedLeft.data(), expectedRight.data(),
                                      frames, static_cast<float>(rate));
                    Buffers<2> source;
                    if (planarInput) {
                        source.mBuffers[0] = {1, frames * sampleBytes, left.data()};
                        source.mBuffers[1] = {1, frames * sampleBytes, right.data()};
                    } else {
                        source.mBuffers[0] = {2, frames * 2 * sampleBytes, interleaved.data()};
                        source.mNumberBuffers = 1;
                    }
                    std::vector<float> physical(frames * 2 + 2, 123);
                    Buffers<2> destination;
                    if (planarOutput) {
                        destination.mBuffers[0] = {1, frames * sampleBytes, physical.data() + 1};
                        destination.mBuffers[1] = {1, frames * sampleBytes,
                                                   physical.data() + 1 + frames};
                    } else {
                        destination.mBuffers[0] = {2, frames * 2 * sampleBytes,
                                                   physical.data() + 1};
                        destination.mNumberBuffers = 1;
                    }
                    ProcessCoreAudio(0, nullptr, source.List(), nullptr, destination.List(),
                                     nullptr, state.get());
                    REQUIRE_FALSE(state->mBufferError.load());
                    REQUIRE(interleaved == originalInput);
                    REQUIRE(physical.front() == 123);
                    REQUIRE(physical.back() == 123);
                    for (uint32_t frame = 0; frame < frames; ++frame) {
                        REQUIRE(physical[1 + (planarOutput ? frame : frame * 2)] ==
                                expectedLeft[frame]);
                        REQUIRE(physical[1 + (planarOutput ? frames + frame : frame * 2 + 1)] ==
                                expectedRight[frame]);
                    }
                }
            }
        }
    }
}
