#include "VideoTransport.h"
#include "ControlClient.h"
#include "PipelineDiagnostics.h"

#include <limits>

namespace {
constexpr uint32_t kVideoMagic = 0x31564341;
constexpr uint32_t kProtocolVersion = 3;
constexpr uint32_t kStreamInfo = 4;
constexpr uint32_t kSyncMagic = 0x31534341;
constexpr uint32_t kSyncVersion = 1;
constexpr size_t kHeaderSize = 80;
constexpr double kMaximumPreDecoderAgeMs = 500.0;
constexpr int kStaleAccessUnitsBeforeRecovery = 3;

template <typename T>
T ReadLittle(const uint8_t* bytes, size_t offset) {
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

template <typename T>
void WriteLittle(uint8_t* bytes, size_t offset, T value) {
    std::memcpy(bytes + offset, &value, sizeof(T));
}

std::array<uint64_t, 32> CountNalTypes(const std::vector<uint8_t>& payload) {
    std::array<uint64_t, 32> counts{};
    size_t offset = 0;
    while (offset + 3 < payload.size()) {
        size_t start = payload.size();
        for (; offset + 3 < payload.size(); ++offset) {
            if (payload[offset] == 0 && payload[offset + 1] == 0 &&
                (payload[offset + 2] == 1 || (offset + 3 < payload.size() && payload[offset + 2] == 0 && payload[offset + 3] == 1))) {
                start = offset + (payload[offset + 2] == 1 ? 3 : 4);
                offset = start;
                break;
            }
        }
        if (start >= payload.size()) break;
        ++counts[payload[start] & 0x1f];
    }
    return counts;
}
}

VideoTransport::VideoTransport(PacketCallback packetCallback, InfoCallback infoCallback, StatusCallback statusCallback,
                               StateCallback stateCallback, RecoveryCallback recoveryCallback)
    : packetCallback_(std::move(packetCallback)), infoCallback_(std::move(infoCallback)),
      statusCallback_(std::move(statusCallback)), stateCallback_(std::move(stateCallback)),
      recoveryCallback_(std::move(recoveryCallback)) {}

VideoTransport::~VideoTransport() { Stop(); }

bool VideoTransport::Start() {
    if (running_.exchange(true)) return false;
    accessUnits_ = codecConfigs_ = spsCount_ = ppsCount_ = idrCount_ = 0;
    transportRecoveryCount_ = 0;
    state_.store(VideoTransportState::Starting);
    PipelineDiagnostics::Log("transport-state", "STARTING worker launched");
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&VideoTransport::Run, this);
    return true;
}

