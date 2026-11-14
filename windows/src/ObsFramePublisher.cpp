#include "ObsFramePublisher.h"

#include <fstream>
#include <iomanip>

using namespace AndroidCamObs;

ObsFramePublisher::~ObsFramePublisher() { Shutdown(); }

int64_t ObsFramePublisher::QpcTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

bool ObsFramePublisher::Initialize(std::string& error) {
    Shutdown();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
    mappingHandle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(kMappingBytes), kMappingName);
    if (!mappingHandle_) { error = "Could not create OBS shared memory: " + LastErrorText(); return false; }
    const bool mappingAlreadyExists = GetLastError() == ERROR_ALREADY_EXISTS;
    mapping_ = MapViewOfFile(mappingHandle_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
    if (!mapping_) { error = "Could not map OBS shared memory: " + LastErrorText(); Shutdown(); return false; }
    if (mappingAlreadyExists && ValidHeader(static_cast<SharedHeader*>(mapping_))) {
        const DWORD existingPid = static_cast<SharedHeader*>(mapping_)->producerPid;
        HANDLE existingProcess = existingPid ? OpenProcess(SYNCHRONIZE, FALSE, existingPid) : nullptr;
        const bool existingProducerAlive = existingProcess && WaitForSingleObject(existingProcess, 0) == WAIT_TIMEOUT;
        if (existingProcess) CloseHandle(existingProcess);
        if (existingProducerAlive && existingPid != GetCurrentProcessId()) {
            error = "Another StratoCam instance already owns the OBS output";
            UnmapViewOfFile(mapping_);
            CloseHandle(mappingHandle_);
            mapping_ = nullptr;
            mappingHandle_ = nullptr;
            return false;
        }
    }
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, kFrameEventName);
    if (!frameEvent_) { error = "Could not create OBS frame event: " + LastErrorText(); Shutdown(); return false; }
    std::memset(mapping_, 0, kMappingBytes);
    header_ = static_cast<SharedHeader*>(mapping_);
    std::memcpy(header_->magic, kMagic, sizeof(kMagic));
    header_->version = kProtocolVersion;
    header_->headerBytes = static_cast<uint32_t>(kHeaderBytes);
    header_->slotCount = kSlotCount;
    header_->slotStride = static_cast<uint32_t>(kSlotStride);
    header_->maxWidth = kMaxWidth;
    header_->maxHeight = kMaxHeight;
    header_->producerPid = GetCurrentProcessId();
    header_->qpcFrequency = static_cast<uint64_t>(qpcFrequency_);
    header_->latestSlot = -1;
    header_->lastObsFrameNumber = -1;
    nextSequence_ = 1;
    lastAcknowledgedSequence_ = 0;
    return true;
}

void ObsFramePublisher::Shutdown() {
    StopStream();
    if (mapping_) UnmapViewOfFile(mapping_);
    if (frameEvent_) CloseHandle(frameEvent_);
    if (mappingHandle_) CloseHandle(mappingHandle_);
    mapping_ = nullptr;
    header_ = nullptr;
    frameEvent_ = nullptr;
    mappingHandle_ = nullptr;
}

void ObsFramePublisher::StartStream() {
    if (!header_) return;
    bool expected = false;
    if (!streamActive_.compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard lock(metricsMutex_);
        decodeToPublishMs_.clear();
        copyDurationMs_.clear();
        publishToObsMs_.clear();
        knownCaptureToObsMs_.clear();
        deliveries_.clear();
    }
    lastAcknowledgedSequence_ = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->lastObsSequence, 0, 0));
    InterlockedExchange64(&header_->publishedFrames, 0);
    InterlockedExchange64(&header_->producerDrops, 0);
    InterlockedExchange64(&header_->obsDeliveredFrames, 0);
    InterlockedExchange64(&header_->obsSkippedFrames, 0);
    InterlockedIncrement64(&header_->streamGeneration);
    InterlockedExchange(&header_->producerActive, 1);
    if (frameEvent_) SetEvent(frameEvent_);
}

void ObsFramePublisher::StopStream() {
    if (!header_) return;
    streamActive_.store(false);
    InterlockedExchange(&header_->producerActive, 0);
    if (frameEvent_) SetEvent(frameEvent_);
    CollectAcknowledgement();
}

