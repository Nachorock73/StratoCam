#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "ComPtr.h"
#include "MfVirtualCameraCompat.h"
#include "../shared/VirtualCameraFrameProtocol.h"

#include <mferror.h>
#include <propvarutil.h>
#include <ks.h>
#include <ksmedia.h>
#include <devicetopology.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

using namespace AndroidCamVirtualCamera;

namespace {

std::atomic<long> g_objectCount{};
std::atomic<long> g_serverLocks{};

template <typename T>
HRESULT CopyInterface(T* value, void** result) {
    if (!result) return E_POINTER;
    *result = value;
    value->AddRef();
    return S_OK;
}

std::wstring FrameFilePath() {
    wchar_t programData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    std::wstring result = length && length < MAX_PATH ? programData : L"C:\\ProgramData";
    result += L"\\";
    result += kRelativeFrameFile;
    return result;
}

int64_t QpcTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

HRESULT AddLegacySensorProfile(IMFAttributes* attributes) {
    if (!attributes) return E_POINTER;
    using CreateCollection = HRESULT (WINAPI*)(IMFSensorProfileCollectionCompat**);
    using CreateProfile = HRESULT (WINAPI*)(REFGUID, UINT32, LPCWSTR, IMFSensorProfileCompat**);
    HMODULE library = LoadLibraryW(L"mfsensorgroup.dll");
    if (!library) return HRESULT_FROM_WIN32(GetLastError());
    auto createCollection = reinterpret_cast<CreateCollection>(GetProcAddress(library, "MFCreateSensorProfileCollection"));
    auto createProfile = reinterpret_cast<CreateProfile>(GetProcAddress(library, "MFCreateSensorProfile"));
    if (!createCollection || !createProfile) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ComPtr<IMFSensorProfileCollectionCompat> collection;
    ComPtr<IMFSensorProfileCompat> profile;
    HRESULT hr = createCollection(collection.Put());
    if (SUCCEEDED(hr)) hr = createProfile(KSCAMERAPROFILE_Legacy_Compat, 0, nullptr, profile.Put());
    if (SUCCEEDED(hr)) hr = profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
    if (SUCCEEDED(hr)) hr = collection->AddProfile(profile.Get());
    if (SUCCEEDED(hr)) hr = attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION_Compat, collection.Get());
    return hr;
}

class SharedFrameReader {
public:
    ~SharedFrameReader() { Close(); }

    void Attach() {
        EnsureOpen();
    }
    void Detach() {
        if (!attached_) return;
        if (header_ && ValidHeader(header_)) {
            InterlockedIncrement64(&header_->consumerDetaches);
            InterlockedDecrement(&header_->activeConsumers);
        }
        attached_ = false;
    }

