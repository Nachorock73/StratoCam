#include "VirtualCameraFramePublisher.h"

#include <fstream>
#include <iomanip>

using namespace AndroidCamVirtualCamera;

VirtualCameraFramePublisher::~VirtualCameraFramePublisher() { Shutdown(); }

int64_t VirtualCameraFramePublisher::QpcTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::filesystem::path VirtualCameraFramePublisher::FrameFilePath() {
    wchar_t programData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::filesystem::path root = length && length < MAX_PATH ? programData : L"C:\\ProgramData";
    return root / kRelativeFrameFile;
}

bool VirtualCameraFramePublisher::Initialize(std::string& error) {
    Shutdown();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
    const auto path = FrameFilePath();
    fileHandle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle_ == INVALID_HANDLE_VALUE) {
        error = "Virtual camera transport is not installed (" + LastErrorText() + ")";
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(fileHandle_, &size) || size.QuadPart < static_cast<LONGLONG>(kMappingBytes)) {
        error = "Virtual camera frame file has an invalid size";
        Shutdown();
        return false;
    }
    mappingHandle_ = CreateFileMappingW(fileHandle_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!mappingHandle_) { error = "Could not map virtual camera transport: " + LastErrorText(); Shutdown(); return false; }
    mapping_ = MapViewOfFile(mappingHandle_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
    if (!mapping_) { error = "Could not open virtual camera transport: " + LastErrorText(); Shutdown(); return false; }

    header_ = static_cast<SharedHeader*>(mapping_);
    const bool existingProtocol = ValidHeader(header_);
    if (!existingProtocol) std::memset(mapping_, 0, kMappingBytes);
    std::memcpy(header_->magic, kMagic, sizeof(kMagic));
    header_->version = kProtocolVersion;
    header_->headerBytes = static_cast<uint32_t>(kHeaderBytes);
    header_->slotCount = kSlotCount;
    header_->slotStride = static_cast<uint32_t>(kSlotStride);
    header_->maxWidth = kMaxWidth;
    header_->maxHeight = kMaxHeight;
    header_->producerPid = GetCurrentProcessId();
    header_->qpcFrequency = static_cast<uint64_t>(qpcFrequency_);
    if (!existingProtocol) {
        header_->latestSlot = -1;
        header_->lastConsumedFrameNumber = -1;
    }
    InterlockedExchange(&header_->producerActive, 0);
    nextSequence_ = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->latestSequence, 0, 0)) + 1;
    lastAcknowledgedSequence_ = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->lastConsumedSequence, 0, 0));
    FlushViewOfFile(mapping_, kHeaderBytes);
    return true;
}

void VirtualCameraFramePublisher::Shutdown() {
    StopStream();
    if (mapping_) UnmapViewOfFile(mapping_);
    if (mappingHandle_) CloseHandle(mappingHandle_);
    if (fileHandle_ != INVALID_HANDLE_VALUE) CloseHandle(fileHandle_);
    mapping_ = nullptr;
    header_ = nullptr;
    mappingHandle_ = nullptr;
    fileHandle_ = INVALID_HANDLE_VALUE;
}

void VirtualCameraFramePublisher::StartStream() {
    if (!header_) return;
    bool expected = false;
    if (!streamActive_.compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard lock(metricsMutex_);
        decodeToPublishMs_.clear();
        copyDurationMs_.clear();
        publishToSubmitMs_.clear();
        knownCaptureToSubmitMs_.clear();
        deliveries_.clear();
    }
    lastAcknowledgedSequence_ = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->lastConsumedSequence, 0, 0));
    InterlockedExchange64(&header_->publishedFrames, 0);
    InterlockedExchange64(&header_->producerDrops, 0);
    InterlockedIncrement64(&header_->streamGeneration);
    InterlockedExchange(&header_->producerActive, 1);
}

void VirtualCameraFramePublisher::StopStream() {
    if (!header_) return;
    streamActive_.store(false);
    InterlockedExchange(&header_->producerActive, 0);
    CollectAcknowledgement();
}

bool VirtualCameraFramePublisher::Publish(DecodedFrame& frame, int rotationDegrees) {
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
    target->rotationDegrees = ((rotationDegrees % 360) + 360) % 360;
    target->publishQpc = publishTicks;
    const double knownCaptureToPublishMs = frame.timing.captureTimestampNs > 0
        ? frame.timing.knownPreDecoderMs + (publishNs - frame.timing.windowsReceiveNs) / 1'000'000.0
        : 0.0;
    const double decodeToPublishMs = (publishNs - frame.timing.decoderOutputNs) / 1'000'000.0;
    const double copyDurationMs = static_cast<double>(publishTicks - copyStart) * 1000.0 / qpcFrequency_;
    target->knownCaptureToPublishUs = static_cast<int64_t>(std::llround(knownCaptureToPublishMs * 1000.0));
    target->decodeToPublishUs = static_cast<int64_t>(std::llround(decodeToPublishMs * 1000.0));
    target->copyDurationUs = static_cast<int64_t>(std::llround(copyDurationMs * 1000.0));
    MemoryBarrier();
    InterlockedIncrement64(&target->writeGuard);
    InterlockedExchange(&header_->latestSlot, static_cast<LONG>(targetIndex));
    InterlockedExchange64(&header_->latestSequence, static_cast<LONG64>(sequence));
    InterlockedIncrement64(&header_->publishedFrames);

    std::lock_guard lock(metricsMutex_);
    decodeToPublishMs_.push_back(decodeToPublishMs);
    copyDurationMs_.push_back(copyDurationMs);
    return true;
}

