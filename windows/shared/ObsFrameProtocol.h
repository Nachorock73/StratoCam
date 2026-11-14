#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AndroidCamObs {

inline constexpr wchar_t kMappingName[] = L"Local\\AndroidCamFrames-v1";
inline constexpr wchar_t kFrameEventName[] = L"Local\\AndroidCamFrameReady-v1";
inline constexpr char kMagic[8] = {'A', 'C', 'A', 'M', 'O', 'B', 'S', '1'};
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr uint32_t kSlotCount = 3;
inline constexpr uint32_t kMaxWidth = 3840;
inline constexpr uint32_t kMaxHeight = 2160;
inline constexpr size_t kMaxYBytes = static_cast<size_t>(kMaxWidth) * kMaxHeight;
inline constexpr size_t kMaxChromaBytes = kMaxYBytes / 4;

constexpr size_t Align64(size_t value) { return (value + 63u) & ~size_t{63u}; }

struct alignas(64) SharedHeader {
    char magic[8]{};
    uint32_t version{};
    uint32_t headerBytes{};
    uint32_t slotCount{};
    uint32_t slotStride{};
    uint32_t maxWidth{};
    uint32_t maxHeight{};
    uint32_t producerPid{};
    volatile LONG producerActive{};
    uint64_t qpcFrequency{};
    volatile LONG64 streamGeneration{};
    volatile LONG64 latestSequence{};
    volatile LONG latestSlot{-1};
    volatile LONG consumerPid{};
    volatile LONG64 publishedFrames{};
    volatile LONG64 producerDrops{};
    volatile LONG64 obsDeliveredFrames{};
    volatile LONG64 obsSkippedFrames{};
    volatile LONG64 lastObsSequence{};
    volatile LONG64 lastObsFrameNumber{-1};
    volatile LONG64 lastObsPublishQpc{};
    volatile LONG64 lastObsOutputQpc{};
    volatile LONG64 lastObsKnownCaptureToPublishUs{};
    uint8_t reserved[128]{};
};

struct alignas(64) SlotHeader {
    volatile LONG readers{};
    LONG reserved0{};
    volatile LONG64 writeGuard{};
    uint64_t sequence{};
    int64_t frameNumber{-1};
    uint32_t width{};
    uint32_t height{};
    uint32_t yStride{};
    uint32_t uvStride{};
    int32_t rotationDegrees{};
    uint32_t reserved1{};
    int64_t publishQpc{};
    int64_t knownCaptureToPublishUs{};
    uint8_t reserved[64]{};
};

inline constexpr size_t kHeaderBytes = Align64(sizeof(SharedHeader));
inline constexpr size_t kSlotHeaderBytes = Align64(sizeof(SlotHeader));
inline constexpr size_t kSlotDataBytes = kMaxYBytes + 2 * kMaxChromaBytes;
inline constexpr size_t kSlotStride = Align64(kSlotHeaderBytes + kSlotDataBytes);
inline constexpr size_t kMappingBytes = kHeaderBytes + kSlotCount * kSlotStride;

inline SlotHeader* Slot(void* mapping, uint32_t index) {
    return reinterpret_cast<SlotHeader*>(static_cast<uint8_t*>(mapping) + kHeaderBytes + index * kSlotStride);
}

inline const SlotHeader* Slot(const void* mapping, uint32_t index) {
    return reinterpret_cast<const SlotHeader*>(static_cast<const uint8_t*>(mapping) + kHeaderBytes + index * kSlotStride);
}

inline uint8_t* YPlane(SlotHeader* slot) {
    return reinterpret_cast<uint8_t*>(slot) + kSlotHeaderBytes;
}

inline uint8_t* UPlane(SlotHeader* slot) { return YPlane(slot) + kMaxYBytes; }
inline uint8_t* VPlane(SlotHeader* slot) { return UPlane(slot) + kMaxChromaBytes; }
inline const uint8_t* YPlane(const SlotHeader* slot) {
    return reinterpret_cast<const uint8_t*>(slot) + kSlotHeaderBytes;
}
inline const uint8_t* UPlane(const SlotHeader* slot) { return YPlane(slot) + kMaxYBytes; }
inline const uint8_t* VPlane(const SlotHeader* slot) { return UPlane(slot) + kMaxChromaBytes; }

inline bool ValidHeader(const SharedHeader* header) {
    return header && std::memcmp(header->magic, kMagic, sizeof(kMagic)) == 0 &&
           header->version == kProtocolVersion && header->headerBytes == kHeaderBytes &&
           header->slotCount == kSlotCount && header->slotStride == kSlotStride &&
           header->maxWidth == kMaxWidth && header->maxHeight == kMaxHeight;
}

} // namespace AndroidCamObs