    bool CopyLatestToNv12(BYTE* output, DWORD outputBytes, uint32_t outputWidth,
                          uint32_t outputHeight, bool& discontinuity) {
        if (!output || outputBytes < outputWidth * outputHeight * 3u / 2u) return false;
        FillBlack(output, outputWidth, outputHeight);
        if (!EnsureOpen()) return false;
        if (!ValidHeader(header_)) { Close(); return false; }
        if (InterlockedCompareExchange(&header_->producerActive, 0, 0) == 0) return false;
        const uint64_t sequence = static_cast<uint64_t>(InterlockedCompareExchange64(&header_->latestSequence, 0, 0));
        const LONG index = InterlockedCompareExchange(&header_->latestSlot, 0, 0);
        if (!sequence || index < 0 || index >= static_cast<LONG>(kSlotCount)) return false;
        auto* slot = Slot(mapping_, static_cast<uint32_t>(index));
        const LONG64 guard = InterlockedCompareExchange64(&slot->writeGuard, 0, 0);
        if ((guard & 1) != 0) return false;
        InterlockedIncrement(&slot->readers);
        MemoryBarrier();
        const LONG64 verified = InterlockedCompareExchange64(&slot->writeGuard, 0, 0);
        bool valid = verified == guard && !(verified & 1) && slot->sequence == sequence &&
                     slot->width > 0 && slot->height > 0 && slot->width <= kMaxWidth && slot->height <= kMaxHeight;
        if (valid) {
            const int64_t now = QpcTicks();
            const int64_t frequency = static_cast<int64_t>(header_->qpcFrequency);
            valid = frequency > 0 && now >= slot->publishQpc &&
                    (now - slot->publishQpc) * 1000 <= frequency * kStaleFrameMilliseconds;
        }
        if (valid) {
            ConvertI420ToNv12(*slot, output, outputWidth, outputHeight);
            const uint64_t skipped = lastSequence_ && sequence > lastSequence_ + 1 ? sequence - lastSequence_ - 1 : 0;
            if (skipped) InterlockedAdd64(&header_->consumerDrops, static_cast<LONG64>(skipped));
            discontinuity = discontinuity || skipped || (lastGeneration_ && lastGeneration_ != static_cast<uint64_t>(header_->streamGeneration));
            lastSequence_ = sequence;
            lastGeneration_ = static_cast<uint64_t>(header_->streamGeneration);
            const int64_t submit = QpcTicks();
            InterlockedExchange64(&header_->lastConsumedSequence, static_cast<LONG64>(sequence));
            InterlockedExchange64(&header_->lastConsumedFrameNumber, slot->frameNumber);
            InterlockedExchange64(&header_->lastPublishQpc, slot->publishQpc);
            InterlockedExchange64(&header_->lastKnownCaptureToPublishUs, slot->knownCaptureToPublishUs);
            InterlockedExchange64(&header_->lastDecodeToPublishUs, slot->decodeToPublishUs);
            InterlockedExchange64(&header_->lastCopyDurationUs, slot->copyDurationUs);
            InterlockedExchange64(&header_->lastSubmitQpc, submit);
            InterlockedIncrement64(&header_->consumedFrames);
        }
        InterlockedDecrement(&slot->readers);
        return valid;
    }

    void CountBlackFrame() {
        if (header_ && ValidHeader(header_)) InterlockedIncrement64(&header_->blackFrames);
    }

private:
    static void FillBlack(BYTE* output, uint32_t width, uint32_t height) {
        const size_t yBytes = static_cast<size_t>(width) * height;
        std::memset(output, 16, yBytes);
        std::memset(output + yBytes, 128, yBytes / 2);
    }

    static void SourceCoordinate(int rotation, uint32_t effectiveX, uint32_t effectiveY,
                                 uint32_t sourceWidth, uint32_t sourceHeight,
                                 uint32_t& sourceX, uint32_t& sourceY) {
        switch (rotation) {
        case 90: sourceX = effectiveY; sourceY = sourceHeight - 1 - effectiveX; break;
        case 180: sourceX = sourceWidth - 1 - effectiveX; sourceY = sourceHeight - 1 - effectiveY; break;
        case 270: sourceX = sourceWidth - 1 - effectiveY; sourceY = effectiveX; break;
        default: sourceX = effectiveX; sourceY = effectiveY; break;
        }
        sourceX = std::min(sourceX, sourceWidth - 1);
        sourceY = std::min(sourceY, sourceHeight - 1);
    }

