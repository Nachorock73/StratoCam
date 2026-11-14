#include "DirectH264Recorder.h"
#include "PipelineDiagnostics.h"
#include "RecordingClock.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <fstream>
#include <iomanip>

namespace {
constexpr int kOutputAudioRate = 48'000;

std::string AvError(int result) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    return message;
}

AVFormatContext* Format(void* value) { return static_cast<AVFormatContext*>(value); }
AVStream* Stream(void* value) { return static_cast<AVStream*>(value); }
AVCodecContext* AudioEncoder(void* value) { return static_cast<AVCodecContext*>(value); }
SwrContext* Resampler(void* value) { return static_cast<SwrContext*>(value); }
AVAudioFifo* AudioFifo(void* value) { return static_cast<AVAudioFifo*>(value); }
}

DirectH264Recorder::DirectH264Recorder(StatusCallback statusCallback)
    : statusCallback_(std::move(statusCallback)) {}

DirectH264Recorder::~DirectH264Recorder() { Stop(); }

bool DirectH264Recorder::Start(const std::filesystem::path& path,
                               const std::filesystem::path& metricsPath,
                               const StreamInfo& info,
                               const std::optional<AudioCaptureFormat>& audioFormat,
                               int audioSyncOffsetMs, int rotationDegrees, std::string& error) {
    Stop();
    if (info.width <= 0 || info.height <= 0 || info.fps <= 0 || info.codec != "video/avc") {
        error = "A valid active H.264 stream is required before recording";
        return false;
    }
    if (audioFormat && (audioFormat->sampleRate <= 0 || audioFormat->channels <= 0 ||
                        audioFormat->blockAlign <= 0)) {
        error = "The selected WASAPI input does not have a valid capture format";
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    std::filesystem::create_directories(metricsPath.parent_path(), filesystemError);
    if (filesystemError) {
        error = "Could not create the recording directory: " + filesystemError.message();
        return false;
    }
    auto audioMetrics = metricsPath;
    audioMetrics.replace_filename(metricsPath.stem().wstring() + L"-audio.csv");
    {
        std::lock_guard lock(mutex_);
        path_ = path;
        metricsPath_ = metricsPath;
        audioMetricsPath_ = audioMetrics;
        streamInfo_ = info;
        audioFormat_ = audioFormat;
        audioSyncOffsetMs_ = std::clamp(audioSyncOffsetMs, -5'000, 5'000);
        rotationDegrees_ = NormalizeRecordingRotation(rotationDegrees);
        armedWindowsNs_ = static_cast<int64_t>(std::llround(QpcNanoseconds()));
        commonOriginWindowsNs_ = 0;
        videoQueue_.clear();
        audioQueue_.clear();
        error_.clear();
        audioCodecName_.clear();
        packetsWritten_ = bytesWritten_ = queueHighWatermark_ = queueDrops_ = 0;
        gopRecoveries_ = prerollSkipped_ = timestampErrors_ = 0;
        audioPacketsWritten_ = audioBytesWritten_ = audioQueueHighWatermark_ = audioQueueDrops_ = 0;
        audioOverruns_ = audioUnderruns_ = audioTimestampErrors_ = 0;
        audioOverrunBaseline_ = audioUnderrunBaseline_ = audioTimestampErrorBaseline_ = 0;
        durationUs_ = 0;
        videoFirstPtsUs_ = videoLastPtsUs_ = -1;
        audioFirstPtsUs_ = audioLastPtsUs_ = -1;
        audioAnchorQpcNs_ = 0;
        audioBasePtsSamples_ = audioSamplesEncoded_ = 0;
        audioCapturedFrames_ = 0;
        audioClockDriftFirstMs_ = audioClockDriftLastMs_ = 0.0;
        videoMetrics_.clear();
        audioMetrics_.clear();
        waitingForKeyFrame_ = true;
        recordingStarted_ = false;
        stopRequested_ = false;
        accepting_ = true;
        state_ = DirectRecorderState::WaitingForKeyFrame;
    }
    worker_ = std::thread(&DirectH264Recorder::WorkerMain, this);
    PipelineDiagnostics::Log("recorder", "armed path=" + WideToUtf8(path.wstring()) +
        "; audio=" + (audioFormat ? WideToUtf8(audioFormat->deviceName) : "none") +
        "; syncOffsetMs=" + std::to_string(audioSyncOffsetMs_) +
        "; displayRotationClockwise=" + std::to_string(rotationDegrees_) +
        "; waiting for SPS/PPS + IDR");
    statusCallback_(audioFormat ? "REC armed · audio + video · waiting for IDR"
                                : "REC armed · video only · waiting for IDR");
    return true;
}

void DirectH264Recorder::Stop() {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        stopRequested_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void DirectH264Recorder::ResetCodecConfig() {
    std::lock_guard lock(mutex_);
    if (!accepting_) codecConfig_.reset();
}

void DirectH264Recorder::Enqueue(const EncodedAccessUnit& packet) {
    std::lock_guard lock(mutex_);
    if (packet.flags & kVideoFlagCodecConfig) {
        codecConfig_ = packet;
        return;
    }
    if (!accepting_ || packet.Bytes().empty()) return;
    if (waitingForKeyFrame_) {
        if (!(packet.flags & kVideoFlagKeyFrame) || !codecConfig_) {
            ++prerollSkipped_;
            return;
        }
        waitingForKeyFrame_ = false;
        if (recordingStarted_) ++gopRecoveries_;
    }
    if (videoQueue_.size() >= kMaximumVideoQueueDepth) {
        queueDrops_ += videoQueue_.size() + 1;
        videoQueue_.clear();
        waitingForKeyFrame_ = true;
        PipelineDiagnostics::Log("recorder", "DROP reason=video-recorder-backlog; waiting for IDR");
        return;
    }
    const auto depth = static_cast<uint32_t>(videoQueue_.size());
    videoQueue_.push_back({packet, QpcNanoseconds(), depth});
    queueHighWatermark_ = std::max<uint64_t>(queueHighWatermark_, videoQueue_.size());
    changed_.notify_one();
}

void DirectH264Recorder::EnqueueAudio(AudioCaptureChunk&& chunk) {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !audioFormat_ || !chunk.payload || chunk.payload->empty() || chunk.frames == 0) return;
    const int64_t packetEndNs = chunk.captureQpcNs +
        static_cast<int64_t>(chunk.frames) * 1'000'000'000LL / audioFormat_->sampleRate;
    if (packetEndNs <= armedWindowsNs_) return;
    if (audioQueue_.size() >= kMaximumAudioQueueDepth) {
        audioQueue_.pop_front();
        ++audioQueueDrops_;
        PipelineDiagnostics::Log("recorder", "DROP reason=audio-recorder-backlog");
    }
    const auto depth = static_cast<uint32_t>(audioQueue_.size());
    audioQueue_.push_back({std::move(chunk), QpcNanoseconds(), depth});
    audioQueueHighWatermark_ = std::max<uint64_t>(audioQueueHighWatermark_, audioQueue_.size());
    changed_.notify_one();
}

DirectRecorderSnapshot DirectH264Recorder::Snapshot() const {
    std::lock_guard lock(mutex_);
    return {state_, path_, error_, packetsWritten_, bytesWritten_, static_cast<uint64_t>(videoQueue_.size()),
            queueHighWatermark_, queueDrops_, gopRecoveries_, prerollSkipped_, timestampErrors_, durationUs_,
            audioFormat_.has_value(), audioFormat_ ? audioFormat_->deviceName : L"", audioCodecName_,
            audioPacketsWritten_, audioBytesWritten_, static_cast<uint64_t>(audioQueue_.size()),
            audioQueueHighWatermark_, audioQueueDrops_, audioOverruns_, audioUnderruns_, audioTimestampErrors_,
            videoFirstPtsUs_, videoLastPtsUs_, audioFirstPtsUs_, audioLastPtsUs_,
            audioClockDriftFirstMs_, audioClockDriftLastMs_};
}

void DirectH264Recorder::WorkerMain() {
    bool failed = false;
    for (;;) {
        bool processAudio = false;
        QueuedAccessUnit video;
        QueuedAudioChunk audio;
        std::optional<EncodedAccessUnit> config;
        bool openNow = false;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] {
                if (stopRequested_) return true;
                if (!formatContext_)
                    return !videoQueue_.empty() && (!audioFormat_ || !audioQueue_.empty());
                return !videoQueue_.empty() || !audioQueue_.empty();
            });
            if (!formatContext_) {
                if (videoQueue_.empty() || (audioFormat_ && audioQueue_.empty())) {
                    if (stopRequested_) break;
                    continue;
                }
                video = videoQueue_.front();
                config = codecConfig_;
                openNow = true;
            } else if (videoQueue_.empty() && audioQueue_.empty()) {
                if (stopRequested_) break;
                continue;
            } else {
                const bool hasVideo = !videoQueue_.empty();
                const bool hasAudio = !audioQueue_.empty();
                processAudio = hasAudio && (!hasVideo ||
                    AudioCaptureWindowsNs(audioQueue_.front().chunk) +
                        static_cast<int64_t>(audioSyncOffsetMs_) * 1'000'000LL <=
                    VideoCaptureWindowsNs(videoQueue_.front().packet));
                if (processAudio) {
                    audio = std::move(audioQueue_.front());
                    audioQueue_.pop_front();
                } else {
                    video = std::move(videoQueue_.front());
                    videoQueue_.pop_front();
                }
            }
        }
        std::string error;
        if (openNow) {
            std::optional<QueuedAudioChunk> firstAudio;
            {
                std::lock_guard lock(mutex_);
                if (audioFormat_ && !audioQueue_.empty()) firstAudio = audioQueue_.front();
            }
            if (!config || !OpenMuxer(*config, video, firstAudio, error)) failed = true;
            else {
                std::lock_guard lock(mutex_);
                state_ = DirectRecorderState::Recording;
                recordingStarted_ = true;
            }
        } else if (processAudio) {
            if (!ProcessAudioChunk(audio, error)) failed = true;
        } else if (!WriteVideoPacket(video, error)) {
            failed = true;
        }
        if (failed) {
            PipelineDiagnostics::Log("recorder", "failure=" + error);
            statusCallback_("Recording failed · " + error);
            std::lock_guard lock(mutex_);
            error_ = error;
            state_ = DirectRecorderState::Failed;
            accepting_ = false;
            stopRequested_ = true;
            queueDrops_ += videoQueue_.size();
            audioQueueDrops_ += audioQueue_.size();
            videoQueue_.clear();
            audioQueue_.clear();
            break;
        }
    }
    CloseMuxer(!failed);
    ExportMetrics();
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        if (!failed) state_ = DirectRecorderState::Idle;
    }
}

