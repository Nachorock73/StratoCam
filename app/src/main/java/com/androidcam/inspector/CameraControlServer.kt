package com.androidcam.inspector

import android.util.Log
import org.json.JSONObject
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

class CameraControlServer(
    private val listener: (RemoteRequest) -> RemoteResponse
) {
    private val running = AtomicBoolean(false)
    @Volatile private var serverSocket: ServerSocket? = null
    @Volatile private var clientSocket: Socket? = null
    private var serverThread: Thread? = null

    @Synchronized
    fun start() {
        if (serverThread?.isAlive == true) return
        if (!running.compareAndSet(false, true)) return
        serverThread = Thread(::serve, "CameraControlServer").apply { start() }
    }

    @Synchronized
    fun stop() {
        running.set(false)
        runCatching { clientSocket?.close() }
        runCatching { serverSocket?.close() }
        val thread = serverThread
        thread?.join(SERVER_STOP_TIMEOUT_MS)
        if (thread?.isAlive == true) {
            Log.e(TAG, "Control server did not stop within ${SERVER_STOP_TIMEOUT_MS}ms")
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
                socket.bind(InetSocketAddress(InetAddress.getByName(LOOPBACK_ADDRESS), REMOTE_CONTROL_PORT))
                Log.i(TAG, "Control server listening on $LOOPBACK_ADDRESS:$REMOTE_CONTROL_PORT (ADB loopback only)")
                while (running.get()) {
                    val client = try {
                        socket.accept()
                    } catch (error: SocketException) {
                        if (running.get()) throw error else break
                    }
                    clientSocket = client
                    try {
                        client.use(::handleClient)
                    } finally {
                        if (clientSocket === client) clientSocket = null
                    }
                }
            }
        } catch (error: Exception) {
            if (running.get()) Log.e(TAG, "Control server failed", error)
        } finally {
            serverSocket = null
            running.set(false)
            Log.i(TAG, "Control server stopped")
        }
    }

    private fun handleClient(client: java.net.Socket) {
        client.soTimeout = CLIENT_TIMEOUT_MS
        val reader = BufferedReader(InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8))
        val writer = BufferedWriter(OutputStreamWriter(client.getOutputStream(), StandardCharsets.UTF_8))
        val response = try {
            val line = reader.readLine() ?: throw IllegalArgumentException("Empty request")
            val request = RemoteJsonCodec.decodeRequest(line)
            Log.i(TAG, "USB command received: ${request.command.javaClass.simpleName}, id=${request.id}")
            listener(request)
        } catch (error: Exception) {
            Log.w(TAG, "Rejected control request: ${error.message}")
            RemoteResponse(id = "", ok = false, message = error.message ?: "Invalid request")
        }
        writer.write(RemoteJsonCodec.encodeResponse(response))
        writer.newLine()
        writer.flush()
    }

    companion object {
        private const val TAG = "CameraControlServer"
        private const val LOOPBACK_ADDRESS = "127.0.0.1"
        private const val CLIENT_TIMEOUT_MS = 5_000
        private const val SERVER_STOP_TIMEOUT_MS = 2_000L
    }
}

object RemoteJsonCodec {
    fun decodeRequest(json: String): RemoteRequest {
        val root = JSONObject(json)
        require(root.getInt("version") == REMOTE_CONTROL_PROTOCOL_VERSION) { "Unsupported protocol version" }
        val id = root.optString("id", "")
        val params = root.optJSONObject("params") ?: JSONObject()
        val command = when (root.getString("command")) {
            "ping" -> RemoteCameraCommand.Ping
            "get_state" -> RemoteCameraCommand.GetState
            "get_capabilities" -> RemoteCameraCommand.GetCapabilities
            "get_device_status" -> RemoteCameraCommand.GetDeviceStatus
            "set_video_mode" -> RemoteCameraCommand.SetVideoMode(
                width = params.getInt("width"),
                height = params.getInt("height"),
                fps = params.getInt("fps"),
                bitrate = params.getInt("bitrate")
            )
            "set_exposure" -> RemoteCameraCommand.SetExposure(
                auto = params.getBoolean("auto"),
                iso = params.optionalInt("iso"),
                exposureTimeNs = params.optionalLong("exposureTimeNs")
            )
            "set_focus" -> RemoteCameraCommand.SetFocus(
                auto = params.getBoolean("auto"),
                focusDistanceDiopters = params.optionalFloat("focusDistanceDiopters")
            )
            "set_white_balance" -> RemoteCameraCommand.SetWhiteBalance(
                auto = params.getBoolean("auto"),
                redGain = params.optionalFloat("redGain"),
                blueGain = params.optionalFloat("blueGain")
            )
            "set_zoom" -> RemoteCameraCommand.SetZoom(params.getDouble("zoomRatio").toFloat())
            "focus_at" -> RemoteCameraCommand.FocusAt(
                sensorX = params.getInt("sensorX"),
                sensorY = params.getInt("sensorY")
            )
            "get_focus_status" -> RemoteCameraCommand.GetFocusStatus
            "resume_continuous_focus" -> RemoteCameraCommand.ResumeContinuousFocus
            "reset_auto" -> RemoteCameraCommand.ResetAuto
            else -> throw IllegalArgumentException("Unknown command: ${root.getString("command")}")
        }
        return RemoteRequest(id, command)
    }

