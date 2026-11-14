#include "H264Decoder.h"
#include "PipelineDiagnostics.h"

#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr DWORD kMftPollMilliseconds = 2;
constexpr int kMftRetryCount = 50;

std::string HResultText(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(result);
    return text.str();
}

bool IsKeyFrame(const EncodedAccessUnit& packet) {
    return (packet.flags & kVideoFlagKeyFrame) != 0;
}

IMFTransform* Transform(void* value) { return static_cast<IMFTransform*>(value); }
IMFDXGIDeviceManager* DeviceManager(void* value) { return static_cast<IMFDXGIDeviceManager*>(value); }
}

H264Decoder::H264Decoder(FrameCallback frameCallback, StatusCallback statusCallback)
    : frameCallback_(std::move(frameCallback)), statusCallback_(std::move(statusCallback)) {}

H264Decoder::~H264Decoder() { Stop(); }

bool H264Decoder::Start(int width, int height, int fps, ID3D11Device* d3dDevice) {
    Stop();
    if (!d3dDevice || width <= 0 || height <= 0 || fps <= 0) return false;
    width_ = width;
    height_ = height;
    fps_ = fps;
    d3dDevice_ = d3dDevice;
    d3dDevice_->AddRef();
    {
        std::lock_guard lock(queueMutex_);
        running_ = true;
        initialized_ = false;
        initializationComplete_ = false;
        queue_.Reset();
        submittedAccessUnits_ = decodeOutputs_ = timingMisses_ = publishedFrames_ = 0;
        nextTimestampToken_ = 1;
        decoderName_.clear();
    }
    worker_ = std::thread(&H264Decoder::WorkerMain, this);
    std::unique_lock lock(queueMutex_);
    initializationChanged_.wait_for(lock, std::chrono::seconds(8), [this] { return initializationComplete_; });
    if (!initialized_) {
        lock.unlock();
        Stop();
        return false;
    }
    return true;
}

void H264Decoder::Stop() {
    {
        std::lock_guard lock(queueMutex_);
        running_ = false;
        queue_.Clear();
    }
    queueChanged_.notify_all();
    if (worker_.joinable()) worker_.join();
    SafeRelease(d3dDevice_);
}

void H264Decoder::Decode(EncodedAccessUnit&& packet) {
    std::lock_guard lock(queueMutex_);
    if (!running_) return;
    const auto result = queue_.Push(std::move(packet));
    if (result == DecoderQueuePushResult::RecoveredOnKeyFrame) {
        PipelineDiagnostics::Log("decoder-queue", "recovered with SPS/PPS + IDR");
    } else if (result == DecoderQueuePushResult::BacklogDiscarded) {
        PipelineDiagnostics::Log("decoder-queue", "DROP reason=decoder-backlog; waiting for IDR");
    }
    queueChanged_.notify_one();
}

DecoderSnapshot H264Decoder::GetSnapshot() const {
    std::lock_guard lock(queueMutex_);
    return {decoderName_, mediaFoundationHardware_, static_cast<uint64_t>(queue_.Size()), queue_.HighWatermark(),
            queue_.Drops(), queue_.GopRecoveries(), publishedFrames_};
}

void H264Decoder::WorkerMain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(com);
    bool ready = false;
    if (comInitialized && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        mediaFoundationStarted_ = true;
        ready = StartMediaFoundation();
    }
    if (!ready) ready = StartOpenH264();
    {
        std::lock_guard lock(queueMutex_);
        initialized_ = ready;
        initializationComplete_ = true;
    }
    initializationChanged_.notify_all();
    if (ready) {
        for (;;) {
            EncodedAccessUnit packet;
            {
                std::unique_lock lock(queueMutex_);
                queueChanged_.wait(lock, [this] { return !running_ || !queue_.Empty(); });
                if (!running_ && queue_.Empty()) break;
                packet = queue_.Pop();
            }
            if (transform_) {
                if (!ProcessMediaFoundation(std::move(packet))) {
                    statusCallback_("Media Foundation decoder failed; switching to embedded OpenH264 fallback");
                    StopMediaFoundation();
                    if (!StartOpenH264()) break;
                    std::lock_guard lock(queueMutex_);
                    queue_.DiscardUntilKeyFrame();
                }
            } else {
                ProcessOpenH264(std::move(packet));
            }
        }
    }
    StopMediaFoundation();
    StopOpenH264();
    if (mediaFoundationStarted_) {
        MFShutdown();
        mediaFoundationStarted_ = false;
    }
    if (comInitialized) CoUninitialize();
}