bool DirectH264Recorder::OpenMuxer(const EncodedAccessUnit& codecConfig,
                                   const QueuedAccessUnit& firstVideo,
                                   const std::optional<QueuedAudioChunk>& firstAudio,
                                   std::string& error) {
    const auto& extra = codecConfig.Bytes();
    if (!ContainsNalType(extra, 7) || !ContainsNalType(extra, 8)) {
        error = "H.264 codec configuration does not contain both SPS and PPS";
        return false;
    }
    const int64_t firstVideoNs = VideoCaptureWindowsNs(firstVideo.packet);
    if (audioFormat_ && firstVideo.packet.timing.synchronizedCaptureWindowsNs <= 0) {
        error = "Camera2 capture timestamp was not synchronized to the Windows monotonic clock";
        return false;
    }
    const int64_t firstAudioNs = firstAudio
        ? AudioCaptureWindowsNs(firstAudio->chunk) + static_cast<int64_t>(audioSyncOffsetMs_) * 1'000'000LL
        : firstVideoNs;
    commonOriginWindowsNs_ = std::min(firstVideoNs, firstAudioNs);

    AVFormatContext* format = nullptr;
    const std::string utf8Path = WideToUtf8(path_.wstring());
    int result = avformat_alloc_output_context2(&format, nullptr, "mp4", utf8Path.c_str());
    if (result < 0 || !format) {
        error = "Could not create fragmented MP4 muxer: " + AvError(result);
        return false;
    }
    AVStream* videoStream = avformat_new_stream(format, nullptr);
    if (!videoStream) {
        avformat_free_context(format);
        error = "Could not create H.264 MP4 track";
        return false;
    }
    videoStream->time_base = {1, 1'000'000};
    videoStream->avg_frame_rate = {streamInfo_.fps, 1};
    videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codecpar->codec_id = AV_CODEC_ID_H264;
    videoStream->codecpar->width = streamInfo_.width;
    videoStream->codecpar->height = streamInfo_.height;
    videoStream->codecpar->codec_tag = 0;
    videoStream->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!videoStream->codecpar->extradata) {
        avformat_free_context(format);
        error = "Could not allocate H.264 codec configuration";
        return false;
    }
    std::memcpy(videoStream->codecpar->extradata, extra.data(), extra.size());
    videoStream->codecpar->extradata_size = static_cast<int>(extra.size());
    if (rotationDegrees_ != 0) {
        AVPacketSideData* displayMatrix = av_packet_side_data_new(
            &videoStream->codecpar->coded_side_data,
            &videoStream->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(int32_t), 0);
        if (!displayMatrix) {
            avformat_free_context(format);
            error = "Could not allocate the MP4 video display matrix";
            return false;
        }
        av_display_rotation_set(reinterpret_cast<int32_t*>(displayMatrix->data),
                                static_cast<double>(rotationDegrees_));
    }
    formatContext_ = format;
    videoStream_ = videoStream;
    if (audioFormat_ && !ConfigureAudioEncoder(error)) {
        CloseMuxer(false);
        return false;
    }
    format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    result = avio_open(&format->pb, utf8Path.c_str(), AVIO_FLAG_WRITE);
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+delay_moov+default_base_moof", 0);
    if (result >= 0) result = avformat_write_header(format, &options);
    av_dict_free(&options);
    if (result < 0) {
        error = "Could not open fragmented MP4 recording: " + AvError(result);
        CloseMuxer(false);
        return false;
    }
    PipelineDiagnostics::Log("recorder", "fragmented MP4 stream-copy opened " + utf8Path +
        " videoTimeBase=" + std::to_string(videoStream->time_base.num) + "/" +
        std::to_string(videoStream->time_base.den) + "; audio=" +
        (audioFormat_ ? audioCodecName_ + "/48000Hz" : "none") +
        "; displayRotationClockwise=" + std::to_string(rotationDegrees_) +
        "; commonOriginQpcNs=" + std::to_string(commonOriginWindowsNs_));
    statusCallback_(audioFormat_ ? "REC · direct H.264 + WASAPI " + audioCodecName_ + " to fragmented MP4"
                                 : "REC · direct H.264 to fragmented MP4");
    return true;
}

