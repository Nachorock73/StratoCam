#pragma once

#include "AudioCaptureTypes.h"
#include "Common.h"
#include "RecordingOrientation.h"
#include "VideoTransport.h"

#include <condition_variable>
#include <deque>

enum class DirectRecorderState { Idle, WaitingForKeyFrame, Recording, Failed };

struct DirectRecorderSnapshot {
    DirectRecorderState state{DirectRecorderState::Idle};
    std::filesystem::path path;
    std::string error;
    uint64_t packetsWritten{};
    uint64_t bytesWritten{};
    uint64_t queueDepth{};
    uint64_t queueHighWatermark{};
    uint64_t queueDrops{};
    uint64_t gopRecoveries{};
    uint64_t prerollSkipped{};
    uint64_t timestampErrors{};
    int64_t durationUs{};
    bool audioEnabled{};
    std::wstring audioDeviceName;
    std::string audioCodec;
    uint64_t audioPacketsWritten{};
    uint64_t audioBytesWritten{};
    uint64_t audioQueueDepth{};
    uint64_t audioQueueHighWatermark{};
    uint64_t audioQueueDrops{};
    uint64_t audioOverruns{};
    uint64_t audioUnderruns{};
    uint64_t audioTimestampErrors{};
    int64_t videoFirstPtsUs{-1};
    int64_t videoLastPtsUs{-1};
    int64_t audioFirstPtsUs{-1};
    int64_t audioLastPtsUs{-1};
    double audioClockDriftFirstMs{};
    double audioClockDriftLastMs{};

    bool Active() const {
        return state == DirectRecorderState::WaitingForKeyFrame || state == DirectRecorderState::Recording;
    }
};

class DirectH264Recorder {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    explicit DirectH264Recorder(StatusCallback statusCallback);
    ~DirectH264Recorder();

    bool Start(const std::filesystem::path& path, const std::filesystem::path& metricsPath,
               const StreamInfo& info, const std::optional<AudioCaptureFormat>& audioFormat,
               int audioSyncOffsetMs, int rotationDegrees, std::string& error);
    void Stop();
    void ResetCodecConfig();
    void Enqueue(const EncodedAccessUnit& packet);
    void EnqueueAudio(AudioCaptureChunk&& chunk);
    DirectRecorderSnapshot Snapshot() const;

private:
    struct QueuedAccessUnit { EncodedAccessUnit packet; double enqueuedNs{}; uint32_t queueDepth{}; };
    struct QueuedAudioChunk { AudioCaptureChunk chunk; double enqueuedNs{}; uint32_t queueDepth{}; };
    struct VideoMetric {
        uint64_t accessUnitSequence{};
        int64_t sourcePresentationUs{};
        int64_t captureWindowsNs{};
        int64_t commonPresentationUs{};
        double enqueuedNs{};
        double dequeuedNs{};
        double writeCompleteNs{};
        uint32_t queueDepth{};
        uint64_t payloadBytes{};
        uint64_t cumulativeDrops{};
    };
    struct AudioMetric {
        uint64_t devicePosition{};
        int64_t captureQpcNs{};
        int64_t commonPresentationUs{};
        double clockDriftMs{};
        uint32_t frames{};
        uint32_t queueDepth{};
        double enqueuedNs{};
        double dequeuedNs{};
        uint64_t cumulativeDrops{};
        uint64_t cumulativeOverruns{};
        uint64_t cumulativeUnderruns{};
        uint64_t cumulativeTimestampErrors{};
        bool discontinuity{};
        bool timestampError{};
    };

    void WorkerMain();
    bool OpenMuxer(const EncodedAccessUnit& codecConfig, const QueuedAccessUnit& firstVideo,
                   const std::optional<QueuedAudioChunk>& firstAudio, std::string& error);
    bool ConfigureAudioEncoder(std::string& error);
    bool WriteVideoPacket(const QueuedAccessUnit& queued, std::string& error);
    bool ProcessAudioChunk(const QueuedAudioChunk& queued, std::string& error);
    bool EncodeAudioFrame(int samples, std::string& error);
    bool DrainAudioEncoder(std::string& error);
    bool FlushAudio(std::string& error);
    void CloseMuxer(bool writeTrailer);
    void ExportMetrics();
    int64_t VideoCaptureWindowsNs(const EncodedAccessUnit& packet) const;
    int64_t AudioCaptureWindowsNs(const AudioCaptureChunk& chunk) const;
    static bool ContainsNalType(const std::vector<uint8_t>& bytes, uint8_t type);

    static constexpr size_t kMaximumVideoQueueDepth = 180;
    static constexpr size_t kMaximumAudioQueueDepth = 512;

    StatusCallback statusCallback_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<QueuedAccessUnit> videoQueue_;
    std::deque<QueuedAudioChunk> audioQueue_;
    std::optional<EncodedAccessUnit> codecConfig_;
    std::thread worker_;
    bool accepting_{};
    bool stopRequested_{};
    bool waitingForKeyFrame_{};
    bool recordingStarted_{};
    DirectRecorderState state_{DirectRecorderState::Idle};
    StreamInfo streamInfo_;
    std::optional<AudioCaptureFormat> audioFormat_;
    int audioSyncOffsetMs_{};
    int rotationDegrees_{};
    int64_t armedWindowsNs_{};
    int64_t commonOriginWindowsNs_{};
    std::filesystem::path path_;
    std::filesystem::path metricsPath_;
    std::filesystem::path audioMetricsPath_;
    std::string error_;
    std::string audioCodecName_;
    uint64_t packetsWritten_{};
    uint64_t bytesWritten_{};
    uint64_t queueHighWatermark_{};
    uint64_t queueDrops_{};
    uint64_t gopRecoveries_{};
    uint64_t prerollSkipped_{};
    uint64_t timestampErrors_{};
    int64_t durationUs_{};
    int64_t videoFirstPtsUs_{-1};
    int64_t videoLastPtsUs_{-1};
    uint64_t audioPacketsWritten_{};
    uint64_t audioBytesWritten_{};
    uint64_t audioQueueHighWatermark_{};
    uint64_t audioQueueDrops_{};
    uint64_t audioOverruns_{};
    uint64_t audioUnderruns_{};
    uint64_t audioTimestampErrors_{};
    uint64_t audioOverrunBaseline_{};
    uint64_t audioUnderrunBaseline_{};
    uint64_t audioTimestampErrorBaseline_{};
    int64_t audioFirstPtsUs_{-1};
    int64_t audioLastPtsUs_{-1};
    int64_t audioAnchorQpcNs_{};
    int64_t audioBasePtsSamples_{};
    int64_t audioSamplesEncoded_{};
    uint64_t audioCapturedFrames_{};
    double audioClockDriftFirstMs_{};
    double audioClockDriftLastMs_{};
    std::vector<VideoMetric> videoMetrics_;
    std::vector<AudioMetric> audioMetrics_;
    void* formatContext_{};
    void* videoStream_{};
    void* audioStream_{};
    void* audioEncoderContext_{};
    void* audioResampler_{};
    void* audioFifo_{};
};