bool H264Decoder::StartMediaFoundation() {
    char forceSoftware[8]{};
    if (GetEnvironmentVariableA("ANDROID_CAM_FORCE_SOFTWARE_DECODER", forceSoftware,
                                static_cast<DWORD>(std::size(forceSoftware))) > 0) {
        PipelineDiagnostics::Log("decoder", "Media Foundation disabled by diagnostic fallback override");
        return false;
    }
    MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    HRESULT result = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo, nullptr, &activations, &activationCount);
    ComPtr<IMFTransform> transform;
    bool enumeratedHardware = false;
    if (SUCCEEDED(result) && activationCount > 0) {
        wchar_t* friendlyName = nullptr;
        UINT32 friendlyLength = 0;
        if (SUCCEEDED(activations[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendlyName, &friendlyLength))) {
            decoderName_ = WideToUtf8(std::wstring(friendlyName, friendlyLength));
            CoTaskMemFree(friendlyName);
        }
        result = activations[0]->ActivateObject(IID_PPV_ARGS(&transform));
        enumeratedHardware = SUCCEEDED(result);
    }
    for (UINT32 index = 0; index < activationCount; ++index) activations[index]->Release();
    CoTaskMemFree(activations);
    if (!transform) {
        result = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
        decoderName_ = "Microsoft H.264 Video Decoder MFT";
    }
    if (FAILED(result) || !transform) return false;

    ComPtr<IMFAttributes> attributes;
    UINT32 d3dAware = FALSE;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
    }
    if (!d3dAware) {
        PipelineDiagnostics::Log("decoder", decoderName_ + " is not D3D11-aware");
        return false;
    }

    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> manager;
    result = MFCreateDXGIDeviceManager(&resetToken, &manager);
    if (SUCCEEDED(result)) result = manager->ResetDevice(d3dDevice_, resetToken);
    if (SUCCEEDED(result)) {
        result = transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(manager.Get()));
    }
    if (FAILED(result)) {
        PipelineDiagnostics::Log("decoder", "D3D11 manager rejected: " + HResultText(result));
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = MFCreateMediaType(&inputType);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(result)) result = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
    if (SUCCEEDED(result)) result = MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(result)) result = transform->SetInputType(0, inputType.Get(), 0);
    if (FAILED(result)) {
        PipelineDiagnostics::Log("decoder", "H.264 input type rejected: " + HResultText(result));
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    for (DWORD index = 0; SUCCEEDED(transform->GetOutputAvailableType(0, index, &outputType)); ++index) {
        GUID subtype{};
        if (SUCCEEDED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) break;
        outputType.Reset();
    }
    if (!outputType) return false;
    result = transform->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(result)) {
        PipelineDiagnostics::Log("decoder", "NV12 output type rejected: " + HResultText(result));
        return false;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    result = transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(result) || !(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        PipelineDiagnostics::Log("decoder", "MFT does not provide D3D11 output samples");
        return false;
    }
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    transform_ = transform.Detach();
    deviceManager_ = manager.Detach();
    {
        std::lock_guard lock(queueMutex_);
        mediaFoundationHardware_ = enumeratedHardware;
    }
    PipelineDiagnostics::Log("decoder", "Media Foundation D3D11/NV12 initialized: " + decoderName_);
    statusCallback_("Media Foundation D3D11 decoder ready · " + decoderName_);
    return true;
}

void H264Decoder::StopMediaFoundation() {
    if (transform_) {
        Transform(transform_)->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        Transform(transform_)->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        Transform(transform_)->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        Transform(transform_)->Release();
        transform_ = nullptr;
    }
    if (deviceManager_) {
        DeviceManager(deviceManager_)->Release();
        deviceManager_ = nullptr;
    }
    pendingTimings_.clear();
}