    static void ConvertI420ToNv12(const SlotHeader& slot, BYTE* output,
                                  uint32_t outputWidth, uint32_t outputHeight) {
        const uint32_t sourceWidth = slot.width;
        const uint32_t sourceHeight = slot.height;
        const int rotation = ((slot.rotationDegrees % 360) + 360) % 360;
        const bool quarterTurn = rotation == 90 || rotation == 270;
        const uint32_t effectiveWidth = quarterTurn ? sourceHeight : sourceWidth;
        const uint32_t effectiveHeight = quarterTurn ? sourceWidth : sourceHeight;
        const double scale = std::min(static_cast<double>(outputWidth) / effectiveWidth,
                                      static_cast<double>(outputHeight) / effectiveHeight);
        uint32_t contentWidth = std::max(2u, static_cast<uint32_t>(std::floor(effectiveWidth * scale)) & ~1u);
        uint32_t contentHeight = std::max(2u, static_cast<uint32_t>(std::floor(effectiveHeight * scale)) & ~1u);
        contentWidth = std::min(contentWidth, outputWidth);
        contentHeight = std::min(contentHeight, outputHeight);
        const uint32_t left = ((outputWidth - contentWidth) / 2u) & ~1u;
        const uint32_t top = ((outputHeight - contentHeight) / 2u) & ~1u;
        const uint8_t* sourceYPlane = YPlane(&slot);
        const uint8_t* sourceUPlane = UPlane(&slot);
        const uint8_t* sourceVPlane = VPlane(&slot);
        BYTE* outputY = output;
        BYTE* outputUv = output + static_cast<size_t>(outputWidth) * outputHeight;

        if (rotation == 0 && outputWidth == sourceWidth && outputHeight == sourceHeight) {
            for (uint32_t y = 0; y < sourceHeight; ++y)
                std::memcpy(outputY + static_cast<size_t>(y) * outputWidth,
                            sourceYPlane + static_cast<size_t>(y) * slot.yStride, outputWidth);
            for (uint32_t y = 0; y < sourceHeight / 2; ++y) {
                BYTE* row = outputUv + static_cast<size_t>(y) * outputWidth;
                const uint8_t* u = sourceUPlane + static_cast<size_t>(y) * slot.uvStride;
                const uint8_t* v = sourceVPlane + static_cast<size_t>(y) * slot.uvStride;
                for (uint32_t x = 0; x < sourceWidth / 2; ++x) {
                    row[x * 2] = u[x];
                    row[x * 2 + 1] = v[x];
                }
            }
            return;
        }

        for (uint32_t y = 0; y < contentHeight; ++y) {
            const uint32_t ey = std::min(effectiveHeight - 1, static_cast<uint32_t>(y / scale));
            BYTE* row = outputY + static_cast<size_t>(top + y) * outputWidth + left;
            for (uint32_t x = 0; x < contentWidth; ++x) {
                const uint32_t ex = std::min(effectiveWidth - 1, static_cast<uint32_t>(x / scale));
                uint32_t sx{}, sy{};
                SourceCoordinate(rotation, ex, ey, sourceWidth, sourceHeight, sx, sy);
                row[x] = sourceYPlane[static_cast<size_t>(sy) * slot.yStride + sx];
            }
        }
        for (uint32_t y = 0; y < contentHeight / 2; ++y) {
            const uint32_t ey = std::min(effectiveHeight - 1, static_cast<uint32_t>((y * 2u) / scale));
            BYTE* row = outputUv + static_cast<size_t>(top / 2 + y) * outputWidth + left;
            for (uint32_t x = 0; x < contentWidth / 2; ++x) {
                const uint32_t ex = std::min(effectiveWidth - 1, static_cast<uint32_t>((x * 2u) / scale));
                uint32_t sx{}, sy{};
                SourceCoordinate(rotation, ex, ey, sourceWidth, sourceHeight, sx, sy);
                const size_t chromaIndex = static_cast<size_t>(sy / 2) * slot.uvStride + sx / 2;
                row[x * 2] = sourceUPlane[chromaIndex];
                row[x * 2 + 1] = sourceVPlane[chromaIndex];
            }
        }
    }

    bool EnsureOpen() {
        if (mapping_ && header_) {
            if (!ValidHeader(header_)) { Close(); return false; }
            if (!attached_) {
                InterlockedIncrement64(&header_->consumerAttaches);
                InterlockedIncrement(&header_->activeConsumers);
                attached_ = true;
            }
            return true;
        }
        if (++openAttempt_ % 15 != 1) return false;
        file_ = CreateFileW(FrameFilePath().c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        mappingHandle_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!mappingHandle_) { Close(); return false; }
        mapping_ = MapViewOfFile(mappingHandle_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes);
        if (!mapping_) { Close(); return false; }
        header_ = static_cast<SharedHeader*>(mapping_);
        if (!ValidHeader(header_)) { Close(); return false; }
        InterlockedIncrement64(&header_->reconnections);
        if (!attached_) {
            InterlockedIncrement64(&header_->consumerAttaches);
            InterlockedIncrement(&header_->activeConsumers);
            attached_ = true;
        }
        return true;
    }

