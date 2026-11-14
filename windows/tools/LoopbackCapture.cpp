#include "Common.h"

#include <audioclient.h>
#include <avrt.h>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functiondiscoverykeys_devpkey.h>
#include <iomanip>
#include <iostream>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t kSampleRate = 48'000;
constexpr uint16_t kBitsPerSample = 16;

struct EndpointInfo {
    ComPtr<IMMDevice> device;
    std::wstring id;
    std::wstring name;
    uint32_t mixSampleRate{};
    uint16_t mixChannels{};
    uint16_t mixBitsPerSample{};
};

std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(result);
    return text.str();
}

std::string JsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 16);
    for (const char c : value) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

void WriteU16(std::ostream& stream, uint16_t value) {
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

void WriteU32(std::ostream& stream, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) stream.put(static_cast<char>((value >> shift) & 0xff));
}

void WriteWaveHeader(std::fstream& stream, uint32_t dataBytes, uint16_t channels) {
    const uint16_t blockAlign = channels * kBitsPerSample / 8;
    stream.seekp(0);
    stream.write("RIFF", 4);
    WriteU32(stream, 36 + dataBytes);
    stream.write("WAVEfmt ", 8);
    WriteU32(stream, 16);
    WriteU16(stream, WAVE_FORMAT_PCM);
    WriteU16(stream, channels);
    WriteU32(stream, kSampleRate);
    WriteU32(stream, kSampleRate * blockAlign);
    WriteU16(stream, blockAlign);
    WriteU16(stream, kBitsPerSample);
    stream.write("data", 4);
    WriteU32(stream, dataBytes);
}

WAVEFORMATEX Pcm48k(uint16_t channels) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = channels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = channels * kBitsPerSample / 8;
    format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;
    return format;
}

std::wstring DeviceName(IMMDevice* endpoint) {
    ComPtr<IPropertyStore> properties;
    PROPVARIANT name;
    PropVariantInit(&name);
    std::wstring result = L"Unknown audio endpoint";
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
        result = name.pwszVal;
    }
    PropVariantClear(&name);
    return result;
}

