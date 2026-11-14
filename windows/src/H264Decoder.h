#pragma once

#include "Common.h"
#include "DecoderAccessUnitQueue.h"
#include "VideoTransport.h"

#include <d3d11.h>
#include <condition_variable>
#include <unordered_map>
#include <wels/codec_api.h>

enum class DecodedFrameStorage {
    I420,
    D3D11Nv12
};

struct DecodedFrame {
    ~DecodedFrame() { SafeRelease(texture); }
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
    DecodedFrame() = default;

    int width{};
    int height{};
    int yStride{};
    int uvStride{};
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    DecodedFrameStorage storage{DecodedFrameStorage::I420};
    ID3D11Texture2D* texture{};
    UINT textureSubresource{};
    FrameTiming timing;
};

struct DecoderSnapshot {
    std::string name;
    bool hardware{};
    uint64_t queuedAccessUnits{};
    uint64_t queueHighWatermark{};
    uint64_t queueDrops{};
    uint64_t gopRecoveries{};
    uint64_t decodedFrames{};
};

class H264Decoder {
public:
    using FrameCallback = std::function<void(std::shared_ptr<DecodedFrame>)>;
    using StatusCallback = std::function<void(const std::string&)>;

    H264Decoder(FrameCallback frameCallback, StatusCallback statusCallback);
    ~H264Decoder();
    bool Start(int width, int height, int fps, ID3D11Device* d3dDevice);
    void Stop();
    void Decode(EncodedAccessUnit&& packet);
    DecoderSnapshot GetSnapshot() const;

private:
    void WorkerMain();
    bool StartMediaFoundation();
    void StopMediaFoundation();
    bool ProcessMediaFoundation(EncodedAccessUnit&& encoded);
    bool DrainMediaFoundationOutputs();
    bool PublishMediaFoundationSample(void* sample);
    bool StartOpenH264();
    void StopOpenH264();
    void ProcessOpenH264(EncodedAccessUnit&& encoded);
    void PublishFrame(std::shared_ptr<DecodedFrame> frame);

    FrameCallback frameCallback_;
    StatusCallback statusCallback_;
    int width_{};
    int height_{};
    int fps_{30};
    ID3D11Device* d3dDevice_{};
    mutable std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::condition_variable initializationChanged_;
    DecoderAccessUnitQueue queue_;
    std::thread worker_;
    bool running_{};
    bool initialized_{};
    bool initializationComplete_{};

    void* transform_{};
    void* deviceManager_{};
    bool mediaFoundationStarted_{};
    bool mediaFoundationHardware_{};
    std::string decoderName_;
    ISVCDecoder* openH264_{};
    std::unordered_map<int64_t, FrameTiming> pendingTimings_;
    int64_t nextTimestampToken_{1};
    uint64_t submittedAccessUnits_{};
    uint64_t decodeOutputs_{};
    uint64_t timingMisses_{};
    uint64_t publishedFrames_{};
};
