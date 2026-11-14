#pragma once

#include "Common.h"

#include <filesystem>
#include <fstream>
#include <iomanip>

struct StageSummary {
    size_t samples{};
    double averageMs{};
    double medianMs{};
    double maximumMs{};
    double p95Ms{};
    double p99Ms{};
};

inline StageSummary Summarize(std::vector<double> values) {
    StageSummary result{};
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    result.samples = values.size();
    double sum = 0.0;
    for (double value : values) sum += value;
    result.averageMs = sum / static_cast<double>(values.size());
    const size_t middle = values.size() / 2;
    result.medianMs = values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    result.maximumMs = values.back();
    result.p95Ms = values[static_cast<size_t>(std::floor(0.95 * static_cast<double>(values.size() - 1)))];
    result.p99Ms = values[static_cast<size_t>(std::floor(0.99 * static_cast<double>(values.size() - 1)))];
    return result;
}

struct FrameTiming {
    uint64_t accessUnitSequence{};
    int64_t frameNumber{-1};
    int64_t presentationTimeUs{};
    int64_t captureTimestampNs{};
    int64_t synchronizedCaptureWindowsNs{};
    int64_t encoderOutputTimestampNs{};
    int64_t androidSocketWriteTimestampNs{};
    int encodedQueueDepth{};
    int queueRecoveryCount{};
    uint32_t transportRecoveryCount{};
    uint32_t decoderQueueDepth{};
    uint64_t decoderQueueDrops{};
    uint64_t decoderGopRecoveries{};
    double windowsReceiveNs{};
    double decoderInputNs{};
    double decoderOutputNs{};
    double decodedFramePublishNs{};
    double d3dSubmitNs{};
    double obsPublishNs{};
    double renderSubmitNs{};
    double knownPreDecoderMs{};
    double decodeCallMs{};
    double encoderToWindowsReceiveMs{-1};
    double socketToWindowsReceiveMs{-1};
    double processingLatencyMs{};
    double liveFrameAgeMs{};
    uint64_t decodedFrameReplacementCount{};
};

class LatencyCollector {
public:
    void Add(const FrameTiming& timing) {
        std::lock_guard lock(mutex_);
        frames_.push_back(timing);
        if (timing.decoderOutputNs > timing.windowsReceiveNs) {
            receiveToDecode_.push_back((timing.decoderOutputNs - timing.windowsReceiveNs) / 1'000'000.0);
        }
        if (timing.decodeCallMs > 0) decodeCall_.push_back(timing.decodeCallMs);
        if (timing.renderSubmitNs > timing.decoderOutputNs) {
            decodeToRender_.push_back((timing.renderSubmitNs - timing.decoderOutputNs) / 1'000'000.0);
        }
        if (timing.obsPublishNs > timing.decoderOutputNs) {
            decodeToObsPublish_.push_back((timing.obsPublishNs - timing.decoderOutputNs) / 1'000'000.0);
        }
        if (timing.renderSubmitNs > timing.windowsReceiveNs) {
            receiveToRender_.push_back((timing.renderSubmitNs - timing.windowsReceiveNs) / 1'000'000.0);
        }
        if (timing.knownPreDecoderMs > 0) knownPreDecoder_.push_back(timing.knownPreDecoderMs);
        if (timing.liveFrameAgeMs > 0) liveFrameAge_.push_back(timing.liveFrameAgeMs);
        if (timing.encoderToWindowsReceiveMs >= 0) encoderToWindowsReceive_.push_back(timing.encoderToWindowsReceiveMs);
        if (timing.socketToWindowsReceiveMs >= 0) socketToWindowsReceive_.push_back(timing.socketToWindowsReceiveMs);
        if (timing.processingLatencyMs > 0) processingLatency_.push_back(timing.processingLatencyMs);
        latestLiveFrameAgeMs_ = timing.liveFrameAgeMs > 0 ? std::optional<double>(timing.liveFrameAgeMs) : std::nullopt;
        latestDecodeCallMs_ = timing.decodeCallMs;
        latestTransportMs_ = timing.encoderToWindowsReceiveMs;
    }

    struct Snapshot {
        StageSummary knownPreDecoder;
        StageSummary receiveToDecode;
        StageSummary decodeToRender;
        StageSummary receiveToRender;
        StageSummary decodeToObsPublish;
        StageSummary decodeCall;
        StageSummary liveFrameAge;
        StageSummary encoderToWindowsReceive;
        StageSummary socketToWindowsReceive;
        StageSummary processingLatency;
        std::optional<double> latestLiveFrameAgeMs;
        double latestDecodeCallMs{};
        double latestTransportMs{-1};
    };