    void Close() {
        Detach();
        if (mapping_) UnmapViewOfFile(mapping_);
        if (mappingHandle_) CloseHandle(mappingHandle_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        mapping_ = nullptr;
        header_ = nullptr;
        mappingHandle_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
        lastSequence_ = 0;
        openAttempt_ = 0;
    }

    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mappingHandle_{};
    void* mapping_{};
    SharedHeader* header_{};
    uint64_t lastSequence_{};
    uint64_t lastGeneration_{};
    uint32_t openAttempt_{};
    bool attached_{};
};

class MediaSource;

class MediaStream final : public IMFMediaStream2 {
public:
    MediaStream() { ++g_objectCount; }
    ~MediaStream() { Shutdown(); --g_objectCount; }

    HRESULT Initialize(MediaSource* parent);
    HRESULT Start(IMFMediaType* type, bool sendEvent);
    HRESULT Stop(bool sendEvent);
    HRESULT Shutdown();
    IMFStreamDescriptor* Descriptor() const { return descriptor_.Get(); }
    IMFAttributes* Attributes() const { return attributes_.Get(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) override;
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                         const PROPVARIANT* value) override;
    HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** source) override;
    HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    HRESULT STDMETHODCALLTYPE RequestSample(IUnknown* token) override;
    HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE state) override;
    HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* state) override;

private:
    HRESULT Check() const { return shutdown_ ? MF_E_SHUTDOWN : (events_ ? S_OK : E_UNEXPECTED); }
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    IMFMediaSource* parent_{};
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFMediaType> currentType_;
    SharedFrameReader reader_;
    MF_STREAM_STATE state_{MF_STREAM_STATE_STOPPED};
    bool shutdown_{};
    bool lastWasBlack_{true};
    uint32_t width_{1920};
    uint32_t height_{1080};
    int64_t qpcFrequency_{};
    int64_t nextSampleQpc_{};
};

class MediaSource final : public IMFMediaSourceEx, public IMFSampleAllocatorControl,
                          public IMFGetService, public IKsControl {
public:
    MediaSource() { ++g_objectCount; }
    ~MediaSource() { Shutdown(); --g_objectCount; }
    HRESULT Initialize(IMFAttributes* activateAttributes);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** event) override;
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType type, REFGUID extended, HRESULT status,
                                         const PROPVARIANT* value) override;
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override;
    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                    const PROPVARIANT* startPosition) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    HRESULT STDMETHODCALLTYPE Shutdown() override;
    HRESULT STDMETHODCALLTYPE GetSourceAttributes(IMFAttributes** attributes) override;
    HRESULT STDMETHODCALLTYPE GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    HRESULT STDMETHODCALLTYPE SetD3DManager(IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                                MFSampleAllocatorUsage* usage) override;
    HRESULT STDMETHODCALLTYPE GetService(REFGUID, REFIID, LPVOID* object) override;
    HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned) override;
    HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned) override;
    HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned) override;

private:
    HRESULT Check() const { return shutdown_ ? MF_E_SHUTDOWN : (events_ && stream_ ? S_OK : E_UNEXPECTED); }
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    MediaStream* stream_{};
    bool shutdown_{};
    bool started_{};
    bool streamAnnounced_{};
};

