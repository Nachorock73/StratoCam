// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <util/platform.h>

#include "ObsFrameProtocol.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <new>

OBS_DECLARE_MODULE()

using namespace AndroidCamObs;

namespace {

struct AndroidCamSource {
    obs_source_t* source{};
    HANDLE thread{};
    HANDLE stopEvent{};
    HANDLE frameEvent{};
    HANDLE mappingHandle{};
    HANDLE producerProcess{};
    void* mapping{};
    SharedHeader* header{};
    uint64_t lastSequence{};
    uint32_t lastWidth{};
    uint32_t lastHeight{};
    int32_t lastRotation{-1};
    bool inactiveSent{true};
};

void CloseTransport(AndroidCamSource* context) {
    if (context->header) {
        InterlockedCompareExchange(&context->header->consumerPid, 0, static_cast<LONG>(GetCurrentProcessId()));
    }
    if (context->mapping) UnmapViewOfFile(context->mapping);
    if (context->producerProcess) CloseHandle(context->producerProcess);
    if (context->frameEvent) CloseHandle(context->frameEvent);
    if (context->mappingHandle) CloseHandle(context->mappingHandle);
    context->mapping = nullptr;
    context->header = nullptr;
    context->producerProcess = nullptr;
    context->frameEvent = nullptr;
    context->mappingHandle = nullptr;
    context->lastSequence = 0;
    context->lastWidth = 0;
    context->lastHeight = 0;
    context->lastRotation = -1;
}

bool OpenTransport(AndroidCamSource* context) {
    context->mappingHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
    if (!context->mappingHandle) return false;
    context->mapping = MapViewOfFile(context->mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
    if (!context->mapping) { CloseTransport(context); return false; }
    context->header = static_cast<SharedHeader*>(context->mapping);
    if (!ValidHeader(context->header)) { CloseTransport(context); return false; }
    context->frameEvent = OpenEventW(SYNCHRONIZE, FALSE, kFrameEventName);
    if (!context->frameEvent) { CloseTransport(context); return false; }
    context->producerProcess = OpenProcess(SYNCHRONIZE, FALSE, context->header->producerPid);
    InterlockedExchange(&context->header->consumerPid, static_cast<LONG>(GetCurrentProcessId()));
    blog(LOG_INFO, "[StratoCam] Connected to producer PID %u", context->header->producerPid);
    return true;
}

bool ProducerAlive(const AndroidCamSource* context) {
    return !context->producerProcess || WaitForSingleObject(context->producerProcess, 0) == WAIT_TIMEOUT;
}

void Deactivate(AndroidCamSource* context) {
    if (!context->inactiveSent) {
        obs_source_output_video(context->source, nullptr);
        context->inactiveSent = true;
    }
}

bool OutputLatest(AndroidCamSource* context) {
    const uint64_t sequence = static_cast<uint64_t>(InterlockedCompareExchange64(&context->header->latestSequence, 0, 0));
    const LONG index = InterlockedCompareExchange(&context->header->latestSlot, 0, 0);
    if (!sequence || sequence == context->lastSequence || index < 0 || index >= static_cast<LONG>(kSlotCount)) return false;
    auto* slot = Slot(context->mapping, static_cast<uint32_t>(index));
    const LONG64 guard = InterlockedCompareExchange64(&slot->writeGuard, 0, 0);
    if ((guard & 1) != 0) return false;
    InterlockedIncrement(&slot->readers);
    MemoryBarrier();
    const LONG64 verifiedGuard = InterlockedCompareExchange64(&slot->writeGuard, 0, 0);
    if (verifiedGuard != guard || (verifiedGuard & 1) != 0 || slot->sequence != sequence ||
        slot->width == 0 || slot->height == 0 || slot->width > kMaxWidth || slot->height > kMaxHeight) {
        InterlockedDecrement(&slot->readers);
        return false;
    }

    const uint64_t skipped = context->lastSequence && sequence > context->lastSequence + 1
        ? sequence - context->lastSequence - 1 : 0;
    obs_source_frame frame{};
    frame.data[0] = const_cast<uint8_t*>(YPlane(slot));
    frame.data[1] = const_cast<uint8_t*>(UPlane(slot));
    frame.data[2] = const_cast<uint8_t*>(VPlane(slot));
    frame.linesize[0] = slot->yStride;
    frame.linesize[1] = slot->uvStride;
    frame.linesize[2] = slot->uvStride;
    frame.width = slot->width;
    frame.height = slot->height;
    frame.timestamp = os_gettime_ns();
    frame.format = VIDEO_FORMAT_I420;
    frame.full_range = false;
    video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420,
                                           frame.color_matrix, frame.color_range_min, frame.color_range_max);
    if (slot->width != context->lastWidth || slot->height != context->lastHeight ||
        slot->rotationDegrees != context->lastRotation) {
        const bool rotated = slot->rotationDegrees == 90 || slot->rotationDegrees == 270;
        blog(LOG_INFO, "[StratoCam] Video %ux%u rotation %d (effective %ux%u)",
             slot->width, slot->height, slot->rotationDegrees,
             rotated ? slot->height : slot->width, rotated ? slot->width : slot->height);
        context->lastWidth = slot->width;
        context->lastHeight = slot->height;
        context->lastRotation = slot->rotationDegrees;
    }
    obs_source_set_async_rotation(context->source, slot->rotationDegrees);
    obs_source_output_video(context->source, &frame);
    LARGE_INTEGER outputTicks{};
    QueryPerformanceCounter(&outputTicks);
    InterlockedExchange64(&context->header->lastObsFrameNumber, slot->frameNumber);
    InterlockedExchange64(&context->header->lastObsPublishQpc, slot->publishQpc);
    InterlockedExchange64(&context->header->lastObsKnownCaptureToPublishUs, slot->knownCaptureToPublishUs);
    InterlockedExchange64(&context->header->lastObsOutputQpc, outputTicks.QuadPart);
    InterlockedExchange64(&context->header->lastObsSequence, static_cast<LONG64>(sequence));
    InterlockedIncrement64(&context->header->obsDeliveredFrames);
    if (skipped) InterlockedAdd64(&context->header->obsSkippedFrames, static_cast<LONG64>(skipped));
    context->lastSequence = sequence;
    context->inactiveSent = false;
    InterlockedDecrement(&slot->readers);
    return true;
}

DWORD WINAPI SourceThread(void* opaque) {
    auto* context = static_cast<AndroidCamSource*>(opaque);
    HANDLE waits[2] = {context->stopEvent, nullptr};
    while (WaitForSingleObject(context->stopEvent, 0) != WAIT_OBJECT_0) {
        if (!context->header && !OpenTransport(context)) {
            if (WaitForSingleObject(context->stopEvent, 250) == WAIT_OBJECT_0) break;
            continue;
        }
        if (!ValidHeader(context->header) || !ProducerAlive(context)) {
            Deactivate(context);
            CloseTransport(context);
            continue;
        }
        waits[1] = context->frameEvent;
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) { CloseTransport(context); continue; }
        if (InterlockedCompareExchange(&context->header->producerActive, 0, 0) == 0) {
            Deactivate(context);
            continue;
        }
        OutputLatest(context);
    }
    Deactivate(context);
    CloseTransport(context);
    return 0;
}

