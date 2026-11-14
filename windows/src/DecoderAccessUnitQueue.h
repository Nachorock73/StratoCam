#pragma once

#include "VideoTransport.h"

#include <deque>

enum class DecoderQueuePushResult {
    Accepted,
    DroppedUntilKeyFrame,
    BacklogDiscarded,
    RecoveredOnKeyFrame
};

class DecoderAccessUnitQueue {
public:
    explicit DecoderAccessUnitQueue(size_t maximumDepth = 8) : maximumDepth_(maximumDepth) {}

    DecoderQueuePushResult Push(EncodedAccessUnit&& packet) {
        if (packet.flags & kVideoFlagCodecConfig) codecConfig_ = packet;
        if (waitingForKeyFrame_) {
            if (!(packet.flags & kVideoFlagKeyFrame)) {
                ++drops_;
                return DecoderQueuePushResult::DroppedUntilKeyFrame;
            }
            queue_.clear();
            if (codecConfig_) queue_.push_back(*codecConfig_);
            queue_.push_back(std::move(packet));
            waitingForKeyFrame_ = false;
            ++gopRecoveries_;
            UpdateHighWatermark();
            return DecoderQueuePushResult::RecoveredOnKeyFrame;
        }
        if (queue_.size() >= maximumDepth_) {
            drops_ += queue_.size() + 1;
            queue_.clear();
            waitingForKeyFrame_ = true;
            if (packet.flags & kVideoFlagKeyFrame) {
                if (codecConfig_) queue_.push_back(*codecConfig_);
                queue_.push_back(std::move(packet));
                waitingForKeyFrame_ = false;
                ++gopRecoveries_;
                UpdateHighWatermark();
                return DecoderQueuePushResult::RecoveredOnKeyFrame;
            }
            return DecoderQueuePushResult::BacklogDiscarded;
        }
        queue_.push_back(std::move(packet));
        UpdateHighWatermark();
        return DecoderQueuePushResult::Accepted;
    }

    EncodedAccessUnit Pop() {
        EncodedAccessUnit packet = std::move(queue_.front());
        queue_.pop_front();
        packet.timing.decoderQueueDepth = static_cast<uint32_t>(queue_.size());
        packet.timing.decoderQueueDrops = drops_;
        packet.timing.decoderGopRecoveries = gopRecoveries_;
        return packet;
    }

    void Reset() {
        queue_.clear();
        codecConfig_.reset();
        waitingForKeyFrame_ = false;
        highWatermark_ = drops_ = gopRecoveries_ = 0;
    }

    void Clear() { queue_.clear(); }
    void DiscardUntilKeyFrame() {
        drops_ += queue_.size();
        queue_.clear();
        waitingForKeyFrame_ = true;
    }
    bool Empty() const { return queue_.empty(); }
    size_t Size() const { return queue_.size(); }
    uint64_t HighWatermark() const { return highWatermark_; }
    uint64_t Drops() const { return drops_; }
    uint64_t GopRecoveries() const { return gopRecoveries_; }

private:
    void UpdateHighWatermark() { highWatermark_ = std::max<uint64_t>(highWatermark_, queue_.size()); }

    size_t maximumDepth_;
    std::deque<EncodedAccessUnit> queue_;
    std::optional<EncodedAccessUnit> codecConfig_;
    bool waitingForKeyFrame_{};
    uint64_t highWatermark_{};
    uint64_t drops_{};
    uint64_t gopRecoveries_{};
};
