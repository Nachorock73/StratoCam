package com.androidcam.inspector

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Bundle
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

data class VideoEncoderConfiguration(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val keyFrameIntervalSeconds: Int
)

class H264VideoEncoder(
    private val configuration: VideoEncoderConfiguration,
    private val packetSink: (VideoPacket) -> Unit,
    private val errorListener: (String, Throwable?) -> Unit
) {
    private var codec: MediaCodec? = null
    private var callbackThread: HandlerThread? = null
    private var callbackHandler: Handler? = null
    lateinit var inputSurface: Surface
        private set
    private var encodedFrames = 0L
    private var encodedBytes = 0L
    private var encoderAgeSamples = 0L
    private var encoderAgeTotalMs = 0.0
    private var encoderAgeMaxMs = 0.0
    private var statisticsStartNs = 0L
    private var captureTimingMatches = 0L
    private var captureTimingMisses = 0L
    private val captureTimingLock = Any()
    private val pendingCaptureTimings = LinkedHashMap<Long, CaptureTiming>()

    private data class CaptureTiming(
        val frameNumber: Long,
        val timestampNs: Long,
        val callbackTimestampNs: Long
    )

    fun recordCaptureStarted(
        frameNumber: Long,
        sensorTimestampNs: Long,
        callbackTimestampNs: Long,
        captureTimestampSource: String
    ) {
        val encoderTimestampNs = if (captureTimestampSource == "REALTIME") {
            val realtimeToNanoTimeOffsetNs = SystemClock.elapsedRealtimeNanos() - System.nanoTime()
            sensorTimestampNs - realtimeToNanoTimeOffsetNs
        } else {
            sensorTimestampNs
        }
        synchronized(captureTimingLock) {
            pendingCaptureTimings[encoderTimestampNs / 1_000L] = CaptureTiming(
                frameNumber,
                sensorTimestampNs,
                callbackTimestampNs
            )
            while (pendingCaptureTimings.size > MAX_PENDING_CAPTURE_TIMINGS) {
                val oldestKey = pendingCaptureTimings.keys.firstOrNull() ?: break
                pendingCaptureTimings.remove(oldestKey)
            }
        }
    }

    fun start() {
        check(codec == null) { "Encoder is already running" }
        val format = MediaFormat.createVideoFormat(MIME_TYPE, configuration.width, configuration.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, configuration.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, configuration.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, configuration.keyFrameIntervalSeconds)
            if (Build.VERSION.SDK_INT >= 29) {
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
        }
        val codecInfo = requireNotNull(selectEncoder()) {
            "No H.264 surface encoder supports ${configuration.width}x${configuration.height}@${configuration.fps}"
        }
        val capabilities = codecInfo.getCapabilitiesForType(MIME_TYPE)
        if (capabilities.encoderCapabilities.isBitrateModeSupported(
                MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR
            )
        ) {
            format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        }
        val codecName = codecInfo.name
        val thread = HandlerThread("H264EncoderCallback").also { it.start() }
        callbackThread = thread
        callbackHandler = Handler(thread.looper)
        try {
            val encoder = MediaCodec.createByCodecName(codecName)
            codec = encoder
            encoder.setCallback(codecCallback, callbackHandler)
            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = encoder.createInputSurface()
            encoder.start()
            statisticsStartNs = System.nanoTime()
            captureTimingMatches = 0L
            captureTimingMisses = 0L
            synchronized(captureTimingLock) { pendingCaptureTimings.clear() }
            Log.i(
                TAG,
                "H.264 encoder started: name=$codecName, ${configuration.width}x${configuration.height}@" +
                    "${configuration.fps}, bitrate=${configuration.bitrate}, maxBFrames=0"
            )
        } catch (error: Exception) {
            stop()
            throw error
        }
    }

    private fun selectEncoder(): MediaCodecInfo? {
        val candidates = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.filter { info ->
            info.isEncoder && info.supportedTypes.any { it.equals(MIME_TYPE, ignoreCase = true) }
        }
        candidates.forEach { info ->
            val capabilities = runCatching { info.getCapabilitiesForType(MIME_TYPE) }.getOrNull() ?: return@forEach
            val supportsSurface = MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface in capabilities.colorFormats
            val video = capabilities.videoCapabilities
            val supportsMode = runCatching {
                video.areSizeAndRateSupported(
                    configuration.width,
                    configuration.height,
                    configuration.fps.toDouble()
                )
            }.getOrDefault(false)
            Log.i(
                TAG,
                "H.264 encoder candidate: ${info.name}, surface=$supportsSurface, mode=$supportsMode, " +
                    "hardware=${Build.VERSION.SDK_INT < 29 || info.isHardwareAccelerated}"
            )
        }
        return candidates
            .filter { info ->
                val capabilities = runCatching { info.getCapabilitiesForType(MIME_TYPE) }.getOrNull()
                    ?: return@filter false
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface in capabilities.colorFormats &&
                    runCatching {
                        capabilities.videoCapabilities.areSizeAndRateSupported(
                            configuration.width,
                            configuration.height,
                            configuration.fps.toDouble()
                        )
                    }.getOrDefault(false)
            }
            .sortedByDescending { info -> Build.VERSION.SDK_INT < 29 || info.isHardwareAccelerated }
            .firstOrNull()
    }

    fun requestKeyFrame() {
        callbackHandler?.post {
            try {
                codec?.setParameters(Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                })
                Log.i(TAG, "Requested H.264 sync frame for new USB receiver")
            } catch (error: Exception) {
                Log.w(TAG, "Could not request H.264 sync frame", error)
            }
        }
    }

    fun stop() {
        val activeCodec = codec
        codec = null
        runCatching { activeCodec?.stop() }
        runCatching { activeCodec?.release() }
        if (::inputSurface.isInitialized) runCatching { inputSurface.release() }
        val thread = callbackThread
        callbackThread = null
        callbackHandler = null
        thread?.quitSafely()
        runCatching { thread?.join(1_000) }
        if (activeCodec != null) Log.i(TAG, "H.264 encoder stopped")
        synchronized(captureTimingLock) { pendingCaptureTimings.clear() }
    }

    private val codecCallback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit

        override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            val encoderOutputTimestampNs = SystemClock.elapsedRealtimeNanos()
            try {
                if (info.size > 0) {
                    val output = requireNotNull(codec.getOutputBuffer(index))
                    val bytes = output.readBytes(info.offset, info.size)
                    val packetFlags =
                        (if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) VIDEO_FLAG_CODEC_CONFIG else 0) or
                            (if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) VIDEO_FLAG_KEY_FRAME else 0)
                    val payload = H264AnnexB.normalize(bytes)
                    val captureTiming = if (packetFlags and VIDEO_FLAG_CODEC_CONFIG == 0) {
                        takeCaptureTiming(info.presentationTimeUs)
                    } else null
                    packetSink(
                        VideoPacket(
                            flags = packetFlags,
                            presentationTimeUs = info.presentationTimeUs,
                            payload = payload,
                            frameNumber = captureTiming?.frameNumber ?: -1L,
                            captureTimestampNs = captureTiming?.timestampNs ?: 0L,
                            captureCallbackTimestampNs = captureTiming?.callbackTimestampNs ?: 0L,
                            encoderOutputTimestampNs = encoderOutputTimestampNs
                        )
                    )
                    if (packetFlags and VIDEO_FLAG_CODEC_CONFIG == 0) {
                        recordStatistics(payload.size, captureTiming?.timestampNs, encoderOutputTimestampNs)
                    }
                }
            } catch (error: Exception) {
                Log.e(TAG, "Failed to consume H.264 output", error)
                errorListener("Failed to consume H.264 output", error)
            } finally {
                runCatching { codec.releaseOutputBuffer(index, false) }
            }
        }

        override fun onError(codec: MediaCodec, exception: MediaCodec.CodecException) {
            Log.e(TAG, "H.264 encoder error", exception)
            errorListener("H.264 encoder error: ${exception.diagnosticInfo}", exception)
        }

        override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
            val codecSpecificData = mutableListOf<ByteArray>()
            var index = 0
            while (true) {
                val buffer = format.getByteBuffer("csd-$index") ?: break
                codecSpecificData += buffer.readRemainingBytes()
                index += 1
            }
            val payload = H264AnnexB.combineCodecSpecificData(codecSpecificData)
            if (payload.isNotEmpty()) {
                packetSink(VideoPacket(VIDEO_FLAG_CODEC_CONFIG, 0L, payload))
            }
            val avc = describeAvcProfileAndLevel(payload)
            Log.i(TAG, "H.264 output format ready: $format, $avc, codecConfigBytes=${payload.size}")
        }
    }

    private fun describeAvcProfileAndLevel(codecConfig: ByteArray): String {
        for (index in 0 until codecConfig.size - 7) {
            val start = codecConfig[index] == 0.toByte() && codecConfig[index + 1] == 0.toByte() &&
                ((codecConfig[index + 2] == 1.toByte()) ||
                    (codecConfig[index + 2] == 0.toByte() && codecConfig[index + 3] == 1.toByte()))
            if (!start) continue
            val nal = index + if (codecConfig[index + 2] == 1.toByte()) 3 else 4
            if (nal + 3 >= codecConfig.size || (codecConfig[nal].toInt() and 0x1f) != 7) continue
            val profileIdc = codecConfig[nal + 1].toInt() and 0xff
            val levelIdc = codecConfig[nal + 3].toInt() and 0xff
            val profile = when (profileIdc) {
                66 -> "Baseline"
                77 -> "Main"
                88 -> "Extended"
                100 -> "High"
                110 -> "High10"
                122 -> "High422"
                244 -> "High444"
                else -> "idc-$profileIdc"
            }
            return "profile=$profile, level=${levelIdc / 10}.${levelIdc % 10}"
        }
        return "profile=unknown, level=unknown"
    }

    private fun takeCaptureTiming(presentationTimeUs: Long): CaptureTiming? = synchronized(captureTimingLock) {
        pendingCaptureTimings.remove(presentationTimeUs)?.also {
            captureTimingMatches += 1
            return@synchronized it
        }
        val nearest = pendingCaptureTimings.keys.minByOrNull { key -> kotlin.math.abs(key - presentationTimeUs) }
        val nearestDeltaUs = nearest?.let { it - presentationTimeUs }
        if (nearest != null && kotlin.math.abs(requireNotNull(nearestDeltaUs)) <= CAPTURE_PTS_MATCH_TOLERANCE_US) {
            captureTimingMatches += 1
            return@synchronized pendingCaptureTimings.remove(nearest)
        }
        captureTimingMisses += 1
        if (captureTimingMisses <= 5L || captureTimingMisses % STATISTICS_FRAME_INTERVAL == 0L) {
            Log.w(
                TAG,
                "Capture timing miss: ptsUs=$presentationTimeUs nearestDeltaUs=$nearestDeltaUs " +
                    "pending=${pendingCaptureTimings.size} matches=$captureTimingMatches misses=$captureTimingMisses"
            )
        }
        null
    }

    private fun recordStatistics(bytes: Int, captureTimestampNs: Long?, encoderOutputTimestampNs: Long) {
        encodedFrames += 1
        encodedBytes += bytes
        val ageMs = captureTimestampNs?.let { (encoderOutputTimestampNs - it) / 1_000_000.0 } ?: Double.NaN
        if (ageMs in 0.0..MAX_REASONABLE_ENCODER_AGE_MS) {
            encoderAgeSamples += 1
            encoderAgeTotalMs += ageMs
            encoderAgeMaxMs = maxOf(encoderAgeMaxMs, ageMs)
        }
        if (encodedFrames % STATISTICS_FRAME_INTERVAL != 0L) return
        val elapsedSeconds = (System.nanoTime() - statisticsStartNs) / 1_000_000_000.0
        if (elapsedSeconds <= 0.0) return
        val fps = encodedFrames / elapsedSeconds
        val bitrate = encodedBytes * 8.0 / elapsedSeconds
        val averageAgeMs = if (encoderAgeSamples > 0) encoderAgeTotalMs / encoderAgeSamples else Double.NaN
        Log.i(
            TAG,
            "Encoder effective: %.2f FPS, %.2f Mbps, outputAgeAvg=%.1f ms, outputAgeMax=%.1f ms".format(
                java.util.Locale.US,
                fps,
                bitrate / 1_000_000.0,
                averageAgeMs,
                encoderAgeMaxMs
            )
        )
    }

    private fun ByteBuffer.readRemainingBytes(): ByteArray {
        val duplicate = duplicate()
        val bytes = ByteArray(duplicate.remaining())
        duplicate.get(bytes)
        return bytes
    }

    private fun ByteBuffer.readBytes(offset: Int, size: Int): ByteArray {
        val duplicate = duplicate()
        duplicate.position(offset)
        duplicate.limit(offset + size)
        val bytes = ByteArray(size)
        duplicate.get(bytes)
        return bytes
    }

    companion object {
        private const val TAG = "H264VideoEncoder"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val STATISTICS_FRAME_INTERVAL = 120L
        private const val MAX_REASONABLE_ENCODER_AGE_MS = 5_000.0
        private const val MAX_PENDING_CAPTURE_TIMINGS = 256
        private const val CAPTURE_PTS_MATCH_TOLERANCE_US = 2_000L
    }
}