const char* SourceName(void*) { return "StratoCam"; }

void* SourceCreate(obs_data_t*, obs_source_t* source) {
    auto* context = new (std::nothrow) AndroidCamSource{};
    if (!context) return nullptr;
    context->source = source;
    context->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!context->stopEvent) { delete context; return nullptr; }
    context->thread = CreateThread(nullptr, 0, SourceThread, context, 0, nullptr);
    if (!context->thread) {
        CloseHandle(context->stopEvent);
        delete context;
        return nullptr;
    }
    return context;
}

void SourceDestroy(void* data) {
    auto* context = static_cast<AndroidCamSource*>(data);
    if (!context) return;
    SetEvent(context->stopEvent);
    if (context->frameEvent) SetEvent(context->frameEvent);
    WaitForSingleObject(context->thread, INFINITE);
    CloseHandle(context->thread);
    CloseHandle(context->stopEvent);
    delete context;
}

} // namespace

bool obs_module_load(void) {
    obs_source_info sourceInfo{};
    sourceInfo.id = "android_cam_source";
    sourceInfo.type = OBS_SOURCE_TYPE_INPUT;
    sourceInfo.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    sourceInfo.get_name = SourceName;
    sourceInfo.create = SourceCreate;
    sourceInfo.destroy = SourceDestroy;
    obs_register_source(&sourceInfo);
    blog(LOG_INFO, "[StratoCam] OBS source registered (shared I420 protocol v%u)", kProtocolVersion);
    return true;
}

const char* obs_module_name(void) { return "StratoCam"; }
const char* obs_module_description(void) {
    return "Low-latency StratoCam I420 source over local shared memory";
}