bool H264Decoder::ProcessMediaFoundation(EncodedAccessUnit&& encoded) {
    if (!transform_) return false;
    ++submittedAccessUnits_;
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = MFCreateSample(&sample);
    const auto& bytes = encoded.Bytes();
    if (SUCCEEDED(result)) result = MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer);
    BYTE* destination = nullptr;
    DWORD maximum = 0;
    if (SUCCEEDED(result)) result = buffer->Lock(&destination, &maximum, nullptr);
    if (SUCCEEDED(result)) {
        std::memcpy(destination, bytes.data(), bytes.size());
        buffer->Unlock();
        result = buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    }
    if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
    if (ShouldAssignDecoderTimestamp(encoded)) {
        const int64_t token = nextTimestampToken_++;
        encoded.timing.decoderInputNs = QpcNanoseconds();
        pendingTimings_[token] = encoded.timing;
        sample->SetSampleTime(token);
        sample->SetSampleDuration(10'000'000LL / fps_);
    }
    if (IsKeyFrame(encoded)) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (FAILED(result)) return false;

    for (int attempt = 0; attempt < kMftRetryCount; ++attempt) {
        result = Transform(transform_)->ProcessInput(0, sample.Get(), 0);
        if (SUCCEEDED(result)) break;
        if (result != MF_E_NOTACCEPTING) return false;
        DrainMediaFoundationOutputs();
        std::this_thread::sleep_for(std::chrono::milliseconds(kMftPollMilliseconds));
    }
    if (FAILED(result)) return false;
    DrainMediaFoundationOutputs();
    return true;
}

bool H264Decoder::DrainMediaFoundationOutputs() {
    if (!transform_) return false;
    bool produced = false;
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output{};
        DWORD status = 0;
        const HRESULT result = Transform(transform_)->ProcessOutput(0, 1, &output, &status);
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT || result == E_UNEXPECTED) return produced;
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            for (DWORD index = 0; SUCCEEDED(Transform(transform_)->GetOutputAvailableType(0, index, &type)); ++index) {
                GUID subtype{};
                if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) break;
                type.Reset();
            }
            if (!type || FAILED(Transform(transform_)->SetOutputType(0, type.Get(), 0))) return false;
            continue;
        }
        if (FAILED(result)) {
            PipelineDiagnostics::Log("decoder", "ProcessOutput failed: " + HResultText(result));
            if (output.pSample) output.pSample->Release();
            if (output.pEvents) output.pEvents->Release();
            return false;
        }
        if (output.pSample) {
            produced = PublishMediaFoundationSample(output.pSample) || produced;
            output.pSample->Release();
        }
        if (output.pEvents) output.pEvents->Release();
    }
}

bool H264Decoder::PublishMediaFoundationSample(void* sampleValue) {
    auto* sample = static_cast<IMFSample*>(sampleValue);
    LONGLONG token = 0;
    sample->GetSampleTime(&token);
    auto timing = pendingTimings_.find(token);
    FrameTiming outputTiming{};
    if (timing != pendingTimings_.end()) {
        outputTiming = timing->second;
        pendingTimings_.erase(timing);
    } else {
        ++timingMisses_;
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer.As(&dxgiBuffer))) {
        PipelineDiagnostics::Log("decoder", "Media Foundation output was not a DXGI surface");
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) return false;
    UINT subresource = 0;
    dxgiBuffer->GetSubresourceIndex(&subresource);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_NV12) return false;

    const double decodedNs = QpcNanoseconds();
    auto frame = std::make_shared<DecodedFrame>();
    frame->width = width_;
    frame->height = height_;
    frame->storage = DecodedFrameStorage::D3D11Nv12;
    frame->texture = texture.Detach();
    frame->textureSubresource = subresource;
    frame->timing = outputTiming;
    frame->timing.decoderOutputNs = decodedNs;
    frame->timing.decodeCallMs = outputTiming.decoderInputNs > 0
        ? (decodedNs - outputTiming.decoderInputNs) / 1'000'000.0 : 0.0;
    {
        std::lock_guard lock(queueMutex_);
        mediaFoundationHardware_ = true;
    }
    ++decodeOutputs_;
    PublishFrame(std::move(frame));
    return true;
}

bool H264Decoder::StartOpenH264() {
    if (WelsCreateDecoder(&openH264_) != 0 || !openH264_) return false;
    SDecodingParam parameters{};
    parameters.uiTargetDqLayer = 0xff;
    parameters.eEcActiveIdc = ERROR_CON_DISABLE;
    parameters.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (openH264_->Initialize(&parameters) != cmResultSuccess) {
        WelsDestroyDecoder(openH264_);
        openH264_ = nullptr;
        return false;
    }
    OpenH264Version version{};
    WelsGetCodecVersionEx(&version);
    const std::string name = "OpenH264 " + std::to_string(version.uMajor) + "." +
        std::to_string(version.uMinor) + "." + std::to_string(version.uRevision);
    {
        std::lock_guard lock(queueMutex_);
        decoderName_ = name;
        mediaFoundationHardware_ = false;
    }
    PipelineDiagnostics::Log("decoder", name + " fallback initialized on decoder worker");
    statusCallback_(name + " software fallback ready");
    return true;
}