bool DirectH264Recorder::ConfigureAudioEncoder(std::string& error) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        error = "AAC audio encoding is unavailable";
        return false;
    }
    AVCodecContext* encoder = avcodec_alloc_context3(codec);
    if (!encoder) {
        error = "Could not allocate the audio encoder";
        return false;
    }
    encoder->sample_rate = kOutputAudioRate;
    encoder->time_base = {1, kOutputAudioRate};
    encoder->profile = AV_PROFILE_AAC_LOW;
    encoder->bit_rate = audioFormat_->channels == 1 ? 96'000 : 192'000;
    const void* supportedSampleFormats = nullptr;
    int supportedSampleFormatCount = 0;
    int result = 0;
    result = avcodec_get_supported_config(encoder, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                          &supportedSampleFormats, &supportedSampleFormatCount);
    if (result < 0 || supportedSampleFormatCount == 0) {
        error = "Could not query the audio encoder sample formats: " + AvError(result);
        avcodec_free_context(&encoder);
        return false;
    }
    encoder->sample_fmt = static_cast<const AVSampleFormat*>(supportedSampleFormats)[0];
    av_channel_layout_default(&encoder->ch_layout, audioFormat_->channels);
    if (Format(formatContext_)->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    result = avcodec_open2(encoder, codec, nullptr);
    if (result < 0) {
        error = "Could not open " + std::string(codec->name) + " audio encoder: " + AvError(result);
        avcodec_free_context(&encoder);
        return false;
    }
    AVStream* audioStream = avformat_new_stream(Format(formatContext_), nullptr);
    if (!audioStream) {
        error = "Could not create the MP4 audio track";
        avcodec_free_context(&encoder);
        return false;
    }
    audioStream->time_base = encoder->time_base;
    result = avcodec_parameters_from_context(audioStream->codecpar, encoder);
    if (result < 0) {
        error = "Could not copy the audio encoder configuration: " + AvError(result);
        avcodec_free_context(&encoder);
        return false;
    }
    AVChannelLayout inputLayout{};
    if (audioFormat_->channelMask)
        result = av_channel_layout_from_mask(&inputLayout, audioFormat_->channelMask);
    else {
        av_channel_layout_default(&inputLayout, audioFormat_->channels);
        result = 0;
    }
    AVSampleFormat inputSampleFormat = AV_SAMPLE_FMT_FLT;
    if (audioFormat_->sampleKind == AudioSampleKind::Pcm16) inputSampleFormat = AV_SAMPLE_FMT_S16;
    else if (audioFormat_->sampleKind == AudioSampleKind::Pcm32) inputSampleFormat = AV_SAMPLE_FMT_S32;
    SwrContext* resampler = nullptr;
    if (result >= 0) result = swr_alloc_set_opts2(&resampler, &encoder->ch_layout, encoder->sample_fmt,
                                                   encoder->sample_rate, &inputLayout, inputSampleFormat,
                                                   audioFormat_->sampleRate, 0, nullptr);
    av_channel_layout_uninit(&inputLayout);
    if (result >= 0) result = swr_init(resampler);
    if (result < 0 || !resampler) {
        error = "Could not configure 48 kHz audio conversion: " + AvError(result);
        swr_free(&resampler);
        avcodec_free_context(&encoder);
        return false;
    }
    AVAudioFifo* fifo = av_audio_fifo_alloc(encoder->sample_fmt, encoder->ch_layout.nb_channels,
                                            std::max(encoder->frame_size, 1024));
    if (!fifo) {
        error = "Could not allocate the audio sample queue";
        swr_free(&resampler);
        avcodec_free_context(&encoder);
        return false;
    }
    audioEncoderContext_ = encoder;
    audioStream_ = audioStream;
    audioResampler_ = resampler;
    audioFifo_ = fifo;
    audioCodecName_ = codec->name;
    return true;
}

