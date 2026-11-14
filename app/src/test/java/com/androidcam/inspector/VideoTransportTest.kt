package com.androidcam.inspector

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class VideoTransportTest {
    @Test
    fun packetHeaderUsesDocumentedLittleEndianLayout() {
        val packet = VideoPacket(
            flags = VIDEO_FLAG_KEY_FRAME,
            presentationTimeUs = 123_456L,
            payload = byteArrayOf(1, 2, 3),
            frameNumber = 42L,
            captureTimestampNs = 123_456_789L,
            captureCallbackTimestampNs = 123_500_000L,
            encoderOutputTimestampNs = 123_600_000L
        )
        val header = ByteBuffer.wrap(
            VideoPacketCodec.encodeHeader(
                packet,
                androidSendTimestampNs = 123_700_000L,
                accessUnitSequence = 77L,
                encodedQueueDepth = 5,
                queueRecoveryCount = 2
            )
        ).order(ByteOrder.LITTLE_ENDIAN)

        assertEquals(VIDEO_PACKET_MAGIC, header.int)
        assertEquals(VIDEO_PROTOCOL_VERSION, header.int)
        assertEquals(VIDEO_FLAG_KEY_FRAME, header.int)
        assertEquals(3, header.int)
        assertEquals(123_456L, header.long)
        assertEquals(42L, header.long)
        assertEquals(123_456_789L, header.long)
        assertEquals(123_500_000L, header.long)
        assertEquals(123_600_000L, header.long)
        assertEquals(123_700_000L, header.long)
        assertEquals(77L, header.long)
        assertEquals(5, header.int)
        assertEquals(2, header.int)
    }

    @Test
    fun convertsLengthPrefixedAccessUnitToAnnexB() {
        val source = byteArrayOf(0, 0, 0, 3, 0x65, 0x11, 0x22)

        assertArrayEquals(
            byteArrayOf(0, 0, 0, 1, 0x65, 0x11, 0x22),
            H264AnnexB.normalize(source)
        )
    }

    @Test
    fun leavesAnnexBAccessUnitUnchanged() {
        val source = byteArrayOf(0, 0, 0, 1, 0x67, 0x01)

        assertArrayEquals(source, H264AnnexB.normalize(source))
    }

    @Test
    fun parsesAvcDecoderConfigurationRecord() {
        val avcConfig = byteArrayOf(
            1, 100, 0, 31, -1, -31,
            0, 3, 0x67, 0x64, 0x01,
            1, 0, 2, 0x68, -18
        )

        assertArrayEquals(
            byteArrayOf(0, 0, 0, 1, 0x67, 0x64, 0x01, 0, 0, 0, 1, 0x68, -18),
            H264AnnexB.normalize(avcConfig)
        )
    }
}
