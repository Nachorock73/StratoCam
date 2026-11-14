#pragma once

#include "Common.h"

enum class AudioSampleKind {
    Float32,
    Pcm16,
    Pcm32
};

struct AudioCaptureDevice {
    std::wstring id;
    std::wstring name;
};

struct AudioCaptureFormat {
    std::wstring deviceId;
    std::wstring deviceName;
    int sampleRate{};
    int channels{};
    int bitsPerSample{};
    int blockAlign{};
    uint64_t channelMask{};
    uint64_t devicePositionFrequency{};
    AudioSampleKind sampleKind{AudioSampleKind::Float32};
};

struct AudioCaptureChunk {
    std::shared_ptr<const std::vector<uint8_t>> payload;
    uint32_t frames{};
    uint64_t devicePosition{};
    int64_t captureQpcNs{};
    bool timestampError{};
    bool discontinuity{};
    uint64_t cumulativeOverruns{};
    uint64_t cumulativeUnderruns{};
    uint64_t cumulativeTimestampErrors{};
};

struct AudioCaptureSnapshot {
    bool running{};
    AudioCaptureFormat format;
    std::string error;
    double peak{};
    uint64_t packets{};
    uint64_t frames{};
    uint64_t overruns{};
    uint64_t underruns{};
    uint64_t timestampErrors{};
};
