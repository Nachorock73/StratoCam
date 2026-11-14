package com.androidcam.inspector

data class CameraReport(
    val generatedAtUtc: String,
    val device: DeviceInfo,
    val cameras: List<CameraCapability>,
    val mainRearSelection: MainRearSelection,
    val warnings: List<String>
)

data class DeviceInfo(
    val manufacturer: String,
    val model: String,
    val device: String,
    val androidVersion: String,
    val sdkInt: Int
)

data class CameraCapability(
    val cameraId: String,
    val lensFacing: String,
    val sensorOrientationDegrees: Int?,
    val hardwareLevel: String,
    val physicalCameraIds: List<String>,
    val focalLengthsMm: List<Float>,
    val sensorSizeMm: DimensionF?,
    val activeArray: String?,
    val outputFormats: List<OutputFormatCapability>,
    val aeTargetFpsRanges: List<String>,
    val highSpeedModes: List<HighSpeedMode>,
    val isoRange: String?,
    val exposureTimeRangeNs: String?,
    val autofocusModes: List<String>,
    val minimumFocusDistanceDiopters: Float?,
    val awbModes: List<String>,
    val awbLockAvailable: Boolean?,
    val aeCompensationRange: String?,
    val aeCompensationStep: String?,
    val opticalStabilizationModes: List<String>,
    val videoStabilizationModes: List<String>,
    val maxDigitalZoom: Float?,
    val zoomRatioRange: String?,
    val supportsControlZoomRatio: Boolean,
    val capabilities: List<String>,
    val supportsRaw: Boolean,
    val supportsManualSensor: Boolean,
    val supportsManualPostProcessing: Boolean,
    val inspectionWarnings: List<String>
)

data class DimensionF(val width: Float, val height: Float)

data class OutputFormatCapability(
    val name: String,
    val value: Int?,
    val sizes: List<String>
)

data class HighSpeedMode(
    val size: String,
    val fpsRanges: List<String>
)

data class MainRearSelection(
    val cameraId: String?,
    val confidence: String,
    val reason: String,
    val equivalentFocalLengthMm: Double?
)
