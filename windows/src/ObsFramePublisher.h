#pragma once

#include "H264Decoder.h"
#include "Metrics.h"
#include "../shared/ObsFrameProtocol.h"

#include <atomic>
#include <filesystem>

struct ObsPublisherSnapshot {
    StageSummary decodeToPublish;
    StageSummary copyDuration;
    StageSummary publishToObs;
    StageSummary knownCaptureToObs;
    uint64_t publishedFrames{};
    uint64_t producerDrops{};
    uint64_t obsDeliveredFrames{};
    uint64_t obsSkippedFrames{};
    DWORD consumerPid{};
};

class ObsFramePublisher {
public:
    ObsFramePublisher() = default;
    ~ObsFramePublisher();
    bool Initialize(std::string& error);
    void Shutdown();
    void StartStream();
    void StopStream();
    bool Publish(DecodedFrame& frame, int rotationDegrees);
    bool HasActiveConsumer() const {
        return streamActive_.load() && header_ &&
               InterlockedCompareExchange(&header_->consumerPid, 0, 0) != 0;
    }
    ObsPublisherSnapshot GetSnapshot();
    bool ExportCsv(const std::filesystem::path& path);

private:
    struct DeliveryRecord {
        int64_t frameNumber{};
        uint64_t sequence{};
        double publishToObsMs{};
        double knownCaptureToObsMs{};
    };

    void CollectAcknowledgement();
    static int64_t QpcTicks();

    HANDLE mappingHandle_{};
    HANDLE frameEvent_{};
    void* mapping_{};
    AndroidCamObs::SharedHeader* header_{};
    int64_t qpcFrequency_{};
    uint64_t nextSequence_{1};
    uint64_t lastAcknowledgedSequence_{};
    std::atomic_bool streamActive_{};
    mutable std::mutex metricsMutex_;
    std::vector<double> decodeToPublishMs_;
    std::vector<double> copyDurationMs_;
    std::vector<double> publishToObsMs_;
    std::vector<double> knownCaptureToObsMs_;
    std::vector<DeliveryRecord> deliveries_;
};