void H264Decoder::StopOpenH264() {
    if (openH264_) {
        openH264_->Uninitialize();
        WelsDestroyDecoder(openH264_);
        openH264_ = nullptr;
    }
    pendingTimings_.clear();
}

void H264Decoder::ProcessOpenH264(EncodedAccessUnit&& encoded) {
    if (!openH264_) return;
    ++submittedAccessUnits_;
    const double decodeStartNs = QpcNanoseconds();
    unsigned char* planes[3]{};
    SBufferInfo bufferInfo{};
    int64_t assignedTimestamp = 0;
    if (ShouldAssignDecoderTimestamp(encoded)) {
        bufferInfo.uiInBsTimeStamp = nextTimestampToken_++;
        assignedTimestamp = bufferInfo.uiInBsTimeStamp;
        pendingTimings_[assignedTimestamp] = encoded.timing;
    }
    const DECODING_STATE state = openH264_->DecodeFrameNoDelay(
        encoded.Bytes().data(), static_cast<int>(encoded.Bytes().size()), planes, &bufferInfo);
    if (bufferInfo.iBufferStatus != 1 || !planes[0]) return;
    ++decodeOutputs_;
    auto timing = pendingTimings_.find(bufferInfo.uiOutYuvTimeStamp);
    if (timing == pendingTimings_.end() && assignedTimestamp != 0) timing = pendingTimings_.find(assignedTimestamp);
    FrameTiming outputTiming = encoded.timing;
    if (timing != pendingTimings_.end()) {
        outputTiming = timing->second;
        pendingTimings_.erase(timing);
    } else {
        ++timingMisses_;
    }
    const double decodedNs = QpcNanoseconds();
    auto output = std::make_shared<DecodedFrame>();
    output->width = bufferInfo.UsrData.sSystemBuffer.iWidth;
    output->height = bufferInfo.UsrData.sSystemBuffer.iHeight;
    output->yStride = output->width;
    output->uvStride = (output->width + 1) / 2;
    const int chromaHeight = (output->height + 1) / 2;
    output->y.resize(static_cast<size_t>(output->yStride) * output->height);
    output->u.resize(static_cast<size_t>(output->uvStride) * chromaHeight);
    output->v.resize(static_cast<size_t>(output->uvStride) * chromaHeight);
    for (int row = 0; row < output->height; ++row) {
        std::memcpy(output->y.data() + static_cast<size_t>(row) * output->yStride,
                    planes[0] + static_cast<size_t>(row) * bufferInfo.UsrData.sSystemBuffer.iStride[0], output->width);
    }
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(output->u.data() + static_cast<size_t>(row) * output->uvStride,
                    planes[1] + static_cast<size_t>(row) * bufferInfo.UsrData.sSystemBuffer.iStride[1], output->uvStride);
        std::memcpy(output->v.data() + static_cast<size_t>(row) * output->uvStride,
                    planes[2] + static_cast<size_t>(row) * bufferInfo.UsrData.sSystemBuffer.iStride[1], output->uvStride);
    }
    output->timing = outputTiming;
    output->timing.decoderInputNs = decodeStartNs;
    output->timing.decoderOutputNs = decodedNs;
    output->timing.decodeCallMs = (decodedNs - decodeStartNs) / 1'000'000.0;
    PublishFrame(std::move(output));
    (void)state;
}

void H264Decoder::PublishFrame(std::shared_ptr<DecodedFrame> frame) {
    uint64_t published = 0;
    {
        std::lock_guard lock(queueMutex_);
        published = ++publishedFrames_;
    }
    frame->timing.decodedFramePublishNs = QpcNanoseconds();
    if (published <= 3 || published % 30 == 0) {
        PipelineDiagnostics::Log(
            "decoder",
            std::string(frame->storage == DecodedFrameStorage::D3D11Nv12 ? "D3D11 NV12" : "I420") +
                " produced+published=" + std::to_string(published) + " size=" +
                std::to_string(frame->width) + "x" + std::to_string(frame->height));
    }
    frameCallback_(std::move(frame));
}
