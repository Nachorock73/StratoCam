#pragma once

#include "H264Decoder.h"
#include "Metrics.h"
#include "../shared/VirtualCameraFrameProtocol.h"

#include <filesystem>

struct VirtualCameraPublisherSnapshot {
    StageSummary decodeToPublish;
    StageSummary copyDuration;
    StageSummary publishToSubmit;
    StageSummary knownCaptureToSubmit;
    uint64_t publishedFrames{};
    uint64_t producerDrops{};
    uint64_t consumedFrames{};
    uint64_t consumerDrops{};
    uint64_t blackFrames{};
    uint64_t consumerAttaches{};
    uint64_t consumerDetaches{};
    uint64_t reconnections{};
    uint32_t activeConsumers{};
};

class VirtualCameraFramePublisher {
public:
    ~VirtualCameraFramePublisher();
    bool Initialize(std::string& error);
    void Shutdown();
    void StartStream();
    void StopStream();
    bool Publish(DecodedFrame& frame, int rotationDegrees);
    bool HasActiveConsumer() const {
        return streamActive_.load() && header_ &&
               InterlockedCompareExchange(&header_->activeConsumers, 0, 0) > 0;
    }
    VirtualCameraPublisherSnapshot GetSnapshot();
    bool ExportCsv(const std::filesystem::path& path);

private:
    struct DeliveryRecord {
        int64_t frameNumber{};
        uint64_t sequence{};
        double decodeToPublishMs{};
        double copyDurationMs{};
        double publishToSubmitMs{};
        double knownCaptureToSubmitMs{};
    };
    void CollectAcknowledgement();
    static int64_t QpcTicks();
    static std::filesystem::path FrameFilePath();

    HANDLE fileHandle_{INVALID_HANDLE_VALUE};
    HANDLE mappingHandle_{};
    void* mapping_{};
    AndroidCamVirtualCamera::SharedHeader* header_{};
    int64_t qpcFrequency_{};
    uint64_t nextSequence_{1};
    uint64_t lastAcknowledgedSequence_{};
    std::atomic_bool streamActive_{};
    std::mutex metricsMutex_;
    std::vector<double> decodeToPublishMs_;
    std::vector<double> copyDurationMs_;
    std::vector<double> publishToSubmitMs_;
    std::vector<double> knownCaptureToSubmitMs_;
    std::vector<DeliveryRecord> deliveries_;
};
