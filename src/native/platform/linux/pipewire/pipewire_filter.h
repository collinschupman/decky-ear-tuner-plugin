#pragma once

#include "ear_tuner/control_state.h"
#include "ear_tuner/ear_tuner_processor.h"
#include "ear_tuner/realtime.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/utils/ringbuffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

/**
 * State shared by the PipeWire streams and their processing callbacks.
 *
 * Owns the planar audio scratch buffers and ring buffers used to transfer
 * processed samples from ingress to output, along with the DSP controls and
 * processor. The pointed-to PipeWire objects and listeners are initialized
 * and released by the runtime setup code.
 */
struct AudioState {
    /** Maximum number of float samples stored per channel. */
    static constexpr uint32_t kBufferSampleCount = 16384;

    /** PipeWire objects and listeners associated with the main loop. */
    pw_main_loop *mLoop = nullptr;
    pw_context *mContext = nullptr;
    pw_core *mCore = nullptr;
    pw_stream *mIngressStream = nullptr;
    spa_hook mIngressListener{};
    pw_stream *mOutputStream = nullptr;
    spa_hook mOutputListener{};
    spa_ringbuffer mLeftRing{};
    spa_ringbuffer mRightRing{};
    /** Scratch storage backing the per-channel ring buffers. */
    std::array<float, kBufferSampleCount> mLeftBuffer{};
    std::array<float, kBufferSampleCount> mRightBuffer{};
    /** Processed samples awaiting transfer to the ring buffers. */
    std::array<float, kBufferSampleCount> mProcessedLeft{};
    std::array<float, kBufferSampleCount> mProcessedRight{};
    /** Shared settings read by the DSP callback. */
    EarTunerControlState mControl;
    /** Stateful DSP processor used by the ingress callback. */
    EarTunerProcessor mProcessor;
    /** Negotiated ingress rate; zero until a format is available. */
    std::atomic<uint32_t> mIngressSampleRate{0};
    /** Lock-free counters sampled by diagnostics outside the audio thread. */
    std::atomic<uint32_t> mIngressCallbackCount{0};
    std::atomic<uint32_t> mIngressDeadlineMissCount{0};
    std::atomic<uint32_t> mIngressMaximumNanoseconds{0};
    std::atomic<uint32_t> mOutputCallbackCount{0};
    std::atomic<uint32_t> mOutputDeadlineMissCount{0};
    std::atomic<uint32_t> mOutputMaximumNanoseconds{0};
    std::atomic<uint32_t> mRingUnderrunCount{0};
    std::atomic<uint32_t> mRingOverflowCount{0};
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);

/** Serializes a relaxed snapshot of callback telemetry for diagnostics. */
std::string AudioDiagnosticsJson(const AudioState &state);

/** PipeWire event handlers for the output stream. */
extern const pw_stream_events kOutputStreamEvents;
/** PipeWire event handlers for the ingress stream. */
extern const pw_stream_events kIngressStreamEvents;

/**
 * Creates properties for the virtual sink used as the smart-filter ingress.
 *
 * The caller owns the returned PipeWire properties and must free them with
 * pw_properties_free().
 */
pw_properties *CreateIngressStreamProperties();

/**
 * Creates properties for the virtual stream that publishes processed audio.
 *
 * The caller owns the returned PipeWire properties and must free them with
 * pw_properties_free().
 */
pw_properties *CreateOutputStreamProperties();

/**
 * Writes processed planar stereo samples to the per-channel ring buffers.
 *
 * If there is insufficient ring-buffer space, buffered data is discarded
 * before writing the new samples.
 *
 * @param state Audio state containing the ring buffers and backing storage.
 * @param left Processed left-channel samples.
 * @param right Processed right-channel samples.
 * @param sampleCount Number of float samples in each channel.
 */
void WriteProcessedAudio(AudioState &state, const float *left, const float *right,
                         uint32_t sampleCount);

/**
 * Reads planar stereo samples from the ring buffers.
 *
 * If either channel has fewer than sampleCount samples available, both output
 * channels are filled with silence, buffered data is left unread, and false
 * is returned.
 *
 * @param state Audio state containing the ring buffers and backing storage.
 * @param left Destination for left-channel samples.
 * @param right Destination for right-channel samples.
 * @param sampleCount Number of float samples to read per channel.
 * @return True if the requested samples were available in both channels.
 */
bool ReadProcessedAudio(AudioState &state, float *left, float *right, uint32_t sampleCount);

/**
 * PipeWire output-stream process callback.
 *
 * Dequeues an output buffer, fills it with processed samples (or silence on
 * ring-buffer underrun), updates its chunk metadata, and queues it again.
 * Intended to run on PipeWire's real-time processing thread.
 */
void ProcessAudio(void *userData) EAR_TUNER_NONBLOCKING;

/**
 * PipeWire ingress-stream process callback.
 *
 * Dequeues planar float samples, processes up to kBufferSampleCount frames at
 * the negotiated ingress rate, writes the result to the ring buffers, and
 * queues the input buffer again. Intended to run on PipeWire's real-time
 * processing thread.
 */
void ProcessIngress(void *userData) EAR_TUNER_NONBLOCKING;

/** Processes bounded planar chunks, including offsets that wrap within a data plane. */
uint32_t ProcessIngressBuffer(AudioState &state, const spa_buffer &buffer,
                              uint32_t sampleRate) EAR_TUNER_NONBLOCKING;

/** Publishes the negotiated ingress rate, stopping the loop on invalid formats. */
void IngressParamChanged(void *userData, uint32_t id, const spa_pod *param);

/**
 * Logs stream state changes and stops the main loop on error or disconnect.
 *
 * @param userData AudioState associated with the stream.
 * @param oldState Previous PipeWire stream state.
 * @param state New PipeWire stream state.
 * @param error Optional error description supplied by PipeWire.
 */
void StreamStateChanged(void *userData, pw_stream_state oldState, pw_stream_state state,
                        const char *error);

/** Stops the PipeWire main loop, typically from a signal handler. */
void QuitMainLoop(void *userData, int signalNumber);