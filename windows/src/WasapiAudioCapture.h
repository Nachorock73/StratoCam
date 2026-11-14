#pragma once

#include "AudioCaptureTypes.h"

#include <condition_variable>

class WasapiAudioCapture {
public:
    using ChunkCallback = std::function<void(AudioCaptureChunk&&)>;
    using StatusCallback = std::function<void(const std::string&)>;

    WasapiAudioCapture(ChunkCallback chunkCallback, StatusCallback statusCallback);
    ~WasapiAudioCapture();

    static std::vector<AudioCaptureDevice> EnumerateDevices(std::string& error);
    bool Start(const AudioCaptureDevice& device, std::string& error);
    void Stop();
    AudioCaptureSnapshot Snapshot() const;

private:
    void WorkerMain(AudioCaptureDevice device);
    void SetInitialization(bool success, const std::string& error, const AudioCaptureFormat& format = {});
    static double MeasurePeak(const uint8_t* data, uint32_t frames, const AudioCaptureFormat& format, bool silent);

    ChunkCallback chunkCallback_;
    StatusCallback statusCallback_;
    mutable std::mutex mutex_;
    std::condition_variable initializedChanged_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{};
    bool initialized_{};
    bool initializationSucceeded_{};
    AudioCaptureSnapshot snapshot_;
    void* stopEvent_{};
};
