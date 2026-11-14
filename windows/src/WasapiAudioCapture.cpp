#include "WasapiAudioCapture.h"
#include "PipelineDiagnostics.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(result);
    return text.str();
}

uint64_t DefaultChannelMask(int channels) {
    if (channels == 1) return SPEAKER_FRONT_CENTER;
    if (channels == 2) return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    return 0;
}

bool DescribeFormat(const WAVEFORMATEX* wave, const AudioCaptureDevice& device,
                    AudioCaptureFormat& format, std::string& error) {
    if (!wave || wave->nChannels == 0 || wave->nSamplesPerSec == 0 || wave->nBlockAlign == 0) {
        error = "WASAPI returned an invalid capture format";
        return false;
    }
    WORD tag = wave->wFormatTag;
    WORD validBits = wave->wBitsPerSample;
    uint64_t channelMask = DefaultChannelMask(wave->nChannels);
    GUID subtype{};
    if (tag == WAVE_FORMAT_EXTENSIBLE && wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        subtype = extended->SubFormat;
        validBits = extended->Samples.wValidBitsPerSample;
        channelMask = extended->dwChannelMask;
        const bool standardWaveSubtype = subtype.Data2 == 0 && subtype.Data3 == 0x0010 &&
            subtype.Data4[0] == 0x80 && subtype.Data4[1] == 0x00 && subtype.Data4[2] == 0x00 &&
            subtype.Data4[3] == 0xaa && subtype.Data4[4] == 0x00 && subtype.Data4[5] == 0x38 &&
            subtype.Data4[6] == 0x9b && subtype.Data4[7] == 0x71;
        if (standardWaveSubtype && subtype.Data1 == WAVE_FORMAT_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (standardWaveSubtype && subtype.Data1 == WAVE_FORMAT_PCM) tag = WAVE_FORMAT_PCM;
    }
    AudioSampleKind kind{};
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wave->wBitsPerSample == 32) kind = AudioSampleKind::Float32;
    else if (tag == WAVE_FORMAT_PCM && wave->wBitsPerSample == 16) kind = AudioSampleKind::Pcm16;
    else if (tag == WAVE_FORMAT_PCM && wave->wBitsPerSample == 32 && validBits >= 24) kind = AudioSampleKind::Pcm32;
    else {
        error = "Unsupported WASAPI sample format: " + std::to_string(wave->wBitsPerSample) + " bits";
        return false;
    }
    format = {device.id, device.name, static_cast<int>(wave->nSamplesPerSec), static_cast<int>(wave->nChannels),
              static_cast<int>(wave->wBitsPerSample), static_cast<int>(wave->nBlockAlign), channelMask, 0, kind};
    return true;
}

WAVEFORMATEX Float48kFormat(WORD channels) {
    WAVEFORMATEX wave{};
    wave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wave.nChannels = channels;
    wave.nSamplesPerSec = 48'000;
    wave.wBitsPerSample = 32;
    wave.nBlockAlign = static_cast<WORD>(channels * sizeof(float));
    wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
    return wave;
}
}

WasapiAudioCapture::WasapiAudioCapture(ChunkCallback chunkCallback, StatusCallback statusCallback)
    : chunkCallback_(std::move(chunkCallback)), statusCallback_(std::move(statusCallback)) {}

WasapiAudioCapture::~WasapiAudioCapture() { Stop(); }

std::vector<AudioCaptureDevice> WasapiAudioCapture::EnumerateDevices(std::string& error) {
    std::vector<AudioCaptureDevice> result;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    ComPtr<IMMDeviceCollection> collection;
    if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
    for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        LPWSTR id = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        hr = collection->Item(index, &device);
        if (SUCCEEDED(hr)) hr = device->GetId(&id);
        if (SUCCEEDED(hr)) hr = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(hr)) hr = properties->GetValue(PKEY_Device_FriendlyName, &name);
        if (SUCCEEDED(hr) && id && name.vt == VT_LPWSTR)
            result.push_back({id, name.pwszVal});
        if (id) CoTaskMemFree(id);
        PropVariantClear(&name);
    }
    if (FAILED(hr)) error = "Could not enumerate WASAPI capture devices: " + HResultText(hr);
    if (uninitialize) CoUninitialize();
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    return result;
}

bool WasapiAudioCapture::Start(const AudioCaptureDevice& device, std::string& error) {
    Stop();
    {
        std::lock_guard lock(mutex_);
        snapshot_ = {};
        initialized_ = false;
        initializationSucceeded_ = false;
    }
    stopRequested_.store(false);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        error = "Could not create the WASAPI stop event";
        return false;
    }
    worker_ = std::thread(&WasapiAudioCapture::WorkerMain, this, device);
    std::unique_lock lock(mutex_);
    if (!initializedChanged_.wait_for(lock, std::chrono::seconds(5), [this] { return initialized_; })) {
        error = "Timed out while opening the WASAPI capture device";
        lock.unlock();
        Stop();
        return false;
    }
    error = snapshot_.error;
    return initializationSucceeded_;
}

void WasapiAudioCapture::Stop() {
    stopRequested_.store(true);
    if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
    if (worker_.joinable()) worker_.join();
    if (stopEvent_) CloseHandle(static_cast<HANDLE>(stopEvent_));
    stopEvent_ = nullptr;
    std::lock_guard lock(mutex_);
    snapshot_.running = false;
}

