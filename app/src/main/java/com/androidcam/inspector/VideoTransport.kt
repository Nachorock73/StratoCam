package com.androidcam.inspector

import java.nio.ByteBuffer
import java.nio.ByteOrder

const val VIDEO_STREAM_PORT = 27184
const val VIDEO_PROTOCOL_VERSION = 3
const val VIDEO_PACKET_HEADER_SIZE = 80
const val VIDEO_PACKET_MAGIC = 0x31564341 // "ACV1" when written little-endian.
const val VIDEO_FLAG_CODEC_CONFIG = 1
const val VIDEO_FLAG_KEY_FRAME = 1 shl 1
const val VIDEO_FLAG_STREAM_INFO = 1 shl 2
const val CLOCK_SYNC_MAGIC = 0x31534341 // "ACS1" when written little-endian.
const val CLOCK_SYNC_VERSION = 1
const val CLOCK_SYNC_SAMPLE_COUNT = 9
const val CLOCK_SYNC_REQUEST_SIZE = 16
const val CLOCK_SYNC_RESPONSE_SIZE = 32

data class VideoPacket(
    val flags: Int,
    val presentationTimeUs: Long,
    val payload: ByteArray,
    val frameNumber: Long = -1L,
    val captureTimestampNs: Long = 0L,
    val captureCallbackTimestampNs: Long = 0L,
    val encoderOutputTimestampNs: Long = 0L
)

data class VideoStreamInfo(
    val codec: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val sensorOrientationDegrees: Int,
    val captureTimestampSource: String = "UNKNOWN",
    val configured: Boolean = false
)

object VideoPacketCodec {
    fun encodeHeader(
        packet: VideoPacket,
        androidSendTimestampNs: Long,
        accessUnitSequence: Long = 0L,
        encodedQueueDepth: Int = 0,
        queueRecoveryCount: Int = 0
    ): ByteArray = ByteBuffer
        .allocate(VIDEO_PACKET_HEADER_SIZE)
        .order(ByteOrder.LITTLE_ENDIAN)
        .putInt(VIDEO_PACKET_MAGIC)
        .putInt(VIDEO_PROTOCOL_VERSION)
        .putInt(packet.flags)
        .putInt(packet.payload.size)
        .putLong(packet.presentationTimeUs)
        .putLong(packet.frameNumber)
        .putLong(packet.captureTimestampNs)
        .putLong(packet.captureCallbackTimestampNs)
        .putLong(packet.encoderOutputTimestampNs)
        .putLong(androidSendTimestampNs)
        .putLong(accessUnitSequence)
        .putInt(encodedQueueDepth)
        .putInt(queueRecoveryCount)
        .array()
}

object H264AnnexB {
    private val startCode = byteArrayOf(0, 0, 0, 1)

    fun normalize(data: ByteArray): ByteArray {
        if (data.isEmpty() || hasStartCode(data)) return data
        parseAvcDecoderConfigurationRecord(data)?.let { return it }
        parseLengthPrefixed(data)?.let { return it }
        return startCode + data
    }

    fun combineCodecSpecificData(buffers: List<ByteArray>): ByteArray = buffers
        .filter { it.isNotEmpty() }
        .map(::normalize)
        .fold(ByteArray(0)) { combined, bytes -> combined + bytes }

    private fun hasStartCode(data: ByteArray): Boolean =
        data.size >= 4 && data[0] == 0.toByte() && data[1] == 0.toByte() &&
            (data[2] == 1.toByte() || (data[2] == 0.toByte() && data[3] == 1.toByte()))

    private fun parseLengthPrefixed(data: ByteArray): ByteArray? {
        val output = ArrayList<Byte>()
        var offset = 0
        var units = 0
        while (offset + 4 <= data.size) {
            val length = ((data[offset].toInt() and 0xff) shl 24) or
                ((data[offset + 1].toInt() and 0xff) shl 16) or
                ((data[offset + 2].toInt() and 0xff) shl 8) or
                (data[offset + 3].toInt() and 0xff)
            offset += 4
            if (length <= 0 || length > data.size - offset) return null
            startCode.forEach(output::add)
            repeat(length) { output.add(data[offset + it]) }
            offset += length
            units += 1
        }
        if (offset != data.size || units == 0) return null
        return output.toByteArray()
    }

    private fun parseAvcDecoderConfigurationRecord(data: ByteArray): ByteArray? {
        if (data.size < 7 || data[0] != 1.toByte()) return null
        val output = ArrayList<Byte>()
        var offset = 6
        val sequenceParameterSets = data[5].toInt() and 0x1f
        repeat(sequenceParameterSets) {
            if (offset + 2 > data.size) return null
            val length = unsignedShort(data, offset)
            offset += 2
            if (length <= 0 || offset + length > data.size) return null
            startCode.forEach(output::add)
            repeat(length) { output.add(data[offset + it]) }
            offset += length
        }
        if (offset >= data.size) return null
        val pictureParameterSets = data[offset].toInt() and 0xff
        offset += 1
        repeat(pictureParameterSets) {
            if (offset + 2 > data.size) return null
            val length = unsignedShort(data, offset)
            offset += 2
            if (length <= 0 || offset + length > data.size) return null
            startCode.forEach(output::add)
            repeat(length) { output.add(data[offset + it]) }
            offset += length
        }
        return if (output.isEmpty()) null else output.toByteArray()
    }

    private fun unsignedShort(data: ByteArray, offset: Int): Int =
        ((data[offset].toInt() and 0xff) shl 8) or (data[offset + 1].toInt() and 0xff)
}
