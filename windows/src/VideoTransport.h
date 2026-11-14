#pragma once

#include "Common.h"
#include "Metrics.h"

struct StreamInfo {
    std::string codec;
    int width{};
    int height{};
    int fps{};
    int bitrate{};
    int sensorOrientation{};
    std::string captureTimestampSource;
    double clockSyncUncertaintyMs{};
};

struct EncodedAccessUnit {
    uint32_t flags{};
    std::shared_ptr<const std::vector<uint8_t>> payload;
    FrameTiming timing;

    const std::vector<uint8_t>& Bytes() const {
        static const std::vector<uint8_t> empty;
        return payload ? *payload : empty;
    }
};

constexpr uint32_t kVideoFlagCodecConfig = 1;
constexpr uint32_t kVideoFlagKeyFrame = 2;

enum class VideoTransportState { Stopped, Starting, Streaming, Error };

inline const wchar_t* VideoTransportStateLabel(VideoTransportState state) {
    switch (state) {
        case VideoTransportState::Starting: return L"●  STARTING";
        case VideoTransportState::Streaming: return L"●  STREAMING";
        case VideoTransportState::Error: return L"●  ERROR";
        default: return L"●  STOPPED";
    }
}

inline bool ShouldAssignDecoderTimestamp(const EncodedAccessUnit& packet) {
    return !(packet.flags & kVideoFlagCodecConfig) && !packet.Bytes().empty();
}

class VideoTransport {
public:
    using PacketCallback = std::function<void(EncodedAccessUnit&&)>;
    using InfoCallback = std::function<void(const StreamInfo&)>;
    using StatusCallback = std::function<void(const std::string&)>;
    using StateCallback = std::function<void(bool)>;
    using RecoveryCallback = std::function<bool(std::string&)>;

    VideoTransport(PacketCallback packetCallback, InfoCallback infoCallback, StatusCallback statusCallback,
                   StateCallback stateCallback = {}, RecoveryCallback recoveryCallback = {});
    ~VideoTransport();
    bool Start();
    void Stop();
    bool Running() const { return running_.load(); }
    VideoTransportState State() const { return state_.load(); }

private:
    void Run();
    SOCKET Connect();
    void SynchronizeClock(SOCKET socket);
    static void SendAll(SOCKET socket, const void* bytes, size_t size);
    void ReadExactly(SOCKET socket, void* bytes, size_t size);

    PacketCallback packetCallback_;
    InfoCallback infoCallback_;
    StatusCallback statusCallback_;
    StateCallback stateCallback_;
    RecoveryCallback recoveryCallback_;
    std::atomic<bool> running_{false};
    std::atomic<VideoTransportState> state_{VideoTransportState::Stopped};
    std::thread thread_;
    std::mutex socketMutex_;
    SOCKET socket_{INVALID_SOCKET};
    double clockOffsetWindowsMinusAndroidNs_{};
    double clockUncertaintyMs_{};
    uint64_t accessUnits_{};
    uint64_t codecConfigs_{};
    uint64_t spsCount_{};
    uint64_t ppsCount_{};
    uint64_t idrCount_{};
    uint32_t transportRecoveryCount_{};
};