HRESULT MediaStream::Initialize(MediaSource* parent) {
    if (!parent) return E_INVALIDARG;
    parent_ = static_cast<IMFMediaSourceEx*>(parent);
    HRESULT hr = MFCreateEventQueue(events_.Put());
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(attributes_.Put(), 8);
    if (FAILED(hr)) return hr;
    attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY_Compat, PINNAME_VIDEO_CAPTURE);
    attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID_Compat, 0);
    attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED_Compat, 1);
    attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES_Compat, 0x0001);

    ComPtr<IMFMediaType> types[4];
    const uint32_t widths[] = {1920, 1080, 3840, 2160};
    const uint32_t heights[] = {1080, 1920, 2160, 3840};
    for (size_t i = 0; i < 4; ++i) {
        hr = MFCreateMediaType(types[i].Put());
        if (FAILED(hr)) return hr;
        types[i]->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        types[i]->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        types[i]->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        types[i]->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        types[i]->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        types[i]->SetUINT32(MF_MT_SAMPLE_SIZE, widths[i] * heights[i] * 3u / 2u);
        types[i]->SetUINT32(MF_MT_DEFAULT_STRIDE, widths[i]);
        types[i]->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        types[i]->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        types[i]->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        types[i]->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        types[i]->SetUINT32(MF_MT_VIDEO_CHROMA_SITING, MFVideoChromaSubsampling_MPEG2);
        MFSetAttributeSize(types[i].Get(), MF_MT_FRAME_SIZE, widths[i], heights[i]);
        MFSetAttributeRatio(types[i].Get(), MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(types[i].Get(), MF_MT_FRAME_RATE_RANGE_MIN, 30, 1);
        MFSetAttributeRatio(types[i].Get(), MF_MT_FRAME_RATE_RANGE_MAX, 30, 1);
        MFSetAttributeRatio(types[i].Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        types[i]->SetUINT32(MF_MT_AVG_BITRATE, widths[i] * heights[i] * 12u * 30u);
    }
    IMFMediaType* rawTypes[] = {types[0].Get(), types[1].Get(), types[2].Get(), types[3].Get()};
    hr = MFCreateStreamDescriptor(0, 4, rawTypes, descriptor_.Put());
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(handler.Put());
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(types[0].Get());
    if (FAILED(hr)) return hr;
    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY_Compat, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID_Compat, 0);
    descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED_Compat, 1);
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES_Compat, 0x0001);
    return S_OK;
}

HRESULT MediaStream::QueryInterface(REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IMFMediaEventGenerator || iid == IID_IMFMediaStream ||
        iid == IID_IMFMediaStream2_Compat)
        return CopyInterface(static_cast<IMFMediaStream2*>(this), result);
    return E_NOINTERFACE;
}

HRESULT MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    { std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); queue = events_; }
    return queue->GetEvent(flags, event);
}
HRESULT MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->BeginGetEvent(callback, state);
}
HRESULT MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->EndGetEvent(result, event);
}
HRESULT MediaStream::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->QueueEventParamVar(type, extended, status, value);
}
HRESULT MediaStream::GetMediaSource(IMFMediaSource** source) {
    std::lock_guard lock(mutex_); if (!source) return E_POINTER; *source = nullptr;
    if (FAILED(Check())) return Check(); parent_->AddRef(); *source = parent_; return S_OK;
}
HRESULT MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    std::lock_guard lock(mutex_); if (!descriptor) return E_POINTER; *descriptor = nullptr;
    if (FAILED(Check())) return Check(); descriptor_->AddRef(); *descriptor = descriptor_.Get(); return S_OK;
}

