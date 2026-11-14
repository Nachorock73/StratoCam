#pragma once

#include <cstdint>
#include <optional>

class RecordingTimeline {
public:
    explicit RecordingTimeline(int fps = 30) : frameDurationUs_(1'000'000 / fps) {}

    void Reset() {
        firstPresentationUs_.reset();
        lastPresentationUs_.reset();
        lastNormalizedUs_ = -1;
    }

    std::optional<int64_t> Normalize(int64_t presentationUs) {
        if (!firstPresentationUs_) firstPresentationUs_ = presentationUs;
        if (lastPresentationUs_ && presentationUs <= *lastPresentationUs_) return std::nullopt;
        lastPresentationUs_ = presentationUs;
        lastNormalizedUs_ = presentationUs - *firstPresentationUs_;
        return lastNormalizedUs_;
    }

    int64_t DurationUs() const {
        return lastNormalizedUs_ < 0 ? 0 : lastNormalizedUs_ + frameDurationUs_;
    }

    int64_t FrameDurationUs() const { return frameDurationUs_; }

private:
    int64_t frameDurationUs_{};
    std::optional<int64_t> firstPresentationUs_;
    std::optional<int64_t> lastPresentationUs_;
    int64_t lastNormalizedUs_{-1};
};
