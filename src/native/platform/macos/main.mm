#include "platform/macos/audio_callback.h"
#include "platform/posix/http/http_server.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

volatile std::sig_atomic_t gStop = 0;

void RequestStop(int) { gStop = 1; }

void Check(OSStatus status, const char *operation)
{
    if (status != noErr) {
        throw std::runtime_error(std::string(operation) + " failed (Core Audio status " +
                                 std::to_string(status) + ")");
    }
}

AudioObjectPropertyAddress Address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
T Property(AudioObjectID object, AudioObjectPropertySelector selector,
           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    auto address = Address(selector, scope);
    T value{};
    UInt32 size = sizeof(value);
    Check(AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value),
          "Read audio property");
    return value;
}

template <typename T>
std::vector<T> PropertyList(AudioObjectID object, AudioObjectPropertySelector selector,
                            AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    auto address = Address(selector, scope);
    UInt32 size = 0;
    Check(AudioObjectGetPropertyDataSize(object, &address, 0, nullptr, &size),
          "Read audio property size");
    std::vector<T> values(size / sizeof(T));
    if (size) {
        Check(AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, values.data()),
              "Read audio property list");
        values.resize(size / sizeof(T));
    }
    return values;
}

NSString *StringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    return CFBridgingRelease(Property<CFStringRef>(object, selector));
}

UInt32 Channels(AudioDeviceID device, AudioObjectPropertyScope scope)
{
    UInt32 channels = 0;
    for (auto stream : PropertyList<AudioStreamID>(device, kAudioDevicePropertyStreams, scope)) {
        channels += Property<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat)
                        .mChannelsPerFrame;
    }
    return channels;
}

AudioDeviceID ResolveDevice(NSString *uid)
{
    for (auto device :
         PropertyList<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
        if ([StringProperty(device, kAudioDevicePropertyDeviceUID) isEqualToString:uid]) {
            return device;
        }
    }
    throw std::runtime_error("Audio device not found: " + std::string(uid.UTF8String));
}

NSString *DefaultOutputUid()
{
    const auto output = Property<AudioDeviceID>(kAudioObjectSystemObject,
                                                kAudioHardwarePropertyDefaultOutputDevice);
    if (output == kAudioObjectUnknown) {
        throw std::runtime_error("macOS has no default audio output device.");
    }
    return StringProperty(output, kAudioDevicePropertyDeviceUID);
}

UInt32 StereoOutputStreamIndex(AudioDeviceID output)
{
    const auto streams = PropertyList<AudioStreamID>(output, kAudioDevicePropertyStreams,
                                                     kAudioDevicePropertyScopeOutput);
    for (size_t index = 0; index < streams.size(); ++index) {
        const auto format = Property<AudioStreamBasicDescription>(
            streams[index], kAudioStreamPropertyVirtualFormat);
        if (format.mChannelsPerFrame >= 2) {
            return static_cast<UInt32>(index);
        }
    }
    throw std::runtime_error("The current default output has no stereo audio stream.");
}

AudioObjectID CurrentProcessObject()
{
    const pid_t processId = getpid();
    auto address = Address(kAudioHardwarePropertyTranslatePIDToProcessObject);
    AudioObjectID process = kAudioObjectUnknown;
    UInt32 size = sizeof(process);
    Check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(processId),
                                     &processId, &size, &process),
          "Find current Core Audio process");
    if (process == kAudioObjectUnknown) {
        throw std::runtime_error("Could not exclude Ear Tuner from its system-audio tap.");
    }
    return process;
}

void PrintDevices()
{
    NSMutableArray *devices = [NSMutableArray array];
    for (auto device :
         PropertyList<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
        [devices addObject:@{
            @"uid" : StringProperty(device, kAudioDevicePropertyDeviceUID),
            @"name" : StringProperty(device, kAudioObjectPropertyName),
            @"input_channels" : @(Channels(device, kAudioObjectPropertyScopeInput)),
            @"output_channels" : @(Channels(device, kAudioObjectPropertyScopeOutput)),
            @"sample_rate" : @(Property<Float64>(device, kAudioDevicePropertyNominalSampleRate))
        }];
    }
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:devices
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:&error];
    if (!json) {
        throw std::runtime_error(error.localizedDescription.UTF8String);
    }
    std::cout.write(static_cast<const char *>(json.bytes), json.length);
    std::cout << "\n";
}

void ValidateOutput(AudioDeviceID output)
{
    if (Channels(output, kAudioObjectPropertyScopeOutput) < 2) {
        throw std::runtime_error("The current default output must provide stereo audio.");
    }
    const auto transport = Property<UInt32>(output, kAudioDevicePropertyTransportType);
    if (transport == kAudioDeviceTransportTypeVirtual ||
        transport == kAudioDeviceTransportTypeAggregate) {
        throw std::runtime_error(
            "Output must be physical headphones, not a virtual/aggregate device.");
    }
    if (!Property<UInt32>(output, kAudioDevicePropertyDeviceIsAlive)) {
        throw std::runtime_error("The current default output is disconnected.");
    }
    const auto rate = Property<Float64>(output, kAudioDevicePropertyNominalSampleRate);
    if (rate < 8000 || rate > 192000) {
        throw std::runtime_error("The current default output has an unsupported sample rate.");
    }
}