HRESULT MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    const int64_t now = QpcTicks();
    if (qpcFrequency_ > 0 && nextSampleQpc_ > now) {
        const DWORD waitMs = static_cast<DWORD>((nextSampleQpc_ - now) * 1000 / qpcFrequency_);
        if (waitMs) Sleep(waitMs);
    }
    const int64_t afterWait = QpcTicks();
    const int64_t frameTicks = qpcFrequency_ / 30;
    nextSampleQpc_ = std::max(nextSampleQpc_ + frameTicks, afterWait);
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    const DWORD bufferBytes = width_ * height_ * 3u / 2u;
    if (FAILED(hr = MFCreateSample(sample.Put()))) return hr;
    if (FAILED(hr = MFCreateMemoryBuffer(bufferBytes, buffer.Put()))) return hr;
    BYTE* bytes{};
    DWORD maximum{}, current{};
    if (FAILED(hr = buffer->Lock(&bytes, &maximum, &current))) return hr;
    bool discontinuity = lastWasBlack_;
    const bool hasFrame = reader_.CopyLatestToNv12(bytes, maximum, width_, height_, discontinuity);
    buffer->Unlock();
    buffer->SetCurrentLength(bufferBytes);
    if (!hasFrame) reader_.CountBlackFrame();
    if (FAILED(hr = sample->AddBuffer(buffer.Get()))) return hr;
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(333333);
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    if (discontinuity || hasFrame != !lastWasBlack_) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    lastWasBlack_ = !hasFrame;
    return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT MediaStream::Start(IMFMediaType* type, bool sendEvent) {
    std::lock_guard lock(mutex_);
    if (!type) return E_INVALIDARG;
    HRESULT hr = Check(); if (FAILED(hr)) return hr;
    currentType_.Reset(type); type->AddRef();
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
    state_ = MF_STREAM_STATE_RUNNING;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); qpcFrequency_ = frequency.QuadPart;
    nextSampleQpc_ = QpcTicks();
    reader_.Attach();
    lastWasBlack_ = true;
    return sendEvent ? events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr) : S_OK;
}
HRESULT MediaStream::Stop(bool sendEvent) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    state_ = MF_STREAM_STATE_STOPPED;
    reader_.Detach();
    return sendEvent && events_ ? events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr) : S_OK;
}
HRESULT MediaStream::Shutdown() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    if (events_) events_->Shutdown();
    events_.Reset(); attributes_.Reset(); descriptor_.Reset(); currentType_.Reset();
    parent_ = nullptr;
    return S_OK;
}
HRESULT MediaStream::SetStreamState(MF_STREAM_STATE state) {
    if (state == MF_STREAM_STATE_RUNNING) {
        ComPtr<IMFMediaType> type;
        { std::lock_guard lock(mutex_); type = currentType_; }
        if (!type) { ComPtr<IMFMediaTypeHandler> handler; descriptor_->GetMediaTypeHandler(handler.Put()); handler->GetCurrentMediaType(type.Put()); }
        return Start(type.Get(), false);
    }
    if (state == MF_STREAM_STATE_STOPPED) return Stop(false);
    if (state == MF_STREAM_STATE_PAUSED) { std::lock_guard lock(mutex_); state_ = state; return S_OK; }
    return MF_E_INVALID_STATE_TRANSITION;
}
HRESULT MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER; std::lock_guard lock(mutex_); *state = state_; return shutdown_ ? MF_E_SHUTDOWN : S_OK;
}

HRESULT MediaSource::Initialize(IMFAttributes* activateAttributes) {
    HRESULT hr = MFCreateEventQueue(events_.Put()); if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(attributes_.Put(), 8); if (FAILED(hr)) return hr;
    if (activateAttributes) activateAttributes->CopyAllItems(attributes_.Get());
    attributes_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, kAndroidCamVirtualCameraName);
    hr = AddLegacySensorProfile(attributes_.Get()); if (FAILED(hr)) return hr;
    stream_ = new (std::nothrow) MediaStream();
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->Initialize(this); if (FAILED(hr)) return hr;
    IMFStreamDescriptor* raw = stream_->Descriptor();
    hr = MFCreatePresentationDescriptor(1, &raw, descriptor_.Put());
    return hr;
}