void VideoTransport::Stop() {
    running_.store(false);
    {
        std::lock_guard lock(socketMutex_);
        if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(socketMutex_);
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    state_.store(VideoTransportState::Stopped);
    PipelineDiagnostics::Log("transport-state", "STOPPED");
}

SOCKET VideoTransport::Connect() {
    for (int attempt = 0; attempt < 30 && running_.load(); ++attempt) {
        SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) throw std::runtime_error("Video socket creation failed");
        BOOL noDelay = TRUE;
        DWORD timeout = 10'000;
        setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(27184);
        inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
        if (connect(candidate, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0) return candidate;
        closesocket(candidate);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw std::runtime_error("Android video channel did not become available");
}

void VideoTransport::SendAll(SOCKET socketHandle, const void* bytes, size_t size) {
    const auto* source = static_cast<const char*>(bytes);
    size_t offset = 0;
    while (offset < size) {
        const int sent = send(socketHandle, source + offset, static_cast<int>(size - offset), 0);
        if (sent <= 0) throw std::runtime_error("Video socket send failed: " + std::to_string(WSAGetLastError()));
        offset += static_cast<size_t>(sent);
    }
}

void VideoTransport::ReadExactly(SOCKET socketHandle, void* bytes, size_t size) {
    auto* destination = static_cast<char*>(bytes);
    size_t offset = 0;
    while (offset < size && running_.load()) {
        const int received = recv(socketHandle, destination + offset, static_cast<int>(size - offset), 0);
        if (received <= 0) throw std::runtime_error("Video socket closed: " + std::to_string(WSAGetLastError()));
        offset += static_cast<size_t>(received);
    }
    if (offset != size) throw std::runtime_error("Video receive stopped");
}

void VideoTransport::SynchronizeClock(SOCKET socketHandle) {
    double bestNetworkRtt = std::numeric_limits<double>::max();
    double bestOffset = 0.0;
    for (int64_t sequence = 0; sequence < 9; ++sequence) {
        std::array<uint8_t, 16> request{};
        WriteLittle<uint32_t>(request.data(), 0, kSyncMagic);
        WriteLittle<uint32_t>(request.data(), 4, kSyncVersion);
        WriteLittle<int64_t>(request.data(), 8, sequence);
        const double windowsSend = QpcNanoseconds();
        SendAll(socketHandle, request.data(), request.size());
        std::array<uint8_t, 32> response{};
        ReadExactly(socketHandle, response.data(), response.size());
        const double windowsReceive = QpcNanoseconds();
        if (ReadLittle<uint32_t>(response.data(), 0) != kSyncMagic ||
            ReadLittle<uint32_t>(response.data(), 4) != kSyncVersion ||
            ReadLittle<int64_t>(response.data(), 8) != sequence) {
            throw std::runtime_error("Invalid Android clock synchronization response");
        }
        const double androidReceive = static_cast<double>(ReadLittle<int64_t>(response.data(), 16));
        const double androidSend = static_cast<double>(ReadLittle<int64_t>(response.data(), 24));
        const double networkRtt = std::max(0.0, (windowsReceive - windowsSend) - (androidSend - androidReceive));
        if (networkRtt < bestNetworkRtt) {
            bestNetworkRtt = networkRtt;
            bestOffset = ((windowsSend + windowsReceive) - (androidReceive + androidSend)) / 2.0;
        }
    }
    clockOffsetWindowsMinusAndroidNs_ = bestOffset;
    clockUncertaintyMs_ = bestNetworkRtt / 2'000'000.0;
}

void VideoTransport::Run() {
    int failedConnections = 0;
    while (running_.load()) {
        try {
        SOCKET active = Connect();
        {
            std::lock_guard lock(socketMutex_);
            socket_ = active;
        }
        SynchronizeClock(active);
        state_.store(VideoTransportState::Starting);
        statusCallback_("Video transport connected; waiting for H.264 access units");
        int consecutiveStaleAccessUnits = 0;
        bool recoverLiveEdge = false;
        while (running_.load()) {
            std::array<uint8_t, kHeaderSize> header{};
            ReadExactly(active, header.data(), header.size());
            if (ReadLittle<uint32_t>(header.data(), 0) != kVideoMagic ||
                ReadLittle<uint32_t>(header.data(), 4) != kProtocolVersion) {
                throw std::runtime_error("Unsupported Android video protocol");
            }
            const uint32_t flags = ReadLittle<uint32_t>(header.data(), 8);
            const uint32_t payloadSize = ReadLittle<uint32_t>(header.data(), 12);
            if (payloadSize > 16 * 1024 * 1024) throw std::runtime_error("Invalid video payload size");
            std::vector<uint8_t> payload(payloadSize);
            ReadExactly(active, payload.data(), payload.size());
            const double windowsReceive = QpcNanoseconds();
            if (flags & kStreamInfo) {
                const std::string json(payload.begin(), payload.end());
                StreamInfo info{};
                info.codec = "video/avc";
                info.width = static_cast<int>(JsonLite::Number(json, "width").value_or(0));
                info.height = static_cast<int>(JsonLite::Number(json, "height").value_or(0));
                info.fps = static_cast<int>(JsonLite::Number(json, "fps").value_or(0));
                info.bitrate = static_cast<int>(JsonLite::Number(json, "bitrate").value_or(0));
                info.sensorOrientation = static_cast<int>(JsonLite::Number(json, "sensorOrientationDegrees").value_or(0));
                info.captureTimestampSource = json.find("REALTIME") != std::string::npos ? "REALTIME" : "UNKNOWN";
                info.clockSyncUncertaintyMs = clockUncertaintyMs_;
                infoCallback_(info);
                continue;
            }
            EncodedAccessUnit packet{};
            packet.flags = flags;
            packet.payload = std::make_shared<const std::vector<uint8_t>>(std::move(payload));
            packet.timing.presentationTimeUs = ReadLittle<int64_t>(header.data(), 16);
            packet.timing.frameNumber = ReadLittle<int64_t>(header.data(), 24);
            packet.timing.captureTimestampNs = ReadLittle<int64_t>(header.data(), 32);
            packet.timing.encoderOutputTimestampNs = ReadLittle<int64_t>(header.data(), 48);
            packet.timing.androidSocketWriteTimestampNs = ReadLittle<int64_t>(header.data(), 56);
            packet.timing.accessUnitSequence = ReadLittle<uint64_t>(header.data(), 64);
            packet.timing.encodedQueueDepth = ReadLittle<int32_t>(header.data(), 72);
            packet.timing.queueRecoveryCount = ReadLittle<int32_t>(header.data(), 76);
            packet.timing.transportRecoveryCount = transportRecoveryCount_;
            packet.timing.windowsReceiveNs = windowsReceive;
            if (!(flags & kVideoFlagCodecConfig) && !packet.Bytes().empty() &&
                state_.exchange(VideoTransportState::Streaming) != VideoTransportState::Streaming) {
                failedConnections = 0;
                PipelineDiagnostics::Log("transport-state", "STREAMING first H.264 access unit received");
                if (stateCallback_) stateCallback_(true);
                statusCallback_("Video streaming; clock uncertainty +/-" +
                    std::to_string(clockUncertaintyMs_) + " ms");
            }
            if (!(flags & kVideoFlagCodecConfig) && packet.timing.captureTimestampNs > 0) {
                packet.timing.synchronizedCaptureWindowsNs = static_cast<int64_t>(std::llround(
                    static_cast<double>(packet.timing.captureTimestampNs) + clockOffsetWindowsMinusAndroidNs_));
                packet.timing.knownPreDecoderMs =
                    (windowsReceive - (static_cast<double>(packet.timing.captureTimestampNs) + clockOffsetWindowsMinusAndroidNs_)) /
                    1'000'000.0;
            }
            if (packet.timing.encoderOutputTimestampNs > 0) {
                packet.timing.encoderToWindowsReceiveMs =
                    (windowsReceive - (static_cast<double>(packet.timing.encoderOutputTimestampNs) +
                        clockOffsetWindowsMinusAndroidNs_)) / 1'000'000.0;
            }
            if (packet.timing.androidSocketWriteTimestampNs > 0) {
                packet.timing.socketToWindowsReceiveMs =
                    (windowsReceive - (static_cast<double>(packet.timing.androidSocketWriteTimestampNs) +
                        clockOffsetWindowsMinusAndroidNs_)) / 1'000'000.0;
            }
            if (!(flags & kVideoFlagCodecConfig) &&
                packet.timing.knownPreDecoderMs > kMaximumPreDecoderAgeMs) {
                ++consecutiveStaleAccessUnits;
            } else {
                consecutiveStaleAccessUnits = 0;
            }
            if (consecutiveStaleAccessUnits >= kStaleAccessUnitsBeforeRecovery) {
                ++transportRecoveryCount_;
                PipelineDiagnostics::Log(
                    "transport",
                    "drop reason=stale-live-edge auSequence=" +
                        std::to_string(packet.timing.accessUnitSequence) +
                        " knownPreDecoderMs=" + std::to_string(packet.timing.knownPreDecoderMs) +
                        " action=reconnect-for-SPS-PPS-IDR recovery=" +
                        std::to_string(transportRecoveryCount_));
                statusCallback_(
                    "Live edge exceeded 500 ms before decode; reconnecting for a fresh IDR");
                recoverLiveEdge = true;
                break;
            }
            const auto nals = CountNalTypes(packet.Bytes());
            spsCount_ += nals[7];
            ppsCount_ += nals[8];
            idrCount_ += nals[5];
            if (flags & kVideoFlagCodecConfig) ++codecConfigs_;
            else ++accessUnits_;
            if (accessUnits_ <= 3 || idrCount_ <= nals[5] || accessUnits_ % 30 == 0) {
                PipelineDiagnostics::Log(
                    "transport",
                    "au=" + std::to_string(accessUnits_) + " config=" + std::to_string(codecConfigs_) +
                    " nal{sps=" + std::to_string(spsCount_) + ",pps=" + std::to_string(ppsCount_) +
                    ",idr=" + std::to_string(idrCount_) + ",nonIdr=" + std::to_string(nals[1]) +
                    "} bytes=" + std::to_string(packet.Bytes().size()) +
                    " queue=" + std::to_string(packet.timing.encodedQueueDepth) +
                    " recoveries=" + std::to_string(packet.timing.queueRecoveryCount) + " -> consumers");
            }
            packetCallback_(std::move(packet));
        }
        {
            std::lock_guard lock(socketMutex_);
            if (socket_ != INVALID_SOCKET) {
                shutdown(socket_, SD_BOTH);
                closesocket(socket_);
                socket_ = INVALID_SOCKET;
            }
        }
        if (stateCallback_) stateCallback_(false);
        state_.store(VideoTransportState::Starting);
        if (recoverLiveEdge && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        break;
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(socketMutex_);
                if (socket_ != INVALID_SOCKET) {
                    shutdown(socket_, SD_BOTH);
                    closesocket(socket_);
                    socket_ = INVALID_SOCKET;
                }
            }
            if (stateCallback_) stateCallback_(false);
            if (!running_.load()) break;
            ++failedConnections;
            if (failedConnections >= 3) {
                state_.store(VideoTransportState::Error);
                PipelineDiagnostics::Log("transport-state", std::string("ERROR after 3 attempts: ") + error.what());
                statusCallback_(std::string("Video transport error after 3 attempts: ") + error.what());
                break;
            }
            state_.store(VideoTransportState::Starting);
            PipelineDiagnostics::Log("transport-state", std::string("STARTING reconnect after: ") + error.what());
            std::string recoveryStatus;
            if (recoveryCallback_ && !recoveryCallback_(recoveryStatus)) {
                statusCallback_("Video transport interrupted; ADB forward repair failed, retrying: " +
                    recoveryStatus);
            } else {
                statusCallback_("Video transport interrupted; rebuilding ADB forward and retrying (" +
                    std::to_string(failedConnections + 1) + "/3)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
    }
    running_.store(false);
    if (stateCallback_) stateCallback_(false);
    if (state_.load() != VideoTransportState::Error) state_.store(VideoTransportState::Stopped);
    std::lock_guard lock(socketMutex_);
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}