void VirtualCameraFramePublisher::CollectAcknowledgement() {
    if (!header_) return;
    const uint64_t sequence = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->lastConsumedSequence, 0, 0));
    if (!sequence || sequence == lastAcknowledgedSequence_) return;
    const int64_t publish = InterlockedCompareExchange64(&header_->lastPublishQpc, 0, 0);
    const int64_t submit = InterlockedCompareExchange64(&header_->lastSubmitQpc, 0, 0);
    const int64_t captureToPublishUs = InterlockedCompareExchange64(&header_->lastKnownCaptureToPublishUs, 0, 0);
    const int64_t decodeToPublishUs = InterlockedCompareExchange64(&header_->lastDecodeToPublishUs, 0, 0);
    const int64_t copyDurationUs = InterlockedCompareExchange64(&header_->lastCopyDurationUs, 0, 0);
    const int64_t frameNumber = InterlockedCompareExchange64(&header_->lastConsumedFrameNumber, 0, 0);
    lastAcknowledgedSequence_ = sequence;
    if (submit <= publish || publish <= 0) return;
    const double deliveryMs = static_cast<double>(submit - publish) * 1000.0 / qpcFrequency_;
    const double knownCaptureToSubmitMs = captureToPublishUs / 1000.0 + deliveryMs;
    std::lock_guard lock(metricsMutex_);
    publishToSubmitMs_.push_back(deliveryMs);
    knownCaptureToSubmitMs_.push_back(knownCaptureToSubmitMs);
    deliveries_.push_back({frameNumber, sequence, decodeToPublishUs / 1000.0,
                           copyDurationUs / 1000.0, deliveryMs, knownCaptureToSubmitMs});
}

VirtualCameraPublisherSnapshot VirtualCameraFramePublisher::GetSnapshot() {
    CollectAcknowledgement();
    VirtualCameraPublisherSnapshot result;
    {
        std::lock_guard lock(metricsMutex_);
        result.decodeToPublish = Summarize(decodeToPublishMs_);
        result.copyDuration = Summarize(copyDurationMs_);
        result.publishToSubmit = Summarize(publishToSubmitMs_);
        result.knownCaptureToSubmit = Summarize(knownCaptureToSubmitMs_);
    }
    if (header_) {
        result.publishedFrames = InterlockedCompareExchange64(&header_->publishedFrames, 0, 0);
        result.producerDrops = InterlockedCompareExchange64(&header_->producerDrops, 0, 0);
        result.consumedFrames = InterlockedCompareExchange64(&header_->consumedFrames, 0, 0);
        result.consumerDrops = InterlockedCompareExchange64(&header_->consumerDrops, 0, 0);
        result.blackFrames = InterlockedCompareExchange64(&header_->blackFrames, 0, 0);
        result.consumerAttaches = InterlockedCompareExchange64(&header_->consumerAttaches, 0, 0);
        result.consumerDetaches = InterlockedCompareExchange64(&header_->consumerDetaches, 0, 0);
        result.reconnections = InterlockedCompareExchange64(&header_->reconnections, 0, 0);
        const int64_t lastSubmit = InterlockedCompareExchange64(&header_->lastSubmitQpc, 0, 0);
        const bool recentConsumer = lastSubmit > 0 && QpcTicks() - lastSubmit <= qpcFrequency_ * 2;
        result.activeConsumers = recentConsumer
            ? static_cast<uint32_t>(std::max<LONG>(1, InterlockedCompareExchange(&header_->activeConsumers, 0, 0)))
            : 0;
    }
    return result;
}

bool VirtualCameraFramePublisher::ExportCsv(const std::filesystem::path& path) {
    CollectAcknowledgement();
    std::lock_guard lock(metricsMutex_);
    if (deliveries_.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "frame_id,publish_sequence,decode_to_publish_ms,shared_copy_ms,"
              "publish_to_virtual_camera_submit_ms,known_capture_to_virtual_camera_submit_ms\n";
    output << std::fixed << std::setprecision(3);
    for (const auto& row : deliveries_)
        output << row.frameNumber << ',' << row.sequence << ',' << row.decodeToPublishMs << ','
               << row.copyDurationMs << ',' << row.publishToSubmitMs << ',' << row.knownCaptureToSubmitMs << '\n';
    return true;
}
