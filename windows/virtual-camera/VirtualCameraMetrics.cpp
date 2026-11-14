#include "../shared/VirtualCameraFrameProtocol.h"

#include <algorithm>
#include <iostream>
#include <string>

int wmain() {
    wchar_t programData[MAX_PATH]{};
    GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    const std::wstring path = std::wstring(programData) + L"\\" + AndroidCamVirtualCamera::kRelativeFrameFile;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, AndroidCamVirtualCamera::kMappingBytes) : nullptr;
    auto* h = static_cast<AndroidCamVirtualCamera::SharedHeader*>(view);
    if (!AndroidCamVirtualCamera::ValidHeader(h)) return 2;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const bool recentConsumer = h->lastSubmitQpc > 0 && h->qpcFrequency > 0 &&
        now.QuadPart - h->lastSubmitQpc <= static_cast<LONGLONG>(h->qpcFrequency) * 2;
    const LONG rawActiveConsumers = h->activeConsumers;
    const LONG activeConsumers = recentConsumer ? std::max<LONG>(1, rawActiveConsumers) : 0;
    std::wcout << L"producer_active=" << h->producerActive << L" latest_sequence=" << h->latestSequence
               << L" published=" << h->publishedFrames << L" consumed=" << h->consumedFrames
               << L" producer_drops=" << h->producerDrops << L" consumer_drops=" << h->consumerDrops
               << L" black=" << h->blackFrames << L" attaches=" << h->consumerAttaches
               << L" detaches=" << h->consumerDetaches << L" active_consumers=" << activeConsumers
               << L" raw_active_consumers=" << rawActiveConsumers
               << L" reconnects=" << h->reconnections << L"\n";
    UnmapViewOfFile(view); CloseHandle(mapping); CloseHandle(file);
    return 0;
}