struct Runtime {
    AudioDeviceID mAggregate = kAudioObjectUnknown;
    AudioObjectID mTap = kAudioObjectUnknown;
    AudioDeviceIOProcID mProc = nullptr;
    bool mStarted = false;
    std::unique_ptr<CoreAudioState> mAudio = std::make_unique<CoreAudioState>();

    ~Runtime()
    {
        if (mStarted) {
            Report(AudioDeviceStop(mAggregate, mProc), "Stop audio");
        }
        if (mProc) {
            Report(AudioDeviceDestroyIOProcID(mAggregate, mProc), "Remove audio callback");
        }
        if (mAggregate != kAudioObjectUnknown) {
            Report(AudioHardwareDestroyAggregateDevice(mAggregate), "Remove private aggregate");
        }
        if (mTap != kAudioObjectUnknown) {
            Report(AudioHardwareDestroyProcessTap(mTap), "Remove private process tap");
        }
    }

    static void Report(OSStatus status, const char *operation)
    {
        if (status != noErr) {
            std::cerr << operation << " failed (Core Audio status " << status << ")\n";
        }
    }
};

void ConfigureStreams(Runtime &runtime, AudioObjectPropertyScope scope, UInt32 firstChannel,
                      UInt32 channelCount)
{
    const auto streams =
        PropertyList<AudioStreamID>(runtime.mAggregate, kAudioDevicePropertyStreams, scope);
    const size_t size =
        offsetof(AudioHardwareIOProcStreamUsage, mStreamIsOn) + streams.size() * sizeof(UInt32);
    // UInt64 storage keeps the variable-length Core Audio structure suitably aligned.
    std::vector<UInt64> storage((size + sizeof(UInt64) - 1) / sizeof(UInt64), 0);
    auto *usage = reinterpret_cast<AudioHardwareIOProcStreamUsage *>(storage.data());
    usage->mIOProc = reinterpret_cast<void *>(runtime.mProc);
    usage->mNumberStreams = static_cast<UInt32>(streams.size());
    UInt32 offset = 0;
    for (size_t index = 0; index < streams.size(); ++index) {
        const auto format = Property<AudioStreamBasicDescription>(
            streams[index], kAudioStreamPropertyVirtualFormat);
        const bool active = offset < firstChannel + channelCount &&
                            offset + format.mChannelsPerFrame > firstChannel;
        usage->mStreamIsOn[index] = active;
        offset += format.mChannelsPerFrame;
        if (active && scope == kAudioObjectPropertyScopeInput &&
            offset > firstChannel + channelCount) {
            throw std::runtime_error("Aggregate input stream does not isolate tap stereo.");
        }
        if (active &&
            (format.mFormatID != kAudioFormatLinearPCM ||
             !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
             (format.mFormatFlags & kAudioFormatFlagIsBigEndian) ||
             !(format.mFormatFlags & kAudioFormatFlagIsPacked) || format.mBitsPerChannel != 32 ||
             format.mSampleRate != runtime.mAudio->mSampleRate)) {
            throw std::runtime_error("Audio device must expose native packed float32 PCM "
                                     "at the selected sample rate.");
        }
    }
    if (offset < firstChannel + channelCount) {
        throw std::runtime_error("Aggregate device is missing the selected stereo channels.");
    }
    auto address = Address(kAudioDevicePropertyIOProcStreamUsage, scope);
    Check(AudioObjectSetPropertyData(runtime.mAggregate, &address, 0, nullptr,
                                     static_cast<UInt32>(size), usage),
          "Select aggregate streams");
}

