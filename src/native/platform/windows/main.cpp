#include "platform/posix/http/http_server.h"
#include "platform/windows/audio_bridge.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <chrono>
#include <cwctype>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStop{false};

BOOL WINAPI ConsoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        gStop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

void Check(HRESULT result, const char *operation)
{
    if (FAILED(result)) {
        std::ostringstream error;
        error << operation << " failed (HRESULT 0x" << std::hex
              << static_cast<unsigned long>(result) << ")";
        throw std::runtime_error(error.str());
    }
}

template <typename T>
struct ComRelease {
    void operator()(T *value) const { value->Release(); }
};
template <typename T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

struct Apartment {
    Apartment() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization"); }
    ~Apartment() { CoUninitialize(); }
};

struct TaskFree {
    void operator()(void *value) const { CoTaskMemFree(value); }
};

struct HandleClose {
    void operator()(void *value) const { CloseHandle(value); }
};
using Handle = std::unique_ptr<void, HandleClose>;

std::string Utf8(const std::wstring &value)
{
    if (value.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error("Invalid Unicode endpoint identifier/name");
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        throw std::runtime_error("Could not encode endpoint identifier/name");
    }
    return result;
}

std::string JsonString(const std::string &value)
{
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 32) {
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result + '"';
}

ComPtr<IMMDeviceEnumerator> Enumerator()
{
    IMMDeviceEnumerator *value = nullptr;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&value)),
          "Audio device enumeration");
    return ComPtr<IMMDeviceEnumerator>(value);
}

std::wstring DeviceName(IMMDevice *device)
{
    IPropertyStore *raw = nullptr;
    Check(device->OpenPropertyStore(STGM_READ, &raw), "Read endpoint properties");
    ComPtr<IPropertyStore> properties(raw);
    PROPVARIANT value;
    PropVariantInit(&value);
    Check(properties->GetValue(PKEY_Device_FriendlyName, &value), "Read endpoint name");
    const bool valid = value.vt == VT_LPWSTR && value.pwszVal;
    const std::wstring result = valid ? value.pwszVal : L"";
    Check(PropVariantClear(&value), "Release endpoint property");
    if (!valid || result.empty()) {
        throw std::runtime_error("Endpoint has no usable friendly name");
    }
    return result;
}

bool IsCable(std::wstring name)
{
    for (auto &ch : name) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return name.find(L"cable") != std::wstring::npos &&
           name.find(L"vb-audio") != std::wstring::npos;
}

EDataFlow Direction(IMMDevice *device)
{
    IMMEndpoint *raw = nullptr;
    Check(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void **>(&raw)),
          "Read endpoint direction");
    ComPtr<IMMEndpoint> endpoint(raw);
    EDataFlow flow{};
    Check(endpoint->GetDataFlow(&flow), "Read endpoint direction");
    return flow;
}

void ListDevices(IMMDeviceEnumerator *enumerator)
{
    IMMDeviceCollection *raw = nullptr;
    Check(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &raw), "List active endpoints");
    ComPtr<IMMDeviceCollection> devices(raw);
    UINT count = 0;
    Check(devices->GetCount(&count), "Count endpoints");
    std::cout << "[";
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *item = nullptr;
        Check(devices->Item(i, &item), "Read endpoint");
        ComPtr<IMMDevice> device(item);
        LPWSTR uid = nullptr;
        Check(device->GetId(&uid), "Read endpoint ID");
        std::unique_ptr<wchar_t, TaskFree> ownedUid(uid);
        const auto name = DeviceName(device.get());
        std::cout << (i ? "," : "") << "{\"uid\":" << JsonString(Utf8(uid))
                  << ",\"name\":" << JsonString(Utf8(name)) << ",\"direction\":\""
                  << (Direction(device.get()) == eCapture ? "capture" : "render")
                  << "\",\"virtual_cable\":" << (IsCable(name) ? "true" : "false") << "}";
    }
    std::cout << "]\n";
}

