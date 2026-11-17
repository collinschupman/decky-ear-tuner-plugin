#include "platform/windows/audio_bridge.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

TEST_CASE("Windows bridge preserves dry stereo samples and ring ordering", "[windows][audio]")
{
    auto bridge = std::make_unique<WindowsAudioBridge>();
    std::vector<float> input(2880);
    for (size_t i = 0; i < input.size() / 2; ++i) {
        input[i * 2] = static_cast<float>(i) / 10000;
        input[i * 2 + 1] = -input[i * 2];
    }
    REQUIRE(bridge->Capture(input.data(), 1440));
    std::vector<float> output(960);
    REQUIRE(bridge->Render(output.data(), 480));
    REQUIRE(std::equal(output.begin(), output.end(), input.begin()));
    REQUIRE(bridge->mRenderRate.load() == 48000);
    for (int i = 0; i < 500; ++i) {
        REQUIRE(bridge->Capture(nullptr, 480));
        REQUIRE(bridge->Render(output.data(), 480));
        if (i < 2) {
            REQUIRE(std::equal(output.begin(), output.end(), input.begin() + (i + 1) * 960));
        } else {
            REQUIRE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
        }
    }
    REQUIRE(bridge->mUnderruns.load() == 0);
    REQUIRE(bridge->mOverflows.load() == 0);
}

TEST_CASE("Windows bridge uses the shared DSP without swapping channels", "[windows][audio]")
{
    auto bridge = std::make_unique<WindowsAudioBridge>();
    bridge->mControl.mFocusEnabled.store(true);
    bridge->mControl.mLeftFocusMode.store(1);
    bridge->mControl.mRightFocusMode.store(3);
    bridge->mControl.mLeftFocusLevel.store(2);
    bridge->mControl.mRightFocusLevel.store(3);
    std::vector<float> left(1440), right(1440), input(2880), output(960);
    for (size_t i = 0; i < left.size(); ++i) {
        left[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.1f;
        right[i] = std::cos(static_cast<float>(i) * 0.2f) * 0.1f;
        input[i * 2] = left[i];
        input[i * 2 + 1] = right[i];
    }
    EarTunerProcessor reference;
    ProcessAudioBlock(bridge->mControl, reference, left.data(), right.data(), left.data(),
                      right.data(), 1440, 48000);
    REQUIRE(bridge->Capture(input.data(), 1440));
    REQUIRE(bridge->Render(output.data(), 480));
    for (size_t i = 0; i < 480; ++i) {
        REQUIRE(output[i * 2] == left[i]);
        REQUIRE(output[i * 2 + 1] == right[i]);
    }
}

TEST_CASE("Windows bridge supports bounded frame sizes with output guards", "[windows][audio]")
{
    for (uint32_t frames : {1u, 17u, 64u, 480u, 4096u, WindowsAudioBridge::kMaxFrames}) {
        auto bridge = std::make_unique<WindowsAudioBridge>();
        uint32_t remaining = frames + WindowsAudioBridge::kTargetFrames;
        while (remaining) {
            const auto chunk = std::min(remaining, WindowsAudioBridge::kMaxFrames);
            REQUIRE(bridge->Capture(nullptr, chunk));
            remaining -= chunk;
        }
        std::vector<float> output(frames * 2 + 2, 17.0f);
        REQUIRE(bridge->Render(output.data() + 1, frames));
        REQUIRE(output.front() == 17);
        REQUIRE(output.back() == 17);
        REQUIRE(std::all_of(output.begin() + 1, output.end() - 1, [](float x) { return x == 0; }));
    }
}

TEST_CASE("Windows bridge primes and recovers after queue discontinuities", "[windows][audio]")
{
    auto bridge = std::make_unique<WindowsAudioBridge>();
    std::vector<float> output(WindowsAudioBridge::kMaxFrames * 2, 17.0f);
    REQUIRE(bridge->Render(output.data(), 480));
    REQUIRE(bridge->mUnderruns.load() == 0);
    REQUIRE(bridge->Capture(nullptr, 1440));
    REQUIRE(bridge->Render(output.data(), 480));
    REQUIRE(bridge->Render(output.data(), WindowsAudioBridge::kMaxFrames));
    REQUIRE(bridge->mUnderruns.load() == 1);
    REQUIRE(bridge->mBufferedFrames.load() == 0);
    REQUIRE(bridge->Capture(nullptr, WindowsAudioBridge::kMaxFrames));
    REQUIRE(bridge->Capture(nullptr, WindowsAudioBridge::kMaxFrames));
    REQUIRE(bridge->Capture(nullptr, 480));
    REQUIRE(bridge->mOverflows.load() == 1);
    REQUIRE(bridge->mBufferedFrames.load() == 480);
    bridge->Discontinuity();
    REQUIRE(bridge->mDiscontinuities.load() == 1);
    REQUIRE(bridge->mBufferedFrames.load() == 0);
    REQUIRE(bridge->Capture(nullptr, 1440));
    REQUIRE(bridge->Render(output.data(), 480));
    REQUIRE_FALSE(bridge->mBufferError.load());
}

TEST_CASE("Windows bridge latches invalid buffer metadata until restart", "[windows][audio]")
{
    for (uint32_t frames :
         {0u, WindowsAudioBridge::kMaxFrames + 1, std::numeric_limits<uint32_t>::max()}) {
        auto bridge = std::make_unique<WindowsAudioBridge>();
        float guard = 17.0f;
        REQUIRE_FALSE(bridge->Capture(&guard, frames));
        REQUIRE(guard == 17);
        REQUIRE(bridge->mBufferError.load());
        std::vector<float> output(960, 17.0f);
        REQUIRE_FALSE(bridge->Capture(nullptr, 1440));
        REQUIRE_FALSE(bridge->Render(output.data(), 480));
        REQUIRE(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
    }
    auto bridge = std::make_unique<WindowsAudioBridge>();
    REQUIRE_FALSE(bridge->Render(nullptr, 480));
    REQUIRE(bridge->mBufferError.load());
}

TEST_CASE("Windows clock controller absorbs ten minutes of device drift",
          "[windows][audio][stress]")
{
    for (double ppm : {-1000.0, 0.0, 1000.0}) {
        auto bridge = std::make_unique<WindowsAudioBridge>();
        std::vector<float> output(2048);
        REQUIRE(bridge->Capture(nullptr, 1440));
        REQUIRE(bridge->Render(output.data(), 480));
        double inputPhase = 0, outputPhase = 0;
        float appliedRate = 48000;
        for (int tick = 0; tick < 60000; ++tick) {
            inputPhase += 480 * (1 + ppm / 1000000);
            const auto captured = static_cast<uint32_t>(inputPhase);
            inputPhase -= captured;
            outputPhase += appliedRate * 0.01;
            const auto rendered = static_cast<uint32_t>(outputPhase);
            outputPhase -= rendered;
            REQUIRE(bridge->Capture(nullptr, captured));
            REQUIRE(bridge->Render(output.data(), rendered));
            if (tick % 25 == 0) {
                appliedRate = bridge->mRenderRate.load();
            }
            REQUIRE(bridge->mBufferedFrames.load() < 1500);
        }
        REQUIRE(bridge->mUnderruns.load() == 0);
        REQUIRE(bridge->mOverflows.load() == 0);
        REQUIRE(bridge->mBufferedFrames.load() > 600);
        REQUIRE(appliedRate == Catch::Approx(48000 * (1 + ppm / 1000000)).margin(2));
    }
}
