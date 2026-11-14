#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "ComPtr.h"
#include "../shared/VirtualCameraFrameProtocol.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <iostream>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    int seconds = 0;
    bool portrait = false;
    bool fourK = false;
    std::wstring dumpPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--portrait") portrait = true;
        else if (argument == L"--4k") fourK = true;
        else if (argument == L"--seconds" && i + 1 < argc) seconds = std::max(0, _wtoi(argv[++i]));
        else if (argument == L"--dump" && i + 1 < argc) dumpPath = argv[++i];
    }
    const UINT32 width = fourK ? (portrait ? 2160u : 3840u) : (portrait ? 1080u : 1920u);
    const UINT32 height = fourK ? (portrait ? 3840u : 2160u) : (portrait ? 1920u : 1080u);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { CoUninitialize(); return 1; }
    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.Put(), 1);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices{};
    UINT32 count{};
    hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    ComPtr<IMFActivate> selected;
    for (UINT32 i = 0; SUCCEEDED(hr) && i < count; ++i) {
        wchar_t* name{}; UINT32 length{};
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) {
            std::wcout << L"device=" << name << L"\n";
            if (std::wstring(name).find(L"StratoCam") != std::wstring::npos) selected = ComPtr<IMFActivate>(devices[i]);
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    if (!selected) { std::wcerr << L"StratoCam Virtual Camera not enumerated\n"; hr = MF_E_NOT_FOUND; }
    ComPtr<IMFMediaSource> source;
    const wchar_t* failedStage = L"enumeration";
    if (SUCCEEDED(hr)) {
        failedStage = L"ActivateObject";
        hr = selected->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void**>(source.Put()));
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) {
        failedStage = L"MFCreateSourceReaderFromMediaSource";
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, reader.Put());
    }
    if (SUCCEEDED(hr)) {
        for (DWORD index = 0; ; ++index) {
            ComPtr<IMFMediaType> nativeType;
            const HRESULT typeHr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, nativeType.Put());
            if (typeHr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(typeHr)) { std::wcout << L"native_type_error=0x" << std::hex << typeHr << std::dec << L"\n"; break; }
            UINT32 nativeWidth{}, nativeHeight{}, rateNumerator{}, rateDenominator{};
            MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);
            MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &rateNumerator, &rateDenominator);
            std::wcout << L"native_type=" << nativeWidth << L"x" << nativeHeight << L"@"
                       << (rateDenominator ? rateNumerator / rateDenominator : 0) << L"\n";
        }
    }
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(type.Put());
    if (SUCCEEDED(hr)) type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1);
    if (SUCCEEDED(hr)) {
        failedStage = L"SetCurrentMediaType";
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
    }
    const auto started = std::chrono::steady_clock::now();
    uint64_t samples = 0;
    uint64_t nonBlackSamples = 0;
    double averageYSum = 0.0;
    bool dumped = false;
    while (SUCCEEDED(hr) && (seconds > 0
               ? std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds)
               : samples < 10)) {
        DWORD stream{}, flags{}; LONGLONG timestamp{}; ComPtr<IMFSample> sample;
        failedStage = L"ReadSample";
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, sample.Put());
        if (SUCCEEDED(hr) && sample) {
            ComPtr<IMFMediaBuffer> buffer;
            DWORD bytes{};
            sample->ConvertToContiguousBuffer(buffer.Put());
            buffer->GetCurrentLength(&bytes);
            BYTE* data{}; DWORD maximum{}, current{}; double averageY{};
            if (SUCCEEDED(buffer->Lock(&data, &maximum, &current))) {
                uint64_t sum{};
                const DWORD yBytes = std::min<DWORD>(width * height, current);
                for (DWORD p = 0; p < yBytes; p += 256) sum += data[p];
                averageY = yBytes ? static_cast<double>(sum) / ((yBytes + 255) / 256) : 0.0;
                if (!dumped && !dumpPath.empty() && averageY > 16.5 && current == width * height * 3u / 2u) {
                    std::ofstream output(std::filesystem::path(dumpPath), std::ios::binary);
                    output.write(reinterpret_cast<const char*>(data), current);
                    dumped = output.good();
                }
                buffer->Unlock();
            }
            ++samples;
            if (averageY > 16.5) ++nonBlackSamples;
            averageYSum += averageY;
            if (seconds == 0 || samples == 1)
                std::wcout << L"sample=" << samples << L" bytes=" << bytes << L" average_y=" << averageY
                           << L" timestamp=" << timestamp << L" flags=" << flags << L"\n";
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::wcout << L"summary resolution=" << width << L"x" << height << L" samples=" << samples
               << L" elapsed_s=" << elapsed << L" fps=" << (elapsed > 0 ? samples / elapsed : 0.0)
               << L" non_black=" << nonBlackSamples
               << L" average_y=" << (samples ? averageYSum / samples : 0.0)
               << L" dumped=" << (dumped ? L"yes" : L"no") << L"\n";
    if (FAILED(hr)) std::wcerr << L"probe failed at " << failedStage << L": 0x" << std::hex << hr << L"\n";
    if (source) source->Shutdown();
    wchar_t programData[MAX_PATH]{};
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring framePath = std::wstring(programData) + L"\\" + AndroidCamVirtualCamera::kRelativeFrameFile;
    HANDLE file = CreateFileW(framePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, AndroidCamVirtualCamera::kMappingBytes) : nullptr;
        auto* header = static_cast<AndroidCamVirtualCamera::SharedHeader*>(view);
        if (AndroidCamVirtualCamera::ValidHeader(header)) {
            std::wcout << L"metrics published=" << header->publishedFrames << L" consumed=" << header->consumedFrames
                       << L" producer_drops=" << header->producerDrops << L" consumer_drops=" << header->consumerDrops
                       << L" black=" << header->blackFrames << L" reconnects=" << header->reconnections << L"\n";
        }
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); CloseHandle(file);
    }
    MFShutdown(); CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