ComPtr<IMMDevice> Device(IMMDeviceEnumerator *enumerator, const std::wstring &uid, EDataFlow flow)
{
    IMMDevice *raw = nullptr;
    Check(enumerator->GetDevice(uid.c_str(), &raw), "Open selected endpoint");
    ComPtr<IMMDevice> device(raw);
    DWORD state = 0;
    Check(device->GetState(&state), "Read endpoint state");
    if (state != DEVICE_STATE_ACTIVE || Direction(device.get()) != flow) {
        throw std::runtime_error("Selected endpoint is inactive or has the wrong direction");
    }
    const bool cable = IsCable(DeviceName(device.get()));
    if ((flow == eCapture && !cable) || (flow == eRender && cable)) {
        throw std::runtime_error(
            "Select VB-CABLE's recording endpoint as input and physical headphones as output; "
            "keep VB-CABLE's original friendly name");
    }
    return device;
}

class AudioEndpoint {
   public:
    AudioEndpoint(IMMDevice *device, bool render)
    {
        IAudioClient *raw = nullptr;
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void **>(&raw)),
              "Activate WASAPI endpoint");
        mClient.reset(raw);
        WAVEFORMATEX *mix = nullptr;
        Check(mClient->GetMixFormat(&mix), "Read WASAPI mix format");
        std::unique_ptr<WAVEFORMATEX, TaskFree> ownedMix(mix);
        if (!mix || (render ? mix->nChannels < 2 : mix->nChannels != 2)) {
            throw std::runtime_error("Both selected endpoints must support stereo audio");
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = WindowsAudioBridge::kSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            (render ? AUDCLNT_STREAMFLAGS_RATEADJUST : 0);
        Check(mClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr),
              "Initialize stereo 48 kHz shared WASAPI stream");
        mEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!mEvent) {
            throw std::runtime_error("Could not create WASAPI event: " +
                                     std::to_string(GetLastError()));
        }
        Check(mClient->SetEventHandle(mEvent.get()), "Set WASAPI event");
        Check(mClient->GetBufferSize(&mFrames), "Read WASAPI buffer capacity");
        if (mFrames == 0 || mFrames > WindowsAudioBridge::kMaxFrames) {
            throw std::runtime_error("WASAPI buffer exceeds supported capacity");
        }
    }
    ~AudioEndpoint()
    {
        if (mStarted) {
            const auto result = mClient->Stop();
            if (FAILED(result)) {
                std::cerr << "WASAPI stop failed: " << std::hex << result << "\n";
            }
        }
    }
    void Start()
    {
        Check(mClient->Start(), "Start WASAPI stream");
        mStarted = true;
    }
    ComPtr<IAudioClient> mClient;
    Handle mEvent;
    UINT32 mFrames = 0;

   private:
    bool mStarted = false;
};