void StartAudio(Runtime &runtime, NSString *outputUid)
{
    const auto output = ResolveDevice(outputUid);
    ValidateOutput(output);
    runtime.mAudio->mSampleRate = Property<Float64>(output, kAudioDevicePropertyNominalSampleRate);
    const auto streamIndex = StereoOutputStreamIndex(output);
    if (@available(macOS 14.2, *)) {
        auto *tapDescription =
            [[CATapDescription alloc] initExcludingProcesses:@[ @(CurrentProcessObject()) ]
                                                andDeviceUID:outputUid
                                                  withStream:streamIndex];
        tapDescription.name = @"Ear Tuner output capture";
        tapDescription.UUID = NSUUID.UUID;
        tapDescription.privateTap = YES;
        tapDescription.mixdown = YES;
        tapDescription.muteBehavior = CATapMuted;
        Check(AudioHardwareCreateProcessTap(tapDescription, &runtime.mTap),
              "Create output process tap");
    } else {
        throw std::runtime_error("System audio capture requires macOS 14.2 or later.");
    }
    const auto tapUid = StringProperty(runtime.mTap, kAudioTapPropertyUID);
    NSDictionary *description = @{
        @kAudioAggregateDeviceNameKey : @"Ear Tuner (private)",
        @kAudioAggregateDeviceUIDKey :
            [@"ear-tuner-" stringByAppendingString:NSUUID.UUID.UUIDString],
        @kAudioAggregateDeviceIsPrivateKey : @1,
        @kAudioAggregateDeviceIsStackedKey : @1,
        @kAudioAggregateDeviceMainSubDeviceKey : outputUid,
        @kAudioAggregateDeviceSubDeviceListKey : @[
            @{@kAudioSubDeviceUIDKey : outputUid}
        ],
        @kAudioAggregateDeviceTapListKey : @[
            @{@kAudioSubTapUIDKey : tapUid,
              @kAudioSubTapDriftCompensationKey : @1}
        ]
    };
    Check(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description,
                                             &runtime.mAggregate),
          "Create private aggregate");
    const auto rate = runtime.mAudio->mSampleRate;
    auto address = Address(kAudioDevicePropertyNominalSampleRate);
    Check(AudioObjectSetPropertyData(runtime.mAggregate, &address, 0, nullptr, sizeof(rate), &rate),
          "Set aggregate sample rate");
    Check(AudioDeviceCreateIOProcID(runtime.mAggregate, ProcessCoreAudio, runtime.mAudio.get(),
                                    &runtime.mProc),
          "Create audio callback");
    ConfigureStreams(runtime, kAudioObjectPropertyScopeInput, 0, 2);
    ConfigureStreams(runtime, kAudioObjectPropertyScopeOutput, 0, 2);
    Check(AudioDeviceStart(runtime.mAggregate, runtime.mProc),
          "Start audio (approve system-audio capture if prompted)");
    runtime.mStarted = true;
}

int Run(int argc, char **argv)
{
    std::string session;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--list-devices" && argc == 2) {
            PrintDevices();
            return 0;
        }
        if (argument == "--session" && index + 1 < argc) {
            NSString *value = [NSString stringWithUTF8String:argv[++index]];
            if (!value || !value.length) {
                throw std::runtime_error("Session must not be empty.");
            }
            session = value.UTF8String;
        } else {
            throw std::runtime_error("Usage: ear_tuner_macos --list-devices | "
                                     "--session ID");
        }
    }
    if (session.empty()) {
        throw std::runtime_error("A --session ID is required.");
    }
    NSString *outputUid = DefaultOutputUid();
    Runtime runtime;
    StartAudio(runtime, outputUid);
    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);
    auto &audio = *runtime.mAudio;
    PosixHttpServerState http;
    http.mDiagnostics = [&] {
        @autoreleasepool {
            NSDictionary *diagnostics = @{
                @"platform" : @"coreaudio",
                @"session" : [NSString stringWithUTF8String:session.c_str()],
                @"pid" : @(getpid()),
                @"output_uid" : outputUid,
                @"sample_rate" : @(audio.mSampleRate),
                @"callbacks" : @(audio.mCallbacks.load(std::memory_order_relaxed)),
                @"audio_ready" : @(audio.mCallbacks.load(std::memory_order_relaxed) > 0 &&
                                   !audio.mBufferError.load(std::memory_order_relaxed))
            };
            NSData *json = [NSJSONSerialization dataWithJSONObject:diagnostics options:0 error:nil];
            return std::string(static_cast<const char *>(json.bytes), json.length);
        }
    };
    std::atomic<bool> httpFinished{false};
    std::thread server([&] {
        @autoreleasepool {
            RunHttpServer(audio.mControl, http);
        }
        httpFinished.store(true);
    });
    int result = 0;
    auto lastCallback = std::chrono::steady_clock::now();
    uint32_t previousCallbacks = 0;
    try {
        while (!gStop) {
            const auto callbacks = audio.mCallbacks.load(std::memory_order_relaxed);
            if (callbacks != previousCallbacks) {
                previousCallbacks = callbacks;
                lastCallback = std::chrono::steady_clock::now();
            }
            if (httpFinished.load() || audio.mBufferError.load() ||
                std::chrono::steady_clock::now() - lastCallback > std::chrono::seconds(3)) {
                throw std::runtime_error(
                    "Audio callback or control server stopped. Check audio permissions, "
                    "device connection, sample rates, and that no other Ear Tuner is running.");
            }
            @autoreleasepool {
                if (![DefaultOutputUid() isEqualToString:outputUid]) {
                    throw std::runtime_error(
                        "The default output changed; restarting to follow the current endpoint.");
                }
                const auto output = ResolveDevice(outputUid);
                ValidateOutput(output);
                if (Property<Float64>(output, kAudioDevicePropertyNominalSampleRate) !=
                    audio.mSampleRate) {
                    throw std::runtime_error("Audio sample rate changed; restarting is required.");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        result = 1;
    }
    StopHttpServer(http);
    server.join();
    return result;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        try {
            return Run(argc, argv);
        } catch (const std::exception &error) {
            std::cerr << "Ear Tuner: " << error.what() << "\n";
            return 1;
        }
    }
}
