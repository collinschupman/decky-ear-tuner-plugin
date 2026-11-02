#include <catch2/catch_test_macros.hpp>

#include "platform/linux/pipewire/pipewire_filter.h"

#include <spa/param/audio/format-utils.h>

#include <array>
#include <csignal>
#include <memory>

namespace {

class PipeWireRuntime {
   public:
    PipeWireRuntime()
    {
        int argumentCount = 0;
        char **argumentValues = nullptr;
        pw_init(&argumentCount, &argumentValues);
        mLoop = pw_main_loop_new(nullptr);
    }

    ~PipeWireRuntime()
    {
        if (mLoop) {
            pw_main_loop_destroy(mLoop);
        }
        pw_deinit();
    }

    pw_main_loop *Loop() const { return mLoop; }

   private:
    pw_main_loop *mLoop = nullptr;
};

void RequestSignalShutdown(void *userData) { QuitMainLoop(userData, SIGTERM); }

void RequestFilterErrorShutdown(void *userData)
{
    StreamStateChanged(userData, PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_ERROR,
                       "simulated stream error");
}

void RequestFilterDisconnectShutdown(void *userData)
{
    StreamStateChanged(userData, PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_UNCONNECTED, nullptr);
}

}  // namespace

TEST_CASE("PipeWire filter event table registers realtime callbacks")
{
    void (*const outputCallback)(void *) = ProcessAudio;
    void (*const ingressCallback)(void *) = ProcessIngress;
    REQUIRE(kOutputStreamEvents.process == outputCallback);
    REQUIRE(kOutputStreamEvents.state_changed == StreamStateChanged);
    REQUIRE(kIngressStreamEvents.process == ingressCallback);
    REQUIRE(kIngressStreamEvents.state_changed == StreamStateChanged);
    REQUIRE(kIngressStreamEvents.param_changed == IngressParamChanged);
}

TEST_CASE("PipeWire ingress tracks negotiated sample rates and format removal")
{
    AudioState state;
    CHECK(state.mIngressSampleRate.load() == 0);

    for (const uint32_t rate : {44100u, 48000u, 96000u}) {
        std::array<uint8_t, 256> buffer{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
        spa_audio_info_raw format{};
        format.format = SPA_AUDIO_FORMAT_F32P;
        format.channels = 2;
        format.rate = rate;
        const spa_pod *param = spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &format);

        kIngressStreamEvents.param_changed(&state, SPA_PARAM_Format, param);
        CHECK(state.mIngressSampleRate.load() == rate);
        kIngressStreamEvents.param_changed(&state, SPA_PARAM_Buffers, nullptr);
        CHECK(state.mIngressSampleRate.load() == rate);
    }

    kIngressStreamEvents.param_changed(&state, SPA_PARAM_Format, nullptr);
    CHECK(state.mIngressSampleRate.load() == 0);
}

TEST_CASE("PipeWire ingress waits for a negotiated format before accessing the stream")
{
    AudioState state;
    ProcessIngress(&state);
}

TEST_CASE("PipeWire ingress honors stereo chunk offsets and clamps sizes to capacity")
{
    AudioState state;
    std::array<float, 4> left{0.1f, 0.2f, 0.3f, 0.4f};
    std::array<float, 4> right{-0.1f, -0.2f, -0.3f, -0.4f};
    std::array<spa_chunk, 2> chunks{};
    std::array<spa_data, 2> planes{};
    for (uint32_t channel = 0; channel < 2; ++channel) {
        planes[channel].data = channel == 0 ? left.data() : right.data();
        planes[channel].maxsize = sizeof(left);
        planes[channel].chunk = &chunks[channel];
        chunks[channel].size = sizeof(left) * 2;
        chunks[channel].stride = sizeof(float);
    }
    chunks[0].offset = sizeof(float) * 6;
    chunks[1].offset = sizeof(float);
    spa_buffer buffer{};
    buffer.n_datas = planes.size();
    buffer.datas = planes.data();
    REQUIRE(ProcessIngressBuffer(state, buffer, 44100));
    std::array<float, 4> outputLeft{};
    std::array<float, 4> outputRight{};
    REQUIRE(ReadProcessedAudio(state, outputLeft.data(), outputRight.data(), outputLeft.size()));
    CHECK(outputLeft == std::array<float, 4>{0.3f, 0.4f, 0.1f, 0.2f});
    CHECK(outputRight == std::array<float, 4>{-0.2f, -0.3f, -0.4f, -0.1f});

    chunks[0].size = sizeof(float) * 2;
    chunks[1].flags = SPA_CHUNK_FLAG_EMPTY;
    planes[1].data = nullptr;
    REQUIRE(ProcessIngressBuffer(state, buffer, 48000));
    std::array<float, 2> shortLeft{};
    std::array<float, 2> shortRight{};
    REQUIRE(ReadProcessedAudio(state, shortLeft.data(), shortRight.data(), shortLeft.size()));
    CHECK(shortLeft == std::array<float, 2>{0.3f, 0.4f});
    CHECK(shortRight == std::array<float, 2>{});

    chunks[0].offset = 1;
    CHECK_FALSE(ProcessIngressBuffer(state, buffer, 48000));
    chunks[0].offset = 0;
    planes[0].maxsize = 0;
    CHECK_FALSE(ProcessIngressBuffer(state, buffer, 48000));
    planes[0].maxsize = sizeof(left);
    planes[0].chunk = nullptr;
    CHECK_FALSE(ProcessIngressBuffer(state, buffer, 48000));
}