HRESULT MediaSource::QueryInterface(REFIID iid, void** result) {
    if (!result) return E_POINTER; *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IMFMediaEventGenerator || iid == IID_IMFMediaSource || iid == IID_IMFMediaSourceEx)
        return CopyInterface(static_cast<IMFMediaSourceEx*>(this), result);
    if (iid == IID_IMFSampleAllocatorControl_Compat)
        return CopyInterface(static_cast<IMFSampleAllocatorControl*>(this), result);
    if (iid == IID_IMFGetService)
        return CopyInterface(static_cast<IMFGetService*>(this), result);
    if (iid == IID_IKsControl)
        return CopyInterface(static_cast<IKsControl*>(this), result);
    return E_NOINTERFACE;
}
HRESULT MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue; { std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); queue = events_; }
    return queue->GetEvent(flags, event);
}
HRESULT MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->BeginGetEvent(callback, state);
}
HRESULT MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->EndGetEvent(result, event);
}
HRESULT MediaSource::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check(); return events_->QueueEventParamVar(type, extended, status, value);
}
HRESULT MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER; std::lock_guard lock(mutex_); if (FAILED(Check())) return Check();
    *characteristics = MFMEDIASOURCE_IS_LIVE; return S_OK;
}
HRESULT MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER; *descriptor = nullptr; std::lock_guard lock(mutex_);
    if (FAILED(Check())) return Check(); return descriptor_->Clone(descriptor);
}
HRESULT MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) {
    if (!descriptor || !startPosition) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check();
    BOOL selected{}; ComPtr<IMFStreamDescriptor> streamDescriptor;
    HRESULT hr = descriptor->GetStreamDescriptorByIndex(0, &selected, streamDescriptor.Put());
    if (FAILED(hr) || !selected) return FAILED(hr) ? hr : MF_E_INVALIDREQUEST;
    ComPtr<IMFMediaTypeHandler> handler; ComPtr<IMFMediaType> type;
    if (FAILED(hr = streamDescriptor->GetMediaTypeHandler(handler.Put()))) return hr;
    if (FAILED(hr = handler->GetCurrentMediaType(type.Put()))) return hr;
    descriptor_->SelectStream(0);
    IUnknown* unknown = static_cast<IMFMediaStream2*>(stream_);
    unknown->AddRef();
    hr = events_->QueueEventParamUnk(streamAnnounced_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, unknown);
    unknown->Release();
    if (FAILED(hr)) return hr;
    streamAnnounced_ = true;
    if (FAILED(hr = stream_->Start(type.Get(), true))) return hr;
    PROPVARIANT value{}; PropVariantInit(&value); value.vt = VT_I8; value.hVal.QuadPart = MFGetSystemTime();
    hr = events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &value);
    PropVariantClear(&value);
    if (SUCCEEDED(hr)) started_ = true;
    return hr;
}
HRESULT MediaSource::Stop() {
    std::lock_guard lock(mutex_); if (FAILED(Check())) return Check();
    if (!started_) return S_OK;
    stream_->Stop(true); descriptor_->DeselectStream(0); started_ = false;
    return events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}