class Runtime {
   public:
    Runtime(WindowsAudioBridge &bridge, AudioEndpoint &output, std::string session)
        : mBridge(bridge)
    {
        mHttp.mDiagnostics = [this, session = std::move(session)] {
            const bool ready = mBridge.mCapturePackets.load() != 0 &&
                               mBridge.mRenderPackets.load() != 0 && mClockReady.load() &&
                               !mBridge.mBufferError.load() && !mClockFailed.load();
            return "{\"platform\":\"wasapi\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
                   ",\"session\":" + JsonString(session) +
                   ",\"sample_rate\":48000,\"audio_ready\":" + (ready ? "true" : "false") +
                   ",\"render_rate\":" + std::to_string(mBridge.mRenderRate.load()) +
                   ",\"buffered_frames\":" + std::to_string(mBridge.mBufferedFrames.load()) +
                   ",\"underruns\":" + std::to_string(mBridge.mUnderruns.load()) +
                   ",\"overflows\":" + std::to_string(mBridge.mOverflows.load()) +
                   ",\"discontinuities\":" + std::to_string(mBridge.mDiscontinuities.load()) + "}";
        };
        mHttpThread = std::thread([this] {
            RunHttpServer(mBridge.mControl, mHttp);
            mHttpExited.store(true);
        });
        try {
            mClockThread = std::thread([this, client = output.mClient.get()] {
                try {
                    Apartment apartment;
                    IAudioClockAdjustment *raw = nullptr;
                    Check(client->GetService(__uuidof(IAudioClockAdjustment),
                                             reinterpret_cast<void **>(&raw)),
                          "Open render clock adjustment");
                    ComPtr<IAudioClockAdjustment> clock(raw);
                    while (!mStop.load()) {
                        Check(clock->SetSampleRate(mBridge.mRenderRate.load()),
                              "Adjust WASAPI render clock");
                        mClockReady.store(true);
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                } catch (const std::exception &error) {
                    std::cerr << "WASAPI clock controller: " << error.what() << "\n";
                    mClockFailed.store(true);
                }
            });
        } catch (...) {
            StopHttpServer(mHttp);
            mHttpThread.join();
            throw;
        }
    }
    ~Runtime()
    {
        mStop.store(true);
        StopHttpServer(mHttp);
        mClockThread.join();
        mHttpThread.join();
    }
    void CheckHealth() const
    {
        if (mClockFailed.load() || mHttpExited.load() || mBridge.mBufferError.load()) {
            throw std::runtime_error("Native audio/control worker failed; see preceding log");
        }
    }

   private:
    WindowsAudioBridge &mBridge;
    PosixHttpServerState mHttp;
    std::atomic<bool> mStop{false};
    std::atomic<bool> mClockFailed{false};
    std::atomic<bool> mClockReady{false};
    std::atomic<bool> mHttpExited{false};
    std::thread mHttpThread;
    std::thread mClockThread;
};

class AudioPriority {
   public:
    AudioPriority()
    {
        DWORD task = 0;
        mHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (!mHandle) {
            throw std::runtime_error("Could not enable MMCSS audio scheduling: " +
                                     std::to_string(GetLastError()));
        }
    }
    ~AudioPriority()
    {
        if (!AvRevertMmThreadCharacteristics(mHandle)) {
            std::cerr << "Could not release MMCSS audio scheduling: " << GetLastError() << "\n";
        }
    }