std::vector<EndpointInfo> CaptureEndpoints(IMMDeviceEnumerator* enumerator, HRESULT& hr) {
    std::vector<EndpointInfo> result;
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
    for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index) {
        EndpointInfo info;
        LPWSTR id = nullptr;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX* mix = nullptr;
        hr = collection->Item(index, &info.device);
        if (SUCCEEDED(hr)) hr = info.device->GetId(&id);
        if (SUCCEEDED(hr) && id) info.id = id;
        if (id) CoTaskMemFree(id);
        info.name = DeviceName(info.device.Get());
        if (SUCCEEDED(hr)) {
            hr = info.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(client.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
        if (SUCCEEDED(hr) && mix) {
            info.mixSampleRate = mix->nSamplesPerSec;
            info.mixChannels = mix->nChannels;
            info.mixBitsPerSample = mix->wBitsPerSample;
        }
        if (mix) CoTaskMemFree(mix);
        if (SUCCEEDED(hr)) result.push_back(std::move(info));
    }
    return result;
}

void ListCaptureEndpoints(IMMDeviceEnumerator* enumerator) {
    HRESULT hr = S_OK;
    const auto devices = CaptureEndpoints(enumerator, hr);
    if (FAILED(hr)) throw std::runtime_error("Could not enumerate capture endpoints: " + HResultText(hr));
    std::cout << "{\n  \"captureEndpoints\": [\n";
    for (size_t index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        std::cout << "    {\"id\": \"" << JsonEscape(WideToUtf8(device.id))
                  << "\", \"name\": \"" << JsonEscape(WideToUtf8(device.name))
                  << "\", \"mixSampleRate\": " << device.mixSampleRate
                  << ", \"mixChannels\": " << device.mixChannels
                  << ", \"mixBitsPerSample\": " << device.mixBitsPerSample << "}"
                  << (index + 1 == devices.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}

ComPtr<IMMDevice> SelectCaptureEndpoint(IMMDeviceEnumerator* enumerator, const std::wstring& match,
                                        EndpointInfo& selected, HRESULT& hr) {
    const auto devices = CaptureEndpoints(enumerator, hr);
    if (FAILED(hr)) return {};
    const std::wstring loweredMatch = Lower(match);
    std::vector<const EndpointInfo*> matches;
    for (const auto& device : devices) {
        if (Lower(device.name).find(loweredMatch) != std::wstring::npos) matches.push_back(&device);
    }
    if (matches.size() != 1) {
        std::ostringstream error;
        error << "Expected exactly one active capture endpoint matching '" << WideToUtf8(match)
              << "'; found " << matches.size();
        throw std::runtime_error(error.str());
    }
    selected = *matches.front();
    return selected.device;
}

void WriteState(const fs::path& path, const std::string& role, const EndpointInfo& endpoint,
                const fs::path& wavePath, const fs::path& csvPath, int64_t startedQpcNs,
                uint64_t deviceClockFrequency, uint16_t channels, uint64_t packets, uint64_t frames,
                uint64_t discontinuities, uint64_t timestampErrors, uint64_t silentPackets,
                uint64_t firstDevicePosition, uint64_t lastDevicePosition,
                uint64_t firstQpc100ns, uint64_t lastQpc100ns, bool complete) {
    std::ofstream output(path, std::ios::trunc);
    output << "{\n"
           << "  \"schemaVersion\": 2,\n"
           << "  \"complete\": " << (complete ? "true" : "false") << ",\n"
           << "  \"role\": \"" << role << "\",\n"
           << "  \"endpoint\": \"" << JsonEscape(WideToUtf8(endpoint.name)) << "\",\n"
           << "  \"endpointId\": \"" << JsonEscape(WideToUtf8(endpoint.id)) << "\",\n"
           << "  \"wavePath\": \"" << JsonEscape(WideToUtf8(wavePath.wstring())) << "\",\n"
           << "  \"csvPath\": \"" << JsonEscape(WideToUtf8(csvPath.wstring())) << "\",\n"
           << "  \"sampleRate\": " << kSampleRate << ",\n"
           << "  \"channels\": " << channels << ",\n"
           << "  \"bitsPerSample\": " << kBitsPerSample << ",\n"
           << "  \"mixSampleRate\": " << endpoint.mixSampleRate << ",\n"
           << "  \"mixChannels\": " << endpoint.mixChannels << ",\n"
           << "  \"mixBitsPerSample\": " << endpoint.mixBitsPerSample << ",\n"
           << "  \"sampleRateConversion\": " << (endpoint.mixSampleRate == kSampleRate ? "false" : "true") << ",\n"
           << "  \"deviceClockFrequency\": " << deviceClockFrequency << ",\n"
           << "  \"startedQpcNs\": " << startedQpcNs << ",\n"
           << "  \"packets\": " << packets << ",\n"
           << "  \"frames\": " << frames << ",\n"
           << "  \"discontinuities\": " << discontinuities << ",\n"
           << "  \"timestampErrors\": " << timestampErrors << ",\n"
           << "  \"silentPackets\": " << silentPackets << ",\n"
           << "  \"firstDevicePosition\": " << firstDevicePosition << ",\n"
           << "  \"lastDevicePosition\": " << lastDevicePosition << ",\n"
           << "  \"firstQpc100ns\": " << firstQpc100ns << ",\n"
           << "  \"lastQpc100ns\": " << lastQpc100ns << "\n"
           << "}\n";
}
}

int wmain(int argc, wchar_t** argv) {
    fs::path outputBase;
    fs::path stopFile;
    std::wstring captureMatch;
    bool listCaptureDevices = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--list-capture-devices") listCaptureDevices = true;
        else if (index + 1 < argc && argument == L"--output-base") outputBase = argv[++index];
        else if (index + 1 < argc && argument == L"--stop-file") stopFile = argv[++index];
        else if (index + 1 < argc && argument == L"--capture-name") captureMatch = argv[++index];
        else {
            std::cerr << "Unknown or incomplete argument: " << WideToUtf8(argument) << "\n";
            return 2;
        }
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::cerr << "Could not initialize WASAPI: " << HResultText(hr) << "\n";
        if (uninitialize) CoUninitialize();
        return 1;
    }
    if (listCaptureDevices) {
        try {
            ListCaptureEndpoints(enumerator.Get());
        } catch (const std::exception& error) {
            std::cerr << error.what() << "\n";
            if (uninitialize) CoUninitialize();
            return 1;
        }
        if (uninitialize) CoUninitialize();
        return 0;
    }
    if (outputBase.empty() || stopFile.empty()) {
        std::cerr << "Usage: StratoCamLoopbackDiagnostic --output-base PATH --stop-file PATH "
                     "[--capture-name SUBSTRING]\n";
        if (uninitialize) CoUninitialize();
        return 2;
    }

    const bool captureInput = !captureMatch.empty();
    const std::string role = captureInput ? "capture" : "loopback";
    const fs::path wavePath = outputBase.wstring() + L".wav";
    const fs::path csvPath = outputBase.wstring() + L".csv";
    const fs::path readyPath = outputBase.wstring() + L".ready.json";
    const fs::path summaryPath = outputBase.wstring() + L".json";
    std::error_code ignored;
    fs::create_directories(outputBase.parent_path(), ignored);
    fs::remove(stopFile, ignored);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    ComPtr<IMMDevice> endpoint;
    EndpointInfo endpointInfo;
    if (captureInput) {
        try {
            endpoint = SelectCaptureEndpoint(enumerator.Get(), captureMatch, endpointInfo, hr);
        } catch (const std::exception& error) {
            std::cerr << error.what() << "\n";
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            if (uninitialize) CoUninitialize();
            return 1;
        }
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
        if (SUCCEEDED(hr)) {
            endpointInfo.device = endpoint;
            endpointInfo.name = DeviceName(endpoint.Get());
            LPWSTR id = nullptr;
            if (SUCCEEDED(endpoint->GetId(&id)) && id) endpointInfo.id = id;
            if (id) CoTaskMemFree(id);
        }
    }

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClock> audioClock;
    WAVEFORMATEX* mix = nullptr;
    HANDLE event = nullptr;
    if (SUCCEEDED(hr)) {
        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && mix) {
        endpointInfo.mixSampleRate = mix->nSamplesPerSec;
        endpointInfo.mixChannels = mix->nChannels;
        endpointInfo.mixBitsPerSample = mix->wBitsPerSample;
        if (mix->nChannels == 0 || mix->nChannels > 8) hr = E_INVALIDARG;
    }
    const uint16_t channels = mix ? mix->nChannels : 0;
    const uint16_t blockAlign = channels * kBitsPerSample / 8;
    WAVEFORMATEX requested = Pcm48k(channels);
    if (SUCCEEDED(hr)) {
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (!captureInput) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1'000'000, 0, &requested, nullptr);
    }
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
    uint64_t deviceClockFrequency = 0;
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&audioClock));
    if (SUCCEEDED(hr)) hr = audioClock->GetFrequency(&deviceClockFrequency);
    if (SUCCEEDED(hr)) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);

    std::fstream wave(wavePath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    std::ofstream csv(csvPath, std::ios::trunc);
    if (SUCCEEDED(hr) && (!wave || !csv)) hr = E_FAIL;
    if (FAILED(hr)) {
        std::cerr << "Could not initialize WASAPI " << role << ": " << HResultText(hr) << "\n";
        if (mix) CoTaskMemFree(mix);
        if (event) CloseHandle(event);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (uninitialize) CoUninitialize();
        return 1;
    }

    WriteWaveHeader(wave, 0, channels);
    csv << "packet_index,wave_frame_start,frames,device_position,qpc_position_100ns,receive_qpc_ns,"
           "flags,discontinuity,timestamp_error,silent\n";
    const int64_t startedQpcNs = static_cast<int64_t>(std::llround(QpcNanoseconds()));
    hr = client->Start();
    if (FAILED(hr)) {
        std::cerr << "Could not start WASAPI " << role << ": " << HResultText(hr) << "\n";
        if (mix) CoTaskMemFree(mix);
        if (event) CloseHandle(event);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (uninitialize) CoUninitialize();
        return 1;
    }
    WriteState(readyPath, role, endpointInfo, wavePath, csvPath, startedQpcNs, deviceClockFrequency,
               channels, 0, 0, 0, 0, 0, 0, 0, 0, 0, false);

    uint64_t packets = 0;
    uint64_t framesWritten = 0;
    uint64_t discontinuities = 0;
    uint64_t timestampErrors = 0;
    uint64_t silentPackets = 0;
    uint64_t firstDevicePosition = 0;
    uint64_t lastDevicePosition = 0;
    uint64_t firstQpc100ns = 0;
    uint64_t lastQpc100ns = 0;
    std::vector<uint8_t> silence;
    auto lastFlush = std::chrono::steady_clock::now();
    while (SUCCEEDED(hr) && !fs::exists(stopFile)) {
        const DWORD wait = WaitForSingleObject(event, 500);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0) { hr = E_FAIL; break; }
        UINT32 available = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&available)) && available > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpc100ns = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc100ns);
            if (FAILED(hr)) break;
            const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            const bool timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const uint64_t receiveQpcNs = static_cast<uint64_t>(std::llround(QpcNanoseconds()));
            const size_t bytes = static_cast<size_t>(frames) * blockAlign;
            if (silent) {
                silence.assign(bytes, 0);
                wave.write(reinterpret_cast<const char*>(silence.data()), static_cast<std::streamsize>(bytes));
                ++silentPackets;
            } else {
                wave.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
            }
            csv << packets << ',' << framesWritten << ',' << frames << ',' << devicePosition << ','
                << qpc100ns << ',' << receiveQpcNs << ',' << flags << ',' << (discontinuity ? 1 : 0)
                << ',' << (timestampError ? 1 : 0) << ',' << (silent ? 1 : 0) << '\n';
            if (packets == 0) {
                firstDevicePosition = devicePosition;
                firstQpc100ns = qpc100ns;
            }
            lastDevicePosition = devicePosition;
            lastQpc100ns = qpc100ns;
            framesWritten += frames;
            ++packets;
            if (discontinuity) ++discontinuities;
            if (timestampError) ++timestampErrors;
            capture->ReleaseBuffer(frames);
        }
        if (std::chrono::steady_clock::now() - lastFlush >= std::chrono::seconds(1)) {
            wave.flush();
            csv.flush();
            lastFlush = std::chrono::steady_clock::now();
        }
    }

    client->Stop();
    const uint64_t dataBytes64 = framesWritten * blockAlign;
    if (dataBytes64 > UINT32_MAX - 36) hr = E_OUTOFMEMORY;
    else WriteWaveHeader(wave, static_cast<uint32_t>(dataBytes64), channels);
    wave.flush();
    csv.flush();
    WriteState(summaryPath, role, endpointInfo, wavePath, csvPath, startedQpcNs, deviceClockFrequency,
               channels, packets, framesWritten, discontinuities, timestampErrors, silentPackets,
               firstDevicePosition, lastDevicePosition, firstQpc100ns, lastQpc100ns, true);
    fs::remove(readyPath, ignored);
    if (mix) CoTaskMemFree(mix);
    if (event) CloseHandle(event);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (uninitialize) CoUninitialize();
    if (FAILED(hr)) {
        std::cerr << "WASAPI " << role << " capture failed: " << HResultText(hr) << "\n";
        return 1;
    }
    return 0;
}
