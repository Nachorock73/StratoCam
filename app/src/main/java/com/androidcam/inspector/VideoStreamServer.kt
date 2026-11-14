package com.androidcam.inspector

import android.os.SystemClock
import android.util.Log
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

class VideoStreamServer(
    @Volatile private var streamInfo: VideoStreamInfo,
    private val onClientConnected: () -> Unit
) {
    private val running = AtomicBoolean(false)
    private val droppedPackets = AtomicLong(0)
    private val maxQueueDepth = AtomicInteger(0)
    private val recoveryCount = AtomicInteger(0)
    private val clientLock = Any()
    private val streamInfoLock = Object()
    @Volatile private var serverSocket: ServerSocket? = null
    @Volatile private var clientSocket: Socket? = null
    @Volatile private var clientQueue: ArrayBlockingQueue<VideoPacket>? = null
    @Volatile private var codecConfig: VideoPacket? = null
    @Volatile private var waitingForRecoveryKeyFrame = false
    private var serverThread: Thread? = null

    @Synchronized
    fun start() {
        if (serverThread?.isAlive == true) return
        if (!running.compareAndSet(false, true)) return
        serverThread = Thread(::serve, "VideoStreamServer").apply { start() }
    }

    fun publish(packet: VideoPacket) {
        if (packet.flags and VIDEO_FLAG_CODEC_CONFIG != 0) codecConfig = packet
        val queue = clientQueue ?: return
        if (packet.flags and VIDEO_FLAG_CODEC_CONFIG != 0) {
            queue.offer(packet)
            recordQueueDepth(queue.size)
            return
        }
        if (waitingForRecoveryKeyFrame) {
            if (packet.flags and VIDEO_FLAG_KEY_FRAME == 0) {
                droppedPackets.incrementAndGet()
                return
            }
            queue.clear()
            codecConfig?.let(queue::offer)
            queue.offer(packet)
            waitingForRecoveryKeyFrame = false
            Log.i(TAG, "Live-edge recovery resumed on requested IDR; recoveries=${recoveryCount.get()}")
            recordQueueDepth(queue.size)
            return
        }
        if (queue.size >= MAX_LIVE_QUEUE_DEPTH || !queue.offer(packet)) {
            val discarded = queue.size + 1
            queue.clear()
            droppedPackets.addAndGet(discarded.toLong())
            waitingForRecoveryKeyFrame = true
            val recoveries = recoveryCount.incrementAndGet()
            Log.w(
                TAG,
                "Live-edge backlog detected: discarded=$discarded depth=$discarded; " +
                    "waiting for IDR, recoveries=$recoveries"
            )
            onClientConnected()
            if (packet.flags and VIDEO_FLAG_KEY_FRAME != 0) {
                codecConfig?.let(queue::offer)
                queue.offer(packet)
                waitingForRecoveryKeyFrame = false
            }
            return
        }
        recordQueueDepth(queue.size)
    }

    fun updateStreamInfo(info: VideoStreamInfo) {
        synchronized(streamInfoLock) {
            streamInfo = info
            streamInfoLock.notifyAll()
        }
    }

    @Synchronized
    fun stop() {
        running.set(false)
        runCatching { clientSocket?.close() }
        runCatching { serverSocket?.close() }
        val thread = serverThread
        thread?.join(SERVER_STOP_TIMEOUT_MS)
        if (thread?.isAlive == true) {
            Log.e(TAG, "Video server did not stop within ${SERVER_STOP_TIMEOUT_MS}ms")
            thread.interrupt()
        } else {
            serverThread = null
        }
    }

    private fun serve() {
        try {
            ServerSocket().use { socket ->
                serverSocket = socket
                socket.reuseAddress = true
                socket.bind(InetSocketAddress(InetAddress.getByName(LOOPBACK_ADDRESS), VIDEO_STREAM_PORT))
                Log.i(TAG, "Video server listening on $LOOPBACK_ADDRESS:$VIDEO_STREAM_PORT (ADB loopback only)")
                while (running.get()) {
                    val client = try {
                        socket.accept()
                    } catch (error: SocketException) {
                        if (running.get()) throw error else break
                    }
                    streamToClient(client)
                }
            }
        } catch (error: Exception) {
            if (running.get()) Log.e(TAG, "Video server failed", error)
        } finally {
            serverSocket = null
            running.set(false)
            Log.i(TAG, "Video server stopped")
        }
    }

    private fun streamToClient(client: Socket) {
        val queue = ArrayBlockingQueue<VideoPacket>(MAX_QUEUED_PACKETS)
        synchronized(clientLock) {
            clientSocket = client
            clientQueue = queue
            waitingForRecoveryKeyFrame = false
        }
        try {
            client.use { socket ->
                socket.tcpNoDelay = true
                socket.soTimeout = CLOCK_SYNC_TIMEOUT_MS
                val input = BufferedInputStream(socket.getInputStream())
                val output = BufferedOutputStream(socket.getOutputStream(), OUTPUT_BUFFER_BYTES)
                synchronizeMonotonicClock(input, output)
                socket.soTimeout = 0
                awaitConfiguredStreamInfo()
                writePacket(output, streamInfoPacket())
                codecConfig?.let { writePacket(output, it) }
                output.flush()
                droppedPackets.set(0)
                maxQueueDepth.set(0)
                recoveryCount.set(0)
                var transmittedPackets = 0L
                Log.i(TAG, "Windows video receiver connected through ADB")
                onClientConnected()
                while (running.get()) {
                    val packet = queue.poll(1, TimeUnit.SECONDS) ?: continue
                    transmittedPackets += 1
                    writePacket(output, packet, transmittedPackets, queue.size)
                    output.flush()
                    if (transmittedPackets % QUEUE_STATISTICS_PACKET_INTERVAL == 0L) {
                        Log.i(
                            TAG,
                            "USB video queue: current=${queue.size}, max=${maxQueueDepth.get()}, " +
                                "dropped=${droppedPackets.get()}"
                        )
                    }
                }
            }
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
        } catch (error: Exception) {
            if (running.get()) Log.i(TAG, "Windows video receiver disconnected: ${error.message}")
        } finally {
            synchronized(clientLock) {
                if (clientSocket === client) {
                    clientSocket = null
                    clientQueue = null
                }
            }
        }
    }

    private fun writePacket(
        output: BufferedOutputStream,
        packet: VideoPacket,
        accessUnitSequence: Long = 0L,
        encodedQueueDepth: Int = 0
    ) {
        val sendTimestampNs = SystemClock.elapsedRealtimeNanos()
        output.write(
            VideoPacketCodec.encodeHeader(
                packet,
                sendTimestampNs,
                accessUnitSequence,
                encodedQueueDepth,
                recoveryCount.get()
            )
        )
        output.write(packet.payload)
    }

    private fun awaitConfiguredStreamInfo() {
        val deadlineNs = SystemClock.elapsedRealtimeNanos() + STREAM_INFO_WAIT_MS * 1_000_000L
        synchronized(streamInfoLock) {
            while (!streamInfo.configured) {
                val remainingNs = deadlineNs - SystemClock.elapsedRealtimeNanos()
                if (remainingNs <= 0L) {
                    Log.w(TAG, "Timed out waiting for configured Camera2 stream metadata")
                    return
                }
                streamInfoLock.wait((remainingNs / 1_000_000L).coerceAtLeast(1L))
            }
        }
    }

    private fun synchronizeMonotonicClock(input: BufferedInputStream, output: BufferedOutputStream) {
        repeat(CLOCK_SYNC_SAMPLE_COUNT) {
            val requestBytes = input.readExactly(CLOCK_SYNC_REQUEST_SIZE)
            val receiveTimestampNs = SystemClock.elapsedRealtimeNanos()
            val request = ByteBuffer.wrap(requestBytes).order(ByteOrder.LITTLE_ENDIAN)
            val magic = request.int
            val version = request.int
            val sequence = request.long
            require(magic == CLOCK_SYNC_MAGIC) { "Invalid clock sync magic: 0x${magic.toString(16)}" }
            require(version == CLOCK_SYNC_VERSION) { "Unsupported clock sync version: $version" }
            val sendTimestampNs = SystemClock.elapsedRealtimeNanos()
            val response = ByteBuffer.allocate(CLOCK_SYNC_RESPONSE_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN)
                .putInt(CLOCK_SYNC_MAGIC)
                .putInt(CLOCK_SYNC_VERSION)
                .putLong(sequence)
                .putLong(receiveTimestampNs)
                .putLong(sendTimestampNs)
                .array()
            output.write(response)
            output.flush()
        }
        Log.i(TAG, "Windows/Android monotonic clock synchronization completed")
    }

    private fun BufferedInputStream.readExactly(count: Int): ByteArray {
        val bytes = ByteArray(count)
        var offset = 0
        while (offset < count) {
            val read = read(bytes, offset, count - offset)
            if (read < 0) throw java.io.EOFException("Clock sync connection closed after $offset of $count bytes")
            offset += read
        }
        return bytes
    }

    private fun recordQueueDepth(observedDepth: Int) {
        var current = maxQueueDepth.get()
        while (observedDepth > current && !maxQueueDepth.compareAndSet(current, observedDepth)) {
            current = maxQueueDepth.get()
        }
    }

    private fun streamInfoPacket(): VideoPacket {
        val json = JSONObject().apply {
            put("codec", streamInfo.codec)
            put("width", streamInfo.width)
            put("height", streamInfo.height)
            put("fps", streamInfo.fps)
            put("bitrate", streamInfo.bitrate)
            put("sensorOrientationDegrees", streamInfo.sensorOrientationDegrees)
            put("captureTimestampSource", streamInfo.captureTimestampSource)
            put("configured", streamInfo.configured)
        }.toString()
        return VideoPacket(VIDEO_FLAG_STREAM_INFO, 0L, json.toByteArray(StandardCharsets.UTF_8))
    }

    companion object {
        private const val TAG = "VideoStreamServer"
        private const val LOOPBACK_ADDRESS = "127.0.0.1"
        private const val MAX_QUEUED_PACKETS = 90
        private const val MAX_LIVE_QUEUE_DEPTH = 12
        private const val OUTPUT_BUFFER_BYTES = 256 * 1024
        private const val QUEUE_STATISTICS_PACKET_INTERVAL = 120L
        private const val CLOCK_SYNC_TIMEOUT_MS = 10_000
        private const val STREAM_INFO_WAIT_MS = 3_000L
        private const val SERVER_STOP_TIMEOUT_MS = 2_000L
    }
}