bool DirectH264Recorder::WriteVideoPacket(const QueuedAccessUnit& queued, std::string& error) {
    const int64_t captureWindowsNs = VideoCaptureWindowsNs(queued.packet);
    const int64_t commonPtsUs = CommonPresentationUs(captureWindowsNs, commonOriginWindowsNs_);
    if (videoLastPtsUs_ >= 0 && commonPtsUs <= videoLastPtsUs_) {
        std::lock_guard lock(mutex_);
        ++timestampErrors_;
        ++queueDrops_;
        return true;
    }
    const auto& bytes = queued.packet.Bytes();
    AVPacket* packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, static_cast<int>(bytes.size())) < 0) {
        av_packet_free(&packet);
        error = "Could not allocate MP4 video packet";
        return false;
    }
    std::memcpy(packet->data, bytes.data(), bytes.size());
    packet->stream_index = Stream(videoStream_)->index;
    packet->pts = commonPtsUs;
    packet->dts = commonPtsUs;
    packet->duration = 1'000'000 / streamInfo_.fps;
    if (queued.packet.flags & kVideoFlagKeyFrame) packet->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(packet, AVRational{1, 1'000'000}, Stream(videoStream_)->time_base);
    const double dequeuedNs = QpcNanoseconds();
    const int result = av_interleaved_write_frame(Format(formatContext_), packet);
    av_packet_free(&packet);
    const double completedNs = QpcNanoseconds();
    if (result < 0) {
        error = "MP4 video write failed: " + AvError(result);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        ++packetsWritten_;
        bytesWritten_ += bytes.size();
        if (videoFirstPtsUs_ < 0) videoFirstPtsUs_ = commonPtsUs;
        videoLastPtsUs_ = commonPtsUs;
        durationUs_ = std::max(durationUs_, commonPtsUs + 1'000'000 / streamInfo_.fps);
        videoMetrics_.push_back({queued.packet.timing.accessUnitSequence,
                                 queued.packet.timing.presentationTimeUs, captureWindowsNs, commonPtsUs,
                                 queued.enqueuedNs, dequeuedNs, completedNs, queued.queueDepth,
                                 bytes.size(), queueDrops_});
    }
    return true;
}