AudioCaptureSnapshot WasapiAudioCapture::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void WasapiAudioCapture::SetInitialization(bool success, const std::string& error,
                                           const AudioCaptureFormat& format) {
    {
        std::lock_guard lock(mutex_);
        initialized_ = true;
        initializationSucceeded_ = success;
        snapshot_.running = success;
        snapshot_.error = error;
        snapshot_.format = format;
    }
    initializedChanged_.notify_all();
}

void WasapiAudioCapture::WorkerMain(AudioCaptureDevice device) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClock> audioClock;
    WAVEFORMATEX* mix = nullptr;
    WAVEFORMATEX requested{};
    const WAVEFORMATEX* activeFormat = &requested;
    HANDLE captureEvent = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(device.id.c_str(), &endpoint);
    if (SUCCEEDED(hr)) hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(client.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr)) {
        requested = Float48kFormat(mix->nChannels);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                1'000'000, 0, &requested, nullptr);
        if (FAILED(hr)) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                    1'000'000, 0, mix, nullptr);
            activeFormat = mix;
        }
    }
    AudioCaptureFormat format;
    std::string error;
    if (SUCCEEDED(hr) && !DescribeFormat(activeFormat, device, format, error)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&audioClock));
    if (SUCCEEDED(hr)) hr = audioClock->GetFrequency(&format.devicePositionFrequency);
    if (SUCCEEDED(hr)) {
        captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!captureEvent) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(captureEvent);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        if (error.empty()) error = "Could not open WASAPI device " + WideToUtf8(device.name) + ": " + HResultText(hr);
        SetInitialization(false, error);
    } else {
        SetInitialization(true, {}, format);
        PipelineDiagnostics::Log("audio", "WASAPI capture started device=" + WideToUtf8(device.name) +
            " format=" + std::to_string(format.sampleRate) + "Hz/" + std::to_string(format.channels) + "ch/" +
            std::to_string(format.bitsPerSample) + "bit; deviceClock=" +
            std::to_string(format.devicePositionFrequency) + " units/s");
        statusCallback_("Audio input · " + WideToUtf8(device.name));
        HANDLE waits[] = {static_cast<HANDLE>(stopEvent_), captureEvent};
        while (!stopRequested_.load()) {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_TIMEOUT) {
                std::lock_guard lock(mutex_);
                ++snapshot_.underruns;
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 1) break;
            UINT32 available = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&available)) && available > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpc100ns = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc100ns);
                if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
                if (FAILED(hr)) {
                    std::lock_guard lock(mutex_);
                    ++snapshot_.underruns;
                    break;
                }
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const size_t bytes = static_cast<size_t>(frames) * format.blockAlign;
                auto owned = std::make_shared<std::vector<uint8_t>>(bytes);
                if (silent) std::fill(owned->begin(), owned->end(), 0);
                else std::memcpy(owned->data(), data, bytes);
                const double peak = MeasurePeak(owned->data(), frames, format, silent);
                const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
                const bool timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
                capture->ReleaseBuffer(frames);
                uint64_t overruns = 0;
                uint64_t underruns = 0;
                uint64_t timestampErrors = 0;
                {
                    std::lock_guard lock(mutex_);
                    snapshot_.peak = std::max(peak, snapshot_.peak * 0.82);
                    ++snapshot_.packets;
                    snapshot_.frames += frames;
                    if (discontinuity) ++snapshot_.overruns;
                    if (timestampError) ++snapshot_.timestampErrors;
                    overruns = snapshot_.overruns;
                    underruns = snapshot_.underruns;
                    timestampErrors = snapshot_.timestampErrors;
                }
                chunkCallback_({owned, frames, devicePosition, static_cast<int64_t>(qpc100ns * 100),
                                timestampError, discontinuity, overruns, underruns, timestampErrors});
            }
        }
        client->Stop();
    }
    if (captureEvent) CloseHandle(captureEvent);
    if (mix) CoTaskMemFree(mix);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (uninitialize) CoUninitialize();
    {
        std::lock_guard lock(mutex_);
        snapshot_.running = false;
    }
    PipelineDiagnostics::Log("audio", "WASAPI capture stopped device=" + WideToUtf8(device.name));
}

double WasapiAudioCapture::MeasurePeak(const uint8_t* data, uint32_t frames,
                                       const AudioCaptureFormat& format, bool silent) {
    if (silent || !data) return 0.0;
    const size_t samples = static_cast<size_t>(frames) * format.channels;
    double peak = 0.0;
    if (format.sampleKind == AudioSampleKind::Float32) {
        const auto* values = reinterpret_cast<const float*>(data);
        for (size_t i = 0; i < samples; ++i) peak = std::max(peak, std::abs(static_cast<double>(values[i])));
    } else if (format.sampleKind == AudioSampleKind::Pcm16) {
        const auto* values = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < samples; ++i) peak = std::max(peak, std::abs(values[i] / 32768.0));
    } else {
        const auto* values = reinterpret_cast<const int32_t*>(data);
        for (size_t i = 0; i < samples; ++i) peak = std::max(peak, std::abs(values[i] / 2147483648.0));
    }
    return std::min(1.0, peak);
}