    fun encodeResponse(response: RemoteResponse): String = JSONObject().apply {
        put("version", REMOTE_CONTROL_PROTOCOL_VERSION)
        put("id", response.id)
        put("ok", response.ok)
        response.message?.let { put("message", it) }
        response.state?.let { put("state", encodeState(it)) }
        response.capabilities?.let { put("capabilities", encodeCapabilities(it)) }
        response.focusStatus?.let { focus ->
            put("focus", JSONObject().apply {
                put("requestId", focus.requestId)
                put("status", focus.status)
                put("sensorX", focus.sensorX)
                put("sensorY", focus.sensorY)
            })
        }
        response.deviceStatus?.let { status ->
            put("deviceStatus", JSONObject().apply {
                status.batteryPercent?.let { put("batteryPercent", it) }
                put("batteryState", status.batteryState)
                put("powerSource", status.powerSource)
                put("batteryHealth", status.batteryHealth)
                status.batteryTemperatureC?.let { put("batteryTemperatureC", it.toDouble()) }
                status.currentNowMilliamps?.let { put("currentNowMilliamps", it.toDouble()) }
                status.currentAverageMilliamps?.let { put("currentAverageMilliamps", it.toDouble()) }
                status.voltageVolts?.let { put("voltageVolts", it.toDouble()) }
                status.approximatePowerWatts?.let { put("approximatePowerWatts", it.toDouble()) }
                put("thermalStatus", status.thermalStatus)
            })
        }
    }.toString()

    private fun encodeState(state: CameraControlState) = JSONObject().apply {
        put("autoExposure", state.autoExposure)
        put("iso", state.iso)
        put("exposureTimeNs", state.exposureTimeNs)
        put("autoFocus", state.autoFocus)
        put("focusDistanceDiopters", state.focusDistanceDiopters.toDouble())
        put("autoWhiteBalance", state.autoWhiteBalance)
        put("redGain", state.redGain.toDouble())
        put("greenEvenGain", state.greenEvenGain.toDouble())
        put("greenOddGain", state.greenOddGain.toDouble())
        put("blueGain", state.blueGain.toDouble())
        put("zoomRatio", state.zoomRatio.toDouble())
    }

    private fun encodeCapabilities(capabilities: ManualControlCapabilities) = JSONObject().apply {
        put("manualExposureSupported", capabilities.manualExposureSupported)
        put("isoMin", capabilities.isoMin)
        put("isoMax", capabilities.isoMax)
        put("exposureTimeMinNs", capabilities.exposureTimeMinNs)
        put("exposureTimeMaxNs", capabilities.exposureTimeMaxNs)
        put("maxFrameDurationNs", capabilities.maxFrameDurationNs)
        put("manualFocusSupported", capabilities.manualFocusSupported)
        put("maxFocusDistanceDiopters", capabilities.maxFocusDistanceDiopters.toDouble())
        put("manualWhiteBalanceSupported", capabilities.manualWhiteBalanceSupported)
        put("colorGainMin", capabilities.colorGainMin.toDouble())
        put("colorGainMax", capabilities.colorGainMax.toDouble())
        put("zoomMin", capabilities.zoomMin.toDouble())
        put("zoomMax", capabilities.zoomMax.toDouble())
        put("zoomRatioSupported", capabilities.zoomRatioSupported)
        put("maxAfRegions", capabilities.maxAfRegions)
        put("activeArrayLeft", capabilities.activeArrayLeft)
        put("activeArrayTop", capabilities.activeArrayTop)
        put("activeArrayRight", capabilities.activeArrayRight)
        put("activeArrayBottom", capabilities.activeArrayBottom)
    }

    private fun JSONObject.optionalInt(name: String): Int? = if (has(name)) getInt(name) else null
    private fun JSONObject.optionalLong(name: String): Long? = if (has(name)) getLong(name) else null
    private fun JSONObject.optionalFloat(name: String): Float? = if (has(name)) getDouble(name).toFloat() else null
}