bool DirectH264Recorder::ProcessAudioChunk(const QueuedAudioChunk& queued, std::string& error) {
    AVCodecContext* encoder = AudioEncoder(audioEncoderContext_);
    const auto& bytes = *queued.chunk.payload;
    const int64_t reportedCaptureNs = queued.chunk.captureQpcNs;
    if (audioAnchorQpcNs_ == 0) {
        audioAnchorQpcNs_ = reportedCaptureNs;
        audioOverrunBaseline_ = queued.chunk.cumulativeOverruns;
        audioUnderrunBaseline_ = queued.chunk.cumulativeUnderruns;
        audioTimestampErrorBaseline_ = queued.chunk.cumulativeTimestampErrors;
    }
    const int64_t expectedNs = AudioSampleTimelineWindowsNs(audioAnchorQpcNs_, audioCapturedFrames_,
                                                             audioFormat_->sampleRate);
    const int64_t captureNs = queued.chunk.timestampError ? expectedNs : reportedCaptureNs;
    const int64_t adjustedCaptureNs = captureNs + static_cast<int64_t>(audioSyncOffsetMs_) * 1'000'000LL;
    if (audioMetrics_.empty())
        audioBasePtsSamples_ = av_rescale_q(adjustedCaptureNs - commonOriginWindowsNs_,
                                            AVRational{1, 1'000'000'000}, encoder->time_base);
    const double clockDriftMs = (reportedCaptureNs - expectedNs) / 1'000'000.0;
    if (audioMetrics_.empty()) audioClockDriftFirstMs_ = clockDriftMs;
    audioClockDriftLastMs_ = clockDriftMs;
    audioOverruns_ = queued.chunk.cumulativeOverruns - audioOverrunBaseline_;
    audioUnderruns_ = queued.chunk.cumulativeUnderruns - audioUnderrunBaseline_;
    audioTimestampErrors_ = queued.chunk.cumulativeTimestampErrors - audioTimestampErrorBaseline_;

    const uint8_t* input[] = {bytes.data()};
    const int maximumOutput = swr_get_out_samples(Resampler(audioResampler_), queued.chunk.frames);
    uint8_t** converted = nullptr;
    int result = av_samples_alloc_array_and_samples(&converted, nullptr, encoder->ch_layout.nb_channels,
                                                     maximumOutput, encoder->sample_fmt, 0);
    if (result < 0) {
        error = "Could not allocate converted audio samples: " + AvError(result);
        return false;
    }
    const int convertedFrames = swr_convert(Resampler(audioResampler_), converted, maximumOutput,
                                            input, queued.chunk.frames);
    if (convertedFrames < 0) {
        error = "Audio resampling failed: " + AvError(convertedFrames);
        av_freep(&converted[0]);
        av_freep(&converted);
        return false;
    }
    result = av_audio_fifo_realloc(AudioFifo(audioFifo_),
                                   av_audio_fifo_size(AudioFifo(audioFifo_)) + convertedFrames);
    if (result >= 0)
        result = av_audio_fifo_write(AudioFifo(audioFifo_), reinterpret_cast<void**>(converted), convertedFrames);
    av_freep(&converted[0]);
    av_freep(&converted);
    if (result < convertedFrames) {
        error = "Could not queue converted audio samples";
        return false;
    }
    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 960;
    while (av_audio_fifo_size(AudioFifo(audioFifo_)) >= frameSize) {
        if (!EncodeAudioFrame(frameSize, error)) return false;
    }
    audioCapturedFrames_ += queued.chunk.frames;
    {
        std::lock_guard lock(mutex_);
        audioMetrics_.push_back({queued.chunk.devicePosition, captureNs,
            CommonPresentationUs(adjustedCaptureNs, commonOriginWindowsNs_), clockDriftMs, queued.chunk.frames,
            queued.queueDepth, queued.enqueuedNs, QpcNanoseconds(), audioQueueDrops_,
            audioOverruns_, audioUnderruns_, audioTimestampErrors_,
            queued.chunk.discontinuity, queued.chunk.timestampError});
    }
    return true;
}

bool DirectH264Recorder::EncodeAudioFrame(int samples, std::string& error) {
    AVCodecContext* encoder = AudioEncoder(audioEncoderContext_);
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        error = "Could not allocate an audio frame";
        return false;
    }
    frame->nb_samples = samples;
    frame->format = encoder->sample_fmt;
    frame->sample_rate = encoder->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    int result = av_frame_get_buffer(frame, 0);
    if (result >= 0)
        result = av_audio_fifo_read(AudioFifo(audioFifo_), reinterpret_cast<void**>(frame->data), samples);
    if (result < samples) {
        error = "Could not read the queued audio frame";
        av_frame_free(&frame);
        return false;
    }
    frame->pts = audioBasePtsSamples_ + audioSamplesEncoded_;
    audioSamplesEncoded_ += samples;
    result = avcodec_send_frame(encoder, frame);
    av_frame_free(&frame);
    if (result < 0) {
        error = "Could not send samples to the audio encoder: " + AvError(result);
        return false;
    }
    return DrainAudioEncoder(error);
}

bool DirectH264Recorder::DrainAudioEncoder(std::string& error) {
    AVCodecContext* encoder = AudioEncoder(audioEncoderContext_);
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        error = "Could not allocate an encoded audio packet";
        return false;
    }
    for (;;) {
        const int result = avcodec_receive_packet(encoder, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        if (result < 0) {
            error = "Could not receive encoded audio: " + AvError(result);
            av_packet_free(&packet);
            return false;
        }
        const int payloadBytes = packet->size;
        const int64_t ptsUs = packet->pts == AV_NOPTS_VALUE ? -1 :
            av_rescale_q(packet->pts, encoder->time_base, AVRational{1, 1'000'000});
        const int64_t durationUs = av_rescale_q(packet->duration, encoder->time_base,
                                                AVRational{1, 1'000'000});
        packet->stream_index = Stream(audioStream_)->index;
        av_packet_rescale_ts(packet, encoder->time_base, Stream(audioStream_)->time_base);
        const int writeResult = av_interleaved_write_frame(Format(formatContext_), packet);
        av_packet_unref(packet);
        if (writeResult < 0) {
            error = "MP4 audio write failed: " + AvError(writeResult);
            av_packet_free(&packet);
            return false;
        }
        std::lock_guard lock(mutex_);
        ++audioPacketsWritten_;
        audioBytesWritten_ += payloadBytes;
        if (ptsUs >= 0 && audioFirstPtsUs_ < 0) audioFirstPtsUs_ = ptsUs;
        if (ptsUs >= 0) {
            audioLastPtsUs_ = ptsUs + durationUs;
            durationUs_ = std::max(durationUs_, audioLastPtsUs_);
        }
    }
    av_packet_free(&packet);
    return true;
}

bool DirectH264Recorder::FlushAudio(std::string& error) {
    AVCodecContext* encoder = AudioEncoder(audioEncoderContext_);
    if (!encoder) return true;
    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 960;
    int remaining = av_audio_fifo_size(AudioFifo(audioFifo_));
    if (remaining > 0) {
        int sendSamples = remaining;
        if (!(encoder->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) && remaining < frameSize) {
            uint8_t** silence = nullptr;
            const int padding = frameSize - remaining;
            int result = av_samples_alloc_array_and_samples(&silence, nullptr, encoder->ch_layout.nb_channels,
                                                             padding, encoder->sample_fmt, 0);
            if (result < 0) {
                error = "Could not allocate final audio padding: " + AvError(result);
                return false;
            }
            av_samples_set_silence(silence, 0, padding, encoder->ch_layout.nb_channels, encoder->sample_fmt);
            result = av_audio_fifo_realloc(AudioFifo(audioFifo_), remaining + padding);
            if (result >= 0)
                result = av_audio_fifo_write(AudioFifo(audioFifo_), reinterpret_cast<void**>(silence), padding);
            av_freep(&silence[0]);
            av_freep(&silence);
            if (result < padding) {
                error = "Could not queue final audio padding";
                return false;
            }
            sendSamples = frameSize;
        }
        if (!EncodeAudioFrame(sendSamples, error)) return false;
    }
    int result = avcodec_send_frame(encoder, nullptr);
    if (result < 0 && result != AVERROR_EOF) {
        error = "Could not flush the audio encoder: " + AvError(result);
        return false;
    }
    return DrainAudioEncoder(error);
}

void DirectH264Recorder::CloseMuxer(bool writeTrailer) {
    AVFormatContext* format = Format(formatContext_);
    if (!format) return;
    if (writeTrailer && audioEncoderContext_) {
        std::string audioError;
        if (!FlushAudio(audioError)) {
            PipelineDiagnostics::Log("recorder", "audio flush failure=" + audioError);
            writeTrailer = false;
        }
    }
    if (writeTrailer) av_write_trailer(format);
    if (format->pb) {
        avio_flush(format->pb);
        avio_closep(&format->pb);
    }
    AVAudioFifo* fifo = AudioFifo(audioFifo_);
    if (fifo) av_audio_fifo_free(fifo);
    audioFifo_ = nullptr;
    SwrContext* resampler = Resampler(audioResampler_);
    swr_free(&resampler);
    audioResampler_ = nullptr;
    AVCodecContext* encoder = AudioEncoder(audioEncoderContext_);
    avcodec_free_context(&encoder);
    audioEncoderContext_ = nullptr;
    avformat_free_context(format);
    formatContext_ = nullptr;
    videoStream_ = nullptr;
    audioStream_ = nullptr;
    const auto snapshot = Snapshot();
    PipelineDiagnostics::Log("recorder", "closed videoPackets=" + std::to_string(snapshot.packetsWritten) +
        " videoBytes=" + std::to_string(snapshot.bytesWritten) +
        " videoQueueMax=" + std::to_string(snapshot.queueHighWatermark) +
        " videoDrops=" + std::to_string(snapshot.queueDrops) +
        " audioPackets=" + std::to_string(snapshot.audioPacketsWritten) +
        " audioBytes=" + std::to_string(snapshot.audioBytesWritten) +
        " audioQueueMax=" + std::to_string(snapshot.audioQueueHighWatermark) +
        " audioDrops=" + std::to_string(snapshot.audioQueueDrops) +
        " audioOverruns=" + std::to_string(snapshot.audioOverruns) +
        " audioUnderruns=" + std::to_string(snapshot.audioUnderruns) +
        " audioTimestampErrors=" + std::to_string(snapshot.audioTimestampErrors));
}

void DirectH264Recorder::ExportMetrics() {
    std::vector<VideoMetric> videoMetrics;
    std::vector<AudioMetric> audioMetrics;
    {
        std::lock_guard lock(mutex_);
        videoMetrics = videoMetrics_;
        audioMetrics = audioMetrics_;
    }
    if (!videoMetrics.empty() && !metricsPath_.empty()) {
        std::ofstream output(metricsPath_, std::ios::trunc);
        if (output) {
            output << "access_unit_sequence,source_presentation_us,capture_windows_ns,common_presentation_us,"
                      "enqueued_ns,dequeued_ns,write_complete_ns,queue_depth,payload_bytes,cumulative_drops\n";
            output << std::fixed << std::setprecision(3);
            for (const auto& metric : videoMetrics) {
                output << metric.accessUnitSequence << ',' << metric.sourcePresentationUs << ','
                       << metric.captureWindowsNs << ',' << metric.commonPresentationUs << ','
                       << metric.enqueuedNs << ',' << metric.dequeuedNs << ',' << metric.writeCompleteNs << ','
                       << metric.queueDepth << ',' << metric.payloadBytes << ',' << metric.cumulativeDrops << '\n';
            }
        }
    }
    if (!audioMetrics.empty() && !audioMetricsPath_.empty()) {
        std::ofstream output(audioMetricsPath_, std::ios::trunc);
        if (output) {
            output << "device_position,capture_qpc_ns,common_presentation_us,clock_drift_ms,frames,queue_depth,"
                      "enqueued_ns,dequeued_ns,cumulative_drops,cumulative_overruns,cumulative_underruns,"
                      "cumulative_timestamp_errors,discontinuity,timestamp_error\n";
            output << std::fixed << std::setprecision(3);
            for (const auto& metric : audioMetrics) {
                output << metric.devicePosition << ',' << metric.captureQpcNs << ','
                       << metric.commonPresentationUs << ',' << metric.clockDriftMs << ',' << metric.frames << ','
                       << metric.queueDepth << ',' << metric.enqueuedNs << ',' << metric.dequeuedNs << ','
                       << metric.cumulativeDrops << ',' << metric.cumulativeOverruns << ','
                       << metric.cumulativeUnderruns << ',' << metric.cumulativeTimestampErrors << ','
                       << metric.discontinuity << ',' << metric.timestampError << '\n';
            }
        }
    }
}

int64_t DirectH264Recorder::VideoCaptureWindowsNs(const EncodedAccessUnit& packet) const {
    if (packet.timing.synchronizedCaptureWindowsNs > 0) return packet.timing.synchronizedCaptureWindowsNs;
    if (packet.timing.captureTimestampNs > 0 && !audioFormat_) return packet.timing.captureTimestampNs;
    return static_cast<int64_t>(std::llround(packet.timing.windowsReceiveNs));
}

int64_t DirectH264Recorder::AudioCaptureWindowsNs(const AudioCaptureChunk& chunk) const {
    return chunk.captureQpcNs;
}

bool DirectH264Recorder::ContainsNalType(const std::vector<uint8_t>& bytes, uint8_t type) {
    for (size_t offset = 0; offset + 4 < bytes.size(); ++offset) {
        size_t header = 0;
        if (bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 1) header = offset + 3;
        else if (bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 0 && bytes[offset + 3] == 1)
            header = offset + 4;
        if (header && header < bytes.size() && (bytes[header] & 0x1f) == type) return true;
    }
    return false;
}
