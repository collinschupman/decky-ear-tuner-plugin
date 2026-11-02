#include "platform/linux/pipewire/pipewire_filter.h"
#include "platform/posix/http/http_server.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

void DestroyStreams(AudioState &state)
{
    if (state.mIngressStream) {
        pw_stream_destroy(state.mIngressStream);
    }
    if (state.mOutputStream) {
        pw_stream_destroy(state.mOutputStream);
    }
    if (state.mCore) {
        pw_core_disconnect(state.mCore);
    }
    if (state.mContext) {
        pw_context_destroy(state.mContext);
    }
    if (state.mLoop) {
        pw_main_loop_destroy(state.mLoop);
    }
    pw_deinit();
}

}  // namespace

int main(int argc, char *argv[])
{
    pw_init(&argc, &argv);

    AudioState state;
    state.mLoop = pw_main_loop_new(nullptr);
    if (!state.mLoop) {
        std::cerr << "pw_main_loop_new failed\n";
        pw_deinit();
        return 1;
    }

    pw_loop_add_signal(pw_main_loop_get_loop(state.mLoop), SIGINT, QuitMainLoop, &state);
    pw_loop_add_signal(pw_main_loop_get_loop(state.mLoop), SIGTERM, QuitMainLoop, &state);

    state.mContext = pw_context_new(pw_main_loop_get_loop(state.mLoop), nullptr, 0);
    if (!state.mContext) {
        std::cerr << "pw_context_new failed\n";
        DestroyStreams(state);
        return 1;
    }
    state.mCore = pw_context_connect(state.mContext, nullptr, 0);
    if (!state.mCore) {
        std::cerr << "pw_context_connect failed\n";
        DestroyStreams(state);
        return 1;
    }

    state.mOutputStream =
        pw_stream_new(state.mCore, "ear-tuner-output", CreateOutputStreamProperties());
    if (!state.mOutputStream) {
        std::cerr << "pw_stream_new output failed\n";
        DestroyStreams(state);
        return 1;
    }
    pw_stream_add_listener(state.mOutputStream, &state.mOutputListener, &kOutputStreamEvents,
                           &state);

    state.mIngressStream = pw_stream_new(state.mCore, "ear-tuner", CreateIngressStreamProperties());
    if (!state.mIngressStream) {
        std::cerr << "pw_stream_new ingress failed\n";
        DestroyStreams(state);
        return 1;
    }
    pw_stream_add_listener(state.mIngressStream, &state.mIngressListener, &kIngressStreamEvents,
                           &state);

    std::array<uint8_t, 256> formatBuffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(formatBuffer.data(), formatBuffer.size());
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32P;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
    };
    constexpr auto streamFlags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(state.mIngressStream, PW_DIRECTION_INPUT, PW_ID_ANY, streamFlags, params,
                          1) < 0) {
        std::cerr << "pw_stream_connect ingress failed: " << std::strerror(errno) << "\n";
        DestroyStreams(state);
        return 1;
    }
    builder = SPA_POD_BUILDER_INIT(formatBuffer.data(), formatBuffer.size());
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    if (pw_stream_connect(state.mOutputStream, PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, params,
                          1) < 0) {
        std::cerr << "pw_stream_connect output failed: " << std::strerror(errno) << "\n";
        DestroyStreams(state);
        return 1;
    }

    PosixHttpServerState httpState;
    httpState.mDiagnostics = [&state] { return AudioDiagnosticsJson(state); };
    std::thread httpThread(RunHttpServer, std::ref(state.mControl), std::ref(httpState));
    pw_main_loop_run(state.mLoop);

    StopHttpServer(httpState);
    if (httpThread.joinable()) {
        httpThread.join();
    }

    DestroyStreams(state);
    return 0;
}