TEST_CASE("PipeWire ingress rejects formats without a sample rate")
{
    PipeWireRuntime runtime;
    REQUIRE(runtime.Loop() != nullptr);
    AudioState state;
    state.mLoop = runtime.Loop();
    state.mIngressSampleRate.store(48000);

    REQUIRE(pw_loop_add_idle(
                pw_main_loop_get_loop(runtime.Loop()), true,
                [](void *userData) {
                    std::array<uint8_t, 256> buffer{};
                    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
                    spa_audio_info_raw format{};
                    format.format = SPA_AUDIO_FORMAT_F32P;
                    format.channels = 2;
                    const spa_pod *param =
                        spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &format);
                    kIngressStreamEvents.param_changed(userData, SPA_PARAM_Format, param);
                },
                &state) != nullptr);

    REQUIRE(pw_main_loop_run(runtime.Loop()) >= 0);
    CHECK(state.mIngressSampleRate.load() == 0);
}

TEST_CASE("PipeWire streams expose an output-agnostic WirePlumber smart-filter pair")
{
    const std::unique_ptr<pw_properties, decltype(&pw_properties_free)> ingress(
        CreateIngressStreamProperties(), pw_properties_free);
    const std::unique_ptr<pw_properties, decltype(&pw_properties_free)> output(
        CreateOutputStreamProperties(), pw_properties_free);

    REQUIRE(ingress != nullptr);
    REQUIRE(output != nullptr);
    CHECK(std::string(pw_properties_get(ingress.get(), PW_KEY_MEDIA_CLASS)) == "Audio/Sink");
    CHECK(std::string(pw_properties_get(ingress.get(), "filter.smart")) == "true");
    CHECK(pw_properties_get(ingress.get(), "filter.smart.target") == nullptr);
    CHECK(std::string(pw_properties_get(ingress.get(), PW_KEY_NODE_LINK_GROUP)) == "ear-tuner");
    CHECK(std::string(pw_properties_get(output.get(), PW_KEY_MEDIA_CLASS)) ==
          "Stream/Output/Audio");
    CHECK(std::string(pw_properties_get(output.get(), PW_KEY_NODE_LINK_GROUP)) == "ear-tuner");
}

TEST_CASE("DSP output uses automatic graph scheduling")
{
    const std::unique_ptr<pw_properties, decltype(&pw_properties_free)> output(
        CreateOutputStreamProperties(), pw_properties_free);

    REQUIRE(output != nullptr);
    CHECK(std::string(pw_properties_get(output.get(), PW_KEY_NODE_PASSIVE)) == "false");
    CHECK(pw_properties_get(output.get(), PW_KEY_NODE_TRIGGER) == nullptr);
}

TEST_CASE("Processed stereo audio crosses the ring buffers unchanged")
{
    AudioState state;
    const std::array<float, 4> inputLeft = {0.1f, 0.2f, 0.3f, 0.4f};
    const std::array<float, 4> inputRight = {-0.1f, -0.2f, -0.3f, -0.4f};
    std::array<float, 4> outputLeft{};
    std::array<float, 4> outputRight{};

    WriteProcessedAudio(state, inputLeft.data(), inputRight.data(), inputLeft.size());

    REQUIRE(ReadProcessedAudio(state, outputLeft.data(), outputRight.data(), outputLeft.size()));
    CHECK(outputLeft == inputLeft);
    CHECK(outputRight == inputRight);
}