   private:
    HANDLE mHandle = nullptr;
};

void Run(IMMDeviceEnumerator *enumerator, const std::wstring &inputUid,
         const std::wstring &outputUid, const std::string &session)
{
    auto inputDevice = Device(enumerator, inputUid, eCapture);
    auto outputDevice = Device(enumerator, outputUid, eRender);
    AudioEndpoint input(inputDevice.get(), false);
    AudioEndpoint output(outputDevice.get(), true);
    IAudioCaptureClient *captureRaw = nullptr;
    Check(input.mClient->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void **>(&captureRaw)),
          "Open capture client");
    ComPtr<IAudioCaptureClient> capture(captureRaw);
    IAudioRenderClient *renderRaw = nullptr;
    Check(output.mClient->GetService(__uuidof(IAudioRenderClient),
                                     reinterpret_cast<void **>(&renderRaw)),
          "Open render client");
    ComPtr<IAudioRenderClient> render(renderRaw);
    auto bridge = std::make_unique<WindowsAudioBridge>();
    Runtime runtime(*bridge, output, session);
    BYTE *data = nullptr;
    Check(render->GetBuffer(output.mFrames, &data), "Prime render buffer");
    Check(render->ReleaseBuffer(output.mFrames, AUDCLNT_BUFFERFLAGS_SILENT),
          "Prime render silence");
    input.Start();
    output.Start();
    AudioPriority priority;
    const HANDLE events[] = {input.mEvent.get(), output.mEvent.get()};
    ULONGLONG lastCapture = GetTickCount64();
    ULONGLONG lastRender = lastCapture;
    while (!gStop.load(std::memory_order_relaxed)) {
        const DWORD event = WaitForMultipleObjects(2, events, FALSE, 100);
        runtime.CheckHealth();
        if (event != WAIT_OBJECT_0 && event != WAIT_OBJECT_0 + 1 && event != WAIT_TIMEOUT) {
            throw std::runtime_error("WASAPI event wait failed: " + std::to_string(GetLastError()));
        }
        if (event == WAIT_OBJECT_0) {
            UINT32 packet = 0;
            Check(capture->GetNextPacketSize(&packet), "Read capture packet size");
            UINT32 drained = 0;
            while (packet != 0) {
                UINT32 frames = 0;
                DWORD flags = 0;
                Check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                      "Read capture buffer");
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    bridge->Discontinuity();
                }
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const bool valid =
                    (silent || data) &&
                    bridge->Capture(silent ? nullptr : reinterpret_cast<float *>(data), frames);
                Check(capture->ReleaseBuffer(frames), "Release capture buffer");
                if (!valid) {
                    throw std::runtime_error("Invalid WASAPI capture buffer");
                }
                lastCapture = GetTickCount64();
                drained += frames;
                // Bound each pass so a continuously refilled capture queue cannot starve output.
                if (drained >= WindowsAudioBridge::kMaxFrames) {
                    break;
                }
                Check(capture->GetNextPacketSize(&packet), "Read next capture packet size");
            }
        }
        const DWORD renderEvent = event == WAIT_OBJECT_0
                                      ? WaitForSingleObject(output.mEvent.get(), 0)
                                      : (event == WAIT_OBJECT_0 + 1 ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
        if (renderEvent == WAIT_OBJECT_0) {
            UINT32 padding = 0;
            Check(output.mClient->GetCurrentPadding(&padding), "Read render padding");
            if (padding > output.mFrames) {
                throw std::runtime_error("Invalid WASAPI render padding");
            }
            const UINT32 frames = output.mFrames - padding;
            if (frames != 0) {
                Check(render->GetBuffer(frames, &data), "Get render buffer");
                const bool valid = bridge->Render(reinterpret_cast<float *>(data), frames);
                Check(render->ReleaseBuffer(frames, valid ? 0 : AUDCLNT_BUFFERFLAGS_SILENT),
                      "Release render buffer");
                if (!valid) {
                    throw std::runtime_error("Invalid WASAPI render buffer");
                }
                lastRender = GetTickCount64();
            }
        } else if (renderEvent != WAIT_TIMEOUT) {
            throw std::runtime_error("WASAPI render event failed: " +
                                     std::to_string(GetLastError()));
        }
        const auto now = GetTickCount64();
        if (now - lastCapture > 3000 || now - lastRender > 3000) {
            throw std::runtime_error("WASAPI endpoint stalled or disconnected");
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
    try {
        Apartment apartment;
        auto enumerator = Enumerator();
        if (argc == 2 && std::wstring(argv[1]) == L"--list-devices") {
            ListDevices(enumerator.get());
            return 0;
        }
        std::wstring input, output, session;
        for (int i = 1; i < argc; ++i) {
            const std::wstring option = argv[i];
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing native argument value");
            }
            if (option == L"--input") {
                input = argv[++i];
            } else if (option == L"--output") {
                output = argv[++i];
            } else if (option == L"--session") {
                session = argv[++i];
            } else {
                throw std::runtime_error("Unknown native argument");
            }
        }
        if (input.empty() || output.empty() || session.empty() || input == output) {
            throw std::runtime_error(
                "Usage: ear_tuner_windows --input UID --output UID --session ID");
        }
        if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
            throw std::runtime_error("Could not register console shutdown handler");
        }
        Run(enumerator.get(), input, output, Utf8(session));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Ear Tuner WASAPI: " << error.what() << "\n";
        return 1;
    }
}
