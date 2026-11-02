#include "platform/linux/pipewire/pipewire_filter.h"

#include "ear_tuner/audio_block_processor.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <algorithm>
#include <ctime>
#include <iostream>
#include <sstream>

namespace {

uint64_t MonotonicNanoseconds() EAR_TUNER_NONBLOCKING
{
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return static_cast<uint64_t>(timestamp.tv_sec) * 1000000000u +
           static_cast<uint64_t>(timestamp.tv_nsec);
}

void UpdateMaximum(std::atomic<uint32_t> &maximum, uint32_t value) EAR_TUNER_NONBLOCKING
{
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    while (previous < value &&
           !maximum.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

void RecordCallback(std::atomic<uint32_t> &callbackCount, std::atomic<uint32_t> &deadlineMissCount,
                    std::atomic<uint32_t> &maximumNanoseconds, uint64_t startedNanoseconds,
                    uint32_t sampleCount, uint32_t sampleRate) EAR_TUNER_NONBLOCKING
{
    const uint64_t elapsed = MonotonicNanoseconds() - startedNanoseconds;
    const uint32_t boundedElapsed = static_cast<uint32_t>(std::min<uint64_t>(elapsed, UINT32_MAX));
    callbackCount.fetch_add(1, std::memory_order_relaxed);
    UpdateMaximum(maximumNanoseconds, boundedElapsed);
    if (sampleCount != 0 && sampleRate != 0 &&
        elapsed * sampleRate > static_cast<uint64_t>(sampleCount) * 1000000000u) {
        deadlineMissCount.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

std::string AudioDiagnosticsJson(const AudioState &state)
{
    std::ostringstream json;
    json << "{\"sample_rate\":" << state.mIngressSampleRate.load(std::memory_order_relaxed)
         << ",\"ingress\":{\"callbacks\":"
         << state.mIngressCallbackCount.load(std::memory_order_relaxed) << ",\"deadline_misses\":"
         << state.mIngressDeadlineMissCount.load(std::memory_order_relaxed)
         << ",\"maximum_nanoseconds\":"
         << state.mIngressMaximumNanoseconds.load(std::memory_order_relaxed)
         << "},\"output\":{\"callbacks\":"
         << state.mOutputCallbackCount.load(std::memory_order_relaxed) << ",\"deadline_misses\":"
         << state.mOutputDeadlineMissCount.load(std::memory_order_relaxed)
         << ",\"maximum_nanoseconds\":"
         << state.mOutputMaximumNanoseconds.load(std::memory_order_relaxed)
         << "},\"ring\":{\"underruns\":" << state.mRingUnderrunCount.load(std::memory_order_relaxed)
         << ",\"overflows\":" << state.mRingOverflowCount.load(std::memory_order_relaxed) << "}}";
    return json.str();
}

pw_properties *CreateIngressStreamProperties()
{
    return pw_properties_new(PW_KEY_NODE_NAME, "ear-tuner", PW_KEY_NODE_DESCRIPTION, "Ear Tuner",
                             PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_GROUP, "ear-tuner",
                             PW_KEY_NODE_LINK_GROUP, "ear-tuner", PW_KEY_NODE_AUTOCONNECT, "true",
                             "filter.smart", "true", "filter.smart.name", "ear-tuner",
                             PW_KEY_NODE_VIRTUAL, "true", PW_KEY_STREAM_IS_LIVE, "true", nullptr);
}

pw_properties *CreateOutputStreamProperties()
{
    return pw_properties_new(PW_KEY_NODE_NAME, "ear-tuner-output", PW_KEY_NODE_DESCRIPTION,
                             "Ear Tuner DSP Output", PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
                             PW_KEY_NODE_GROUP, "ear-tuner", PW_KEY_NODE_LINK_GROUP, "ear-tuner",
                             PW_KEY_NODE_PASSIVE, "false", PW_KEY_NODE_VIRTUAL, "true",
                             PW_KEY_NODE_AUTOCONNECT, "true", PW_KEY_STREAM_DONT_REMIX, "true",
                             PW_KEY_STREAM_IS_LIVE, "true", nullptr);
}

void WriteProcessedAudio(AudioState &state, const float *left, const float *right,
                         uint32_t sampleCount)
{
    const uint32_t byteCount = sampleCount * sizeof(float);
    constexpr uint32_t ringSize = AudioState::kBufferSampleCount * sizeof(float);
    uint32_t leftIndex = 0;
    uint32_t rightIndex = 0;
    const int32_t leftFill = spa_ringbuffer_get_write_index(&state.mLeftRing, &leftIndex);
    const int32_t rightFill = spa_ringbuffer_get_write_index(&state.mRightRing, &rightIndex);
    if (leftFill < 0 || rightFill < 0 || leftFill > static_cast<int32_t>(ringSize - byteCount) ||
        rightFill > static_cast<int32_t>(ringSize - byteCount)) {
        state.mRingOverflowCount.fetch_add(1, std::memory_order_relaxed);
        spa_ringbuffer_init(&state.mLeftRing);
        spa_ringbuffer_init(&state.mRightRing);
        leftIndex = 0;
        rightIndex = 0;
    }
    spa_ringbuffer_write_data(&state.mLeftRing, state.mLeftBuffer.data(), ringSize,
                              leftIndex & (ringSize - 1), left, byteCount);
    spa_ringbuffer_write_data(&state.mRightRing, state.mRightBuffer.data(), ringSize,
                              rightIndex & (ringSize - 1), right, byteCount);
    spa_ringbuffer_write_update(&state.mLeftRing, leftIndex + byteCount);
    spa_ringbuffer_write_update(&state.mRightRing, rightIndex + byteCount);
}

bool ReadProcessedAudio(AudioState &state, float *left, float *right, uint32_t sampleCount)
{
    const uint32_t byteCount = sampleCount * sizeof(float);
    constexpr uint32_t ringSize = AudioState::kBufferSampleCount * sizeof(float);
    uint32_t leftIndex = 0;
    uint32_t rightIndex = 0;
    const int32_t leftAvailable = spa_ringbuffer_get_read_index(&state.mLeftRing, &leftIndex);
    const int32_t rightAvailable = spa_ringbuffer_get_read_index(&state.mRightRing, &rightIndex);
    if (leftAvailable < static_cast<int32_t>(byteCount) ||
        rightAvailable < static_cast<int32_t>(byteCount)) {
        state.mRingUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        std::fill_n(left, sampleCount, 0.0f);
        std::fill_n(right, sampleCount, 0.0f);
        return false;
    }
    spa_ringbuffer_read_data(&state.mLeftRing, state.mLeftBuffer.data(), ringSize,
                             leftIndex & (ringSize - 1), left, byteCount);
    spa_ringbuffer_read_data(&state.mRightRing, state.mRightBuffer.data(), ringSize,
                             rightIndex & (ringSize - 1), right, byteCount);
    spa_ringbuffer_read_update(&state.mLeftRing, leftIndex + byteCount);
    spa_ringbuffer_read_update(&state.mRightRing, rightIndex + byteCount);
    return true;
}

void ProcessAudio(void *userData) EAR_TUNER_NONBLOCKING
{
    auto *state = static_cast<AudioState *>(userData);
    const uint64_t startedNanoseconds = MonotonicNanoseconds();
    pw_buffer *pipeWireBuffer = pw_stream_dequeue_buffer(state->mOutputStream);
    if (!pipeWireBuffer) {
        return;
    }
    if (!pipeWireBuffer->buffer || pipeWireBuffer->buffer->n_datas < 2 ||
        !pipeWireBuffer->buffer->datas) {
        pw_log_trace("Ear Tuner: invalid output buffer");
        pw_stream_queue_buffer(state->mOutputStream, pipeWireBuffer);
        return;
    }
    spa_buffer *buffer = pipeWireBuffer->buffer;
    auto *outputLeft = static_cast<float *>(buffer->datas[0].data);
    auto *outputRight = static_cast<float *>(buffer->datas[1].data);
    if (!outputLeft || !outputRight || !buffer->datas[0].chunk || !buffer->datas[1].chunk) {
        pw_log_trace("Ear Tuner: missing output plane or chunk");
        pw_stream_queue_buffer(state->mOutputStream, pipeWireBuffer);
        return;
    }
    const uint32_t capacity =
        std::min(buffer->datas[0].maxsize, buffer->datas[1].maxsize) / sizeof(float);
    const uint32_t requested =
        pipeWireBuffer->requested ? static_cast<uint32_t>(pipeWireBuffer->requested) : capacity;
    const uint32_t sampleCount = std::min(requested, capacity);

    const uint32_t byteCount = sampleCount * sizeof(float);
    ReadProcessedAudio(*state, outputLeft, outputRight, sampleCount);
    for (uint32_t index = 0; index < 2; ++index) {
        buffer->datas[index].chunk->offset = 0;
        buffer->datas[index].chunk->stride = sizeof(float);
        buffer->datas[index].chunk->size = byteCount;
    }
    pw_stream_queue_buffer(state->mOutputStream, pipeWireBuffer);
    const uint32_t sampleRate = state->mIngressSampleRate.load(std::memory_order_relaxed);
    RecordCallback(state->mOutputCallbackCount, state->mOutputDeadlineMissCount,
                   state->mOutputMaximumNanoseconds, startedNanoseconds, sampleCount, sampleRate);
}

void ProcessIngress(void *userData) EAR_TUNER_NONBLOCKING
{
    auto *state = static_cast<AudioState *>(userData);
    const uint32_t sampleRate = state->mIngressSampleRate.load(std::memory_order_relaxed);
    if (sampleRate == 0) {
        return;
    }
    pw_buffer *pipeWireBuffer = pw_stream_dequeue_buffer(state->mIngressStream);
    if (!pipeWireBuffer) {
        return;
    }
    const uint64_t startedNanoseconds = MonotonicNanoseconds();
    uint32_t sampleCount = 0;
    if (pipeWireBuffer->buffer) {
        sampleCount = ProcessIngressBuffer(*state, *pipeWireBuffer->buffer, sampleRate);
    } else {
        pw_log_trace("Ear Tuner: missing ingress buffer");
    }
    pw_stream_queue_buffer(state->mIngressStream, pipeWireBuffer);
    RecordCallback(state->mIngressCallbackCount, state->mIngressDeadlineMissCount,
                   state->mIngressMaximumNanoseconds, startedNanoseconds, sampleCount, sampleRate);
}

uint32_t ProcessIngressBuffer(AudioState &state, const spa_buffer &buffer,
                              uint32_t sampleRate) EAR_TUNER_NONBLOCKING
{
    if (sampleRate == 0 || buffer.n_datas < 2 || !buffer.datas) {
        pw_log_trace("Ear Tuner: invalid ingress format or planes");
        return 0;
    }
    for (uint32_t channel = 0; channel < 2; ++channel) {
        const spa_data &data = buffer.datas[channel];
        if (!data.chunk || data.maxsize < sizeof(float) || data.maxsize % sizeof(float) != 0 ||
            data.chunk->offset % sizeof(float) != 0 ||
            (data.chunk->stride != 0 && data.chunk->stride != sizeof(float)) ||
            (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) ||
            (!data.data && !(data.chunk->flags & SPA_CHUNK_FLAG_EMPTY))) {
            pw_log_trace("Ear Tuner: invalid ingress chunk");
            return 0;
        }
        if (data.chunk->size > data.maxsize) {
            pw_log_trace("Ear Tuner: clamping oversized ingress chunk");
        }
    }
    const spa_data &left = buffer.datas[0];
    const spa_data &right = buffer.datas[1];
    const uint32_t leftCapacity = left.maxsize / sizeof(float);
    const uint32_t rightCapacity = right.maxsize / sizeof(float);
    const uint32_t sampleCount =
        std::min({std::min(left.chunk->size, left.maxsize) / static_cast<uint32_t>(sizeof(float)),
                  std::min(right.chunk->size, right.maxsize) / static_cast<uint32_t>(sizeof(float)),
                  AudioState::kBufferSampleCount});
    uint32_t processed = 0;
    while (processed < sampleCount) {
        const uint32_t leftOffset = (left.chunk->offset / sizeof(float) + processed) % leftCapacity;
        const uint32_t rightOffset =
            (right.chunk->offset / sizeof(float) + processed) % rightCapacity;
        const uint32_t count = std::min(
            {sampleCount - processed, leftCapacity - leftOffset, rightCapacity - rightOffset});
        const auto *inputLeft = left.chunk->flags & SPA_CHUNK_FLAG_EMPTY
                                    ? nullptr
                                    : static_cast<const float *>(left.data) + leftOffset;
        const auto *inputRight = right.chunk->flags & SPA_CHUNK_FLAG_EMPTY
                                     ? nullptr
                                     : static_cast<const float *>(right.data) + rightOffset;
        ProcessAudioBlock(state.mControl, state.mProcessor, inputLeft, inputRight,
                          state.mProcessedLeft.data() + processed,
                          state.mProcessedRight.data() + processed, count,
                          static_cast<float>(sampleRate));
        processed += count;
    }
    WriteProcessedAudio(state, state.mProcessedLeft.data(), state.mProcessedRight.data(),
                        sampleCount);
    return sampleCount;
}

void IngressParamChanged(void *userData, uint32_t id, const spa_pod *param)
{
    if (id != SPA_PARAM_Format) {
        return;
    }
    auto *state = static_cast<AudioState *>(userData);
    state->mIngressSampleRate.store(0, std::memory_order_relaxed);
    if (!param) {
        return;
    }

    uint32_t mediaType = 0;
    uint32_t mediaSubtype = 0;
    spa_audio_info_raw format{};
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
        mediaType != SPA_MEDIA_TYPE_audio || mediaSubtype != SPA_MEDIA_SUBTYPE_raw ||
        spa_format_audio_raw_parse(param, &format) < 0 || format.format != SPA_AUDIO_FORMAT_F32P ||
        format.channels != 2 || format.rate == 0) {
        std::cerr << "Invalid PipeWire ingress audio format\n";
        pw_main_loop_quit(state->mLoop);
        return;
    }
    state->mIngressSampleRate.store(format.rate, std::memory_order_relaxed);
}

void StreamStateChanged(void *userData, pw_stream_state oldState, pw_stream_state state,
                        const char *error)
{
    auto *audio = static_cast<AudioState *>(userData);
    std::cout << "PipeWire stream state: " << pw_stream_state_as_string(state);
    if (error) {
        std::cout << " (" << error << ")";
    }
    std::cout << std::endl;

    if (state == PW_STREAM_STATE_ERROR ||
        (state == PW_STREAM_STATE_UNCONNECTED && oldState != PW_STREAM_STATE_UNCONNECTED)) {
        pw_main_loop_quit(audio->mLoop);
    }
}

const pw_stream_events kOutputStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = StreamStateChanged,
    .process = ProcessAudio,
};

const pw_stream_events kIngressStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = StreamStateChanged,
    .param_changed = IngressParamChanged,
    .process = ProcessIngress,
};

void QuitMainLoop(void *userData, int /*signalNumber*/)
{
    auto *state = static_cast<AudioState *>(userData);
    pw_main_loop_quit(state->mLoop);
}