TEST_CASE("Processed audio underruns produce stereo silence")
{
    AudioState state;
    std::array<float, 4> outputLeft = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> outputRight = {1.0f, 1.0f, 1.0f, 1.0f};

    CHECK_FALSE(
        ReadProcessedAudio(state, outputLeft.data(), outputRight.data(), outputLeft.size()));
    CHECK(state.mRingUnderrunCount.load() == 1);
    CHECK(outputLeft == std::array<float, 4>{});
    CHECK(outputRight == std::array<float, 4>{});
}

TEST_CASE("Processed audio overflow discards stale buffered frames")
{
    AudioState state;
    std::array<float, AudioState::kBufferSampleCount> staleLeft{};
    std::array<float, AudioState::kBufferSampleCount> staleRight{};
    staleLeft.fill(1.0f);
    staleRight.fill(-1.0f);
    WriteProcessedAudio(state, staleLeft.data(), staleRight.data(), staleLeft.size());

    const std::array<float, 2> freshLeft = {0.25f, 0.5f};
    const std::array<float, 2> freshRight = {-0.25f, -0.5f};
    std::array<float, 2> outputLeft{};
    std::array<float, 2> outputRight{};
    WriteProcessedAudio(state, freshLeft.data(), freshRight.data(), freshLeft.size());

    CHECK(state.mRingOverflowCount.load() == 1);

    REQUIRE(ReadProcessedAudio(state, outputLeft.data(), outputRight.data(), outputLeft.size()));
    CHECK(outputLeft == freshLeft);
    CHECK(outputRight == freshRight);
}

TEST_CASE("Audio diagnostics serialize callback telemetry")
{
    AudioState state;
    state.mIngressSampleRate.store(48000);
    state.mIngressCallbackCount.store(12);
    state.mIngressDeadlineMissCount.store(1);
    state.mIngressMaximumNanoseconds.store(25000);
    state.mRingUnderrunCount.store(2);

    const std::string diagnostics = AudioDiagnosticsJson(state);

    CHECK(diagnostics.find("\"sample_rate\":48000") != std::string::npos);
    CHECK(diagnostics.find("\"callbacks\":12") != std::string::npos);
    CHECK(diagnostics.find("\"deadline_misses\":1") != std::string::npos);
    CHECK(diagnostics.find("\"maximum_nanoseconds\":25000") != std::string::npos);
    CHECK(diagnostics.find("\"underruns\":2") != std::string::npos);
}

TEST_CASE("PipeWire signal callback requests main-loop shutdown")
{
    PipeWireRuntime runtime;
    REQUIRE(runtime.Loop() != nullptr);
    AudioState state;
    state.mLoop = runtime.Loop();

    REQUIRE(pw_loop_add_idle(pw_main_loop_get_loop(runtime.Loop()), true, RequestSignalShutdown,
                             &state) != nullptr);

    REQUIRE(pw_main_loop_run(runtime.Loop()) >= 0);
}

TEST_CASE("PipeWire filter errors request main-loop shutdown")
{
    PipeWireRuntime runtime;
    REQUIRE(runtime.Loop() != nullptr);
    AudioState state;
    state.mLoop = runtime.Loop();

    REQUIRE(pw_loop_add_idle(pw_main_loop_get_loop(runtime.Loop()), true,
                             RequestFilterErrorShutdown, &state) != nullptr);

    REQUIRE(pw_main_loop_run(runtime.Loop()) >= 0);
}

TEST_CASE("PipeWire stream disconnects request main-loop shutdown")
{
    PipeWireRuntime runtime;
    REQUIRE(runtime.Loop() != nullptr);
    AudioState state;
    state.mLoop = runtime.Loop();

    REQUIRE(pw_loop_add_idle(pw_main_loop_get_loop(runtime.Loop()), true,
                             RequestFilterDisconnectShutdown, &state) != nullptr);

    REQUIRE(pw_main_loop_run(runtime.Loop()) >= 0);
}