HRESULT MediaSource::Shutdown() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true; started_ = false;
    if (stream_) { stream_->Shutdown(); stream_->Release(); stream_ = nullptr; }
    if (events_) events_->Shutdown();
    events_.Reset(); attributes_.Reset(); descriptor_.Reset();
    return S_OK;
}
HRESULT MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER; *attributes = nullptr; std::lock_guard lock(mutex_);
    if (FAILED(Check())) return Check(); attributes_->AddRef(); *attributes = attributes_.Get(); return S_OK;
}
HRESULT MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER; *attributes = nullptr; std::lock_guard lock(mutex_);
    if (FAILED(Check())) return Check(); if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    stream_->Attributes()->AddRef(); *attributes = stream_->Attributes(); return S_OK;
}
HRESULT MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER; if (outputStreamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    *inputStreamId = 0; *usage = MFSampleAllocatorUsage_UsesCustomAllocator; return S_OK;
}
HRESULT MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::lock_guard lock(mutex_);
    return FAILED(Check()) ? Check() : MF_E_UNSUPPORTED_SERVICE;
}
HRESULT MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned) {
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
HRESULT MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned) {
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
HRESULT MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned) {
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

class Activation final : public IMFActivate {
public:
    Activation() { ++g_objectCount; MFCreateAttributes(attributes_.Put(), 8); }
    ~Activation() { --g_objectCount; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (iid == IID_IUnknown || iid == IID_IMFAttributes || iid == IID_IMFActivate)
            return CopyInterface(static_cast<IMFActivate*>(this), result);
        if (result) *result = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE ActivateObject(REFIID iid, void** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        auto* source = new (std::nothrow) MediaSource(); if (!source) return E_OUTOFMEMORY;
        HRESULT hr = source->Initialize(attributes_.Get());
        if (SUCCEEDED(hr)) hr = source->QueryInterface(iid, result);
        source->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE ShutdownObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DetachObject() override { return S_OK; }
#define FORWARD_ATTRIBUTE(name, signature, call) HRESULT STDMETHODCALLTYPE name signature override { return attributes_->name call; }
    FORWARD_ATTRIBUTE(GetItem, (REFGUID k, PROPVARIANT* v), (k,v))
    FORWARD_ATTRIBUTE(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* v), (k,v))
    FORWARD_ATTRIBUTE(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k,v,r))
    FORWARD_ATTRIBUTE(Compare, (IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r), (a,t,r))
    FORWARD_ATTRIBUTE(GetUINT32, (REFGUID k, UINT32* v), (k,v))
    FORWARD_ATTRIBUTE(GetUINT64, (REFGUID k, UINT64* v), (k,v))
    FORWARD_ATTRIBUTE(GetDouble, (REFGUID k, double* v), (k,v))
    FORWARD_ATTRIBUTE(GetGUID, (REFGUID k, GUID* v), (k,v))
    FORWARD_ATTRIBUTE(GetStringLength, (REFGUID k, UINT32* v), (k,v))
    FORWARD_ATTRIBUTE(GetString, (REFGUID k, LPWSTR v, UINT32 s, UINT32* l), (k,v,s,l))
    FORWARD_ATTRIBUTE(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k,v,l))
    FORWARD_ATTRIBUTE(GetBlobSize, (REFGUID k, UINT32* v), (k,v))
    FORWARD_ATTRIBUTE(GetBlob, (REFGUID k, UINT8* v, UINT32 s, UINT32* l), (k,v,s,l))
    FORWARD_ATTRIBUTE(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* l), (k,v,l))
    FORWARD_ATTRIBUTE(GetUnknown, (REFGUID k, REFIID i, LPVOID* v), (k,i,v))
    FORWARD_ATTRIBUTE(SetItem, (REFGUID k, REFPROPVARIANT v), (k,v))
    FORWARD_ATTRIBUTE(DeleteItem, (REFGUID k), (k))
    FORWARD_ATTRIBUTE(DeleteAllItems, (), ())
    FORWARD_ATTRIBUTE(SetUINT32, (REFGUID k, UINT32 v), (k,v))
    FORWARD_ATTRIBUTE(SetUINT64, (REFGUID k, UINT64 v), (k,v))
    FORWARD_ATTRIBUTE(SetDouble, (REFGUID k, double v), (k,v))
    FORWARD_ATTRIBUTE(SetGUID, (REFGUID k, REFGUID v), (k,v))
    FORWARD_ATTRIBUTE(SetString, (REFGUID k, LPCWSTR v), (k,v))
    FORWARD_ATTRIBUTE(SetBlob, (REFGUID k, const UINT8* v, UINT32 s), (k,v,s))
    FORWARD_ATTRIBUTE(SetUnknown, (REFGUID k, IUnknown* v), (k,v))
    FORWARD_ATTRIBUTE(LockStore, (), ())
    FORWARD_ATTRIBUTE(UnlockStore, (), ())
    FORWARD_ATTRIBUTE(GetCount, (UINT32* v), (v))
    FORWARD_ATTRIBUTE(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i,k,v))
    FORWARD_ATTRIBUTE(CopyAllItems, (IMFAttributes* v), (v))
#undef FORWARD_ATTRIBUTE
private:
    std::atomic<ULONG> references_{1};
    ComPtr<IMFAttributes> attributes_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { ++g_objectCount; }
    ~ClassFactory() { --g_objectCount; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (iid == IID_IUnknown || iid == IID_IClassFactory) return CopyInterface(static_cast<IClassFactory*>(this), result);
        if (result) *result = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* activation = new (std::nothrow) Activation(); if (!activation) return E_OUTOFMEMORY;
        HRESULT hr = activation->QueryInterface(iid, result); activation->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { lock ? ++g_serverLocks : --g_serverLocks; return S_OK; }
private:
    std::atomic<ULONG> references_{1};
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    return g_objectCount.load() == 0 && g_serverLocks.load() == 0 ? S_OK : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (clsid != CLSID_AndroidCamVirtualCamera) { if (result) *result = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }
    auto* factory = new (std::nothrow) ClassFactory(); if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, result); factory->Release(); return hr;
}
