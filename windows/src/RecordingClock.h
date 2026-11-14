#pragma once

#include <cstdint>

inline int64_t CommonPresentationUs(int64_t eventWindowsNs, int64_t commonOriginWindowsNs) {
    return (eventWindowsNs - commonOriginWindowsNs) / 1'000;
}

inline int64_t AudioSampleTimelineWindowsNs(int64_t anchorWindowsNs, uint64_t elapsedFrames,
                                            uint64_t sampleRate) {
    if (sampleRate == 0) return anchorWindowsNs;
    return anchorWindowsNs + static_cast<int64_t>(elapsedFrames) * 1'000'000'000LL /
        static_cast<int64_t>(sampleRate);
}