    Snapshot Get() const {
        std::lock_guard lock(mutex_);
        return {Summarize(knownPreDecoder_), Summarize(receiveToDecode_), Summarize(decodeToRender_),
                Summarize(receiveToRender_), Summarize(decodeToObsPublish_), Summarize(decodeCall_),
                Summarize(liveFrameAge_), Summarize(encoderToWindowsReceive_), Summarize(socketToWindowsReceive_),
                Summarize(processingLatency_), latestLiveFrameAgeMs_, latestDecodeCallMs_, latestTransportMs_};
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        frames_.clear();
        knownPreDecoder_.clear();
        receiveToDecode_.clear();
        decodeToRender_.clear();
        receiveToRender_.clear();
        decodeToObsPublish_.clear();
        decodeCall_.clear();
        liveFrameAge_.clear();
        encoderToWindowsReceive_.clear();
        socketToWindowsReceive_.clear();
        processingLatency_.clear();
        latestLiveFrameAgeMs_.reset();
        latestDecodeCallMs_ = 0;
        latestTransportMs_ = -1;
    }

    bool ExportCsv(const std::filesystem::path& path) const {
        std::lock_guard lock(mutex_);
        if (frames_.empty()) return false;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::trunc);
        if (!output) return false;
        output << "access_unit_sequence,frame_id,presentation_us,capture_android_ns,capture_windows_ns,encoder_output_android_ns,"
                  "android_socket_write_ns,encoded_queue_depth,queue_recovery_count,transport_recovery_count,"
                  "decoder_queue_depth,decoder_queue_drops,decoder_gop_recoveries,windows_receive_ns,decoder_input_ns,"
                  "decoder_output_ns,decoded_frame_publish_ns,d3d_submit_ns,obs_publish_ns,render_submit_ns,known_predecoder_ms,"
                  "receive_to_decode_ms,decode_to_obs_publish_ms,decode_to_render_ms,receive_to_render_ms,"
                  "processing_latency_ms,live_frame_age_ms,encoder_to_windows_ms,socket_to_windows_ms,decode_call_ms,"
                  "decoded_frame_replacement_count\n";
        output << std::fixed << std::setprecision(3);
        for (const auto& timing : frames_) {
            const double receiveDecode = (timing.decoderOutputNs - timing.windowsReceiveNs) / 1'000'000.0;
            const double decodeObsPublish = timing.obsPublishNs > timing.decoderOutputNs
                ? (timing.obsPublishNs - timing.decoderOutputNs) / 1'000'000.0 : 0.0;
            const double decodeRender = (timing.renderSubmitNs - timing.decoderOutputNs) / 1'000'000.0;
            const double receiveRender = (timing.renderSubmitNs - timing.windowsReceiveNs) / 1'000'000.0;
            output << timing.accessUnitSequence << ',' << timing.frameNumber << ',' << timing.presentationTimeUs << ','
                   << timing.captureTimestampNs << ',' << timing.synchronizedCaptureWindowsNs << ','
                   << timing.encoderOutputTimestampNs << ','
                   << timing.androidSocketWriteTimestampNs << ',' << timing.encodedQueueDepth << ','
                   << timing.queueRecoveryCount << ',' << timing.transportRecoveryCount << ','
                   << timing.decoderQueueDepth << ',' << timing.decoderQueueDrops << ','
                   << timing.decoderGopRecoveries << ','
                   << timing.windowsReceiveNs << ',' << timing.decoderInputNs << ','
                   << timing.decoderOutputNs << ',' << timing.decodedFramePublishNs << ',' << timing.d3dSubmitNs << ','
                   << timing.obsPublishNs << ',' << timing.renderSubmitNs << ',' << timing.knownPreDecoderMs << ','
                   << receiveDecode << ',' << decodeObsPublish << ',' << decodeRender << ',' << receiveRender << ','
                   << timing.processingLatencyMs << ',' << timing.liveFrameAgeMs << ','
                   << timing.encoderToWindowsReceiveMs << ',' << timing.socketToWindowsReceiveMs << ','
                   << timing.decodeCallMs << ',' << timing.decodedFrameReplacementCount << '\n';
        }
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::vector<FrameTiming> frames_;
    std::vector<double> knownPreDecoder_;
    std::vector<double> receiveToDecode_;
    std::vector<double> decodeToRender_;
    std::vector<double> receiveToRender_;
    std::vector<double> decodeToObsPublish_;
    std::vector<double> decodeCall_;
    std::vector<double> liveFrameAge_;
    std::vector<double> encoderToWindowsReceive_;
    std::vector<double> socketToWindowsReceive_;
    std::vector<double> processingLatency_;
    std::optional<double> latestLiveFrameAgeMs_;
    double latestDecodeCallMs_{};
    double latestTransportMs_{-1};
};