bool ObsFramePublisher::Publish(DecodedFrame& frame, int rotationDegrees) {
    if (!streamActive_.load() || !header_ || frame.width <= 0 || frame.height <= 0 ||
        frame.width > static_cast<int>(kMaxWidth) || frame.height > static_cast<int>(kMaxHeight)) return false;
    CollectAcknowledgement();
    const uint64_t sequence = nextSequence_++;
    SlotHeader* target = nullptr;
    uint32_t targetIndex = 0;
    for (uint32_t attempt = 0; attempt < kSlotCount; ++attempt) {
        const uint32_t index = static_cast<uint32_t>((sequence + attempt) % kSlotCount);
        auto* slot = Slot(mapping_, index);
        LONG64 guard = InterlockedCompareExchange64(&slot->writeGuard, 0, 0);
        if ((guard & 1) != 0 || InterlockedCompareExchange(&slot->readers, 0, 0) != 0) continue;
        if (InterlockedCompareExchange64(&slot->writeGuard, guard + 1, guard) != guard) continue;
        if (InterlockedCompareExchange(&slot->readers, 0, 0) != 0) {
            InterlockedExchange64(&slot->writeGuard, guard + 2);
            continue;
        }
        target = slot;
        targetIndex = index;
        break;
    }
    if (!target) { InterlockedIncrement64(&header_->producerDrops); return false; }

    const int64_t copyStart = QpcTicks();
    std::memcpy(YPlane(target), frame.y.data(), frame.y.size());
    std::memcpy(UPlane(target), frame.u.data(), frame.u.size());
    std::memcpy(VPlane(target), frame.v.data(), frame.v.size());
    const int64_t publishTicks = QpcTicks();
    const double publishNs = static_cast<double>(publishTicks) * 1'000'000'000.0 / qpcFrequency_;
    target->sequence = sequence;
    target->frameNumber = frame.timing.frameNumber;
    target->width = static_cast<uint32_t>(frame.width);
    target->height = static_cast<uint32_t>(frame.height);
    target->yStride = static_cast<uint32_t>(frame.yStride);
    target->uvStride = static_cast<uint32_t>(frame.uvStride);
    target->rotationDegrees = rotationDegrees;
    target->publishQpc = publishTicks;
    const double knownCaptureToPublishMs = frame.timing.captureTimestampNs > 0
        ? frame.timing.knownPreDecoderMs + (publishNs - frame.timing.windowsReceiveNs) / 1'000'000.0
        : 0.0;
    target->knownCaptureToPublishUs = static_cast<int64_t>(std::llround(knownCaptureToPublishMs * 1000.0));
    frame.timing.obsPublishNs = publishNs;
    MemoryBarrier();
    InterlockedIncrement64(&target->writeGuard);
    InterlockedExchange(&header_->latestSlot, static_cast<LONG>(targetIndex));
    InterlockedExchange64(&header_->latestSequence, static_cast<LONG64>(sequence));
    InterlockedIncrement64(&header_->publishedFrames);
    SetEvent(frameEvent_);

    std::lock_guard lock(metricsMutex_);
    decodeToPublishMs_.push_back((publishNs - frame.timing.decoderOutputNs) / 1'000'000.0);
    copyDurationMs_.push_back(static_cast<double>(publishTicks - copyStart) * 1000.0 / qpcFrequency_);
    return true;
}

void ObsFramePublisher::CollectAcknowledgement() {
    if (!header_) return;
    const uint64_t sequence = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->lastObsSequence, 0, 0));
    if (!sequence || sequence == lastAcknowledgedSequence_) return;
    const int64_t publish = InterlockedCompareExchange64(&header_->lastObsPublishQpc, 0, 0);
    const int64_t output = InterlockedCompareExchange64(&header_->lastObsOutputQpc, 0, 0);
    const int64_t captureToPublishUs = InterlockedCompareExchange64(&header_->lastObsKnownCaptureToPublishUs, 0, 0);
    const int64_t frameNumber = InterlockedCompareExchange64(&header_->lastObsFrameNumber, 0, 0);
    lastAcknowledgedSequence_ = sequence;
    if (output <= publish || publish <= 0) return;
    const double deliveryMs = static_cast<double>(output - publish) * 1000.0 / qpcFrequency_;
    const double knownCaptureToObsMs = captureToPublishUs / 1000.0 + deliveryMs;
    std::lock_guard lock(metricsMutex_);
    publishToObsMs_.push_back(deliveryMs);
    knownCaptureToObsMs_.push_back(knownCaptureToObsMs);
    deliveries_.push_back({frameNumber, sequence, deliveryMs, knownCaptureToObsMs});
}

ObsPublisherSnapshot ObsFramePublisher::GetSnapshot() {
    CollectAcknowledgement();
    ObsPublisherSnapshot result;
    {
        std::lock_guard lock(metricsMutex_);
        result.decodeToPublish = Summarize(decodeToPublishMs_);
        result.copyDuration = Summarize(copyDurationMs_);
        result.publishToObs = Summarize(publishToObsMs_);
        result.knownCaptureToObs = Summarize(knownCaptureToObsMs_);
    }
    if (header_) {
        result.publishedFrames = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->publishedFrames, 0, 0));
        result.producerDrops = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->producerDrops, 0, 0));
        result.obsDeliveredFrames = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->obsDeliveredFrames, 0, 0));
        result.obsSkippedFrames = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->obsSkippedFrames, 0, 0));
        result.consumerPid = static_cast<DWORD>(InterlockedCompareExchange(&header_->consumerPid, 0, 0));
    }
    return result;
}

bool ObsFramePublisher::ExportCsv(const std::filesystem::path& path) {
    CollectAcknowledgement();
    std::lock_guard lock(metricsMutex_);
    if (deliveries_.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "frame_id,publish_sequence,publish_to_obs_output_ms,known_capture_to_obs_output_ms\n";
    output << std::fixed << std::setprecision(3);
    for (const auto& row : deliveries_)
        output << row.frameNumber << ',' << row.sequence << ',' << row.publishToObsMs << ',' << row.knownCaptureToObsMs << '\n';
    return true;
}
