package com.androidcam.inspector

const val REMOTE_CONTROL_PROTOCOL_VERSION = 1
const val REMOTE_CONTROL_PORT = 27183

data class RemoteRequest(
    val id: String,
    val command: RemoteCameraCommand
)

sealed interface RemoteCameraCommand {
    data object Ping : RemoteCameraCommand
    data object GetState : RemoteCameraCommand
    data object GetCapabilities : RemoteCameraCommand
    data object GetDeviceStatus : RemoteCameraCommand
    data class SetVideoMode(val width: Int, val height: Int, val fps: Int, val bitrate: Int) : RemoteCameraCommand
    data class SetExposure(
        val auto: Boolean,
        val iso: Int? = null,
        val exposureTimeNs: Long? = null
    ) : RemoteCameraCommand
    data class SetFocus(
        val auto: Boolean,
        val focusDistanceDiopters: Float? = null
    ) : RemoteCameraCommand
    data class SetWhiteBalance(
        val auto: Boolean,
        val redGain: Float? = null,
        val blueGain: Float? = null
    ) : RemoteCameraCommand
    data class SetZoom(val zoomRatio: Float) : RemoteCameraCommand
    data class FocusAt(val sensorX: Int, val sensorY: Int) : RemoteCameraCommand
    data object GetFocusStatus : RemoteCameraCommand
    data object ResumeContinuousFocus : RemoteCameraCommand
    data object ResetAuto : RemoteCameraCommand
}

data class FocusPointStatus(
    val requestId: String,
    val status: String,
    val sensorX: Int,
    val sensorY: Int
)

data class RemoteResponse(
    val id: String,
    val ok: Boolean,
    val message: String? = null,
    val state: CameraControlState? = null,
    val capabilities: ManualControlCapabilities? = null,
    val focusStatus: FocusPointStatus? = null,
    val deviceStatus: DeviceStatus? = null
)

data class RemoteCommandResult(
    val state: CameraControlState,
    val changed: Boolean
)

object RemoteControlProcessor {
    fun apply(
        command: RemoteCameraCommand,
        current: CameraControlState,
        capabilities: ManualControlCapabilities
    ): RemoteCommandResult {
        val candidate = when (command) {
            RemoteCameraCommand.Ping,
            RemoteCameraCommand.GetState,
            RemoteCameraCommand.GetCapabilities,
            RemoteCameraCommand.GetDeviceStatus,
            is RemoteCameraCommand.SetVideoMode,
            RemoteCameraCommand.GetFocusStatus,
            RemoteCameraCommand.ResumeContinuousFocus,
            is RemoteCameraCommand.FocusAt -> current

            is RemoteCameraCommand.SetExposure -> current.copy(
                autoExposure = command.auto,
                iso = command.iso ?: current.iso,
                exposureTimeNs = command.exposureTimeNs ?: current.exposureTimeNs
            )

            is RemoteCameraCommand.SetFocus -> current.copy(
                autoFocus = command.auto,
                focusDistanceDiopters = command.focusDistanceDiopters ?: current.focusDistanceDiopters
            )

            is RemoteCameraCommand.SetWhiteBalance -> current.copy(
                autoWhiteBalance = command.auto,
                redGain = command.redGain ?: current.redGain,
                blueGain = command.blueGain ?: current.blueGain
            )

            is RemoteCameraCommand.SetZoom -> current.copy(zoomRatio = command.zoomRatio)
            RemoteCameraCommand.ResetAuto -> CameraControlPolicy.defaults(capabilities)
        }
        val sanitized = CameraControlPolicy.sanitize(candidate, capabilities)
        return RemoteCommandResult(sanitized, sanitized != current)
    }
}
