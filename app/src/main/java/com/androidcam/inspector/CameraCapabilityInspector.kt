package com.androidcam.inspector

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaRecorder
import android.os.Build
import android.util.Size
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

class CameraCapabilityInspector(context: Context) {
    private val cameraManager = context.getSystemService(CameraManager::class.java)

    fun inspect(): CameraReport {
        val warnings = mutableListOf<String>()
        val ids = try {
            cameraManager.cameraIdList.toList()
        } catch (error: Exception) {
            warnings += "Could not enumerate cameras: ${error.javaClass.simpleName}: ${error.message}"
            emptyList()
        }

        val cameras = ids.mapNotNull { cameraId ->
            try {
                inspectCamera(cameraId)
            } catch (error: Exception) {
                warnings += "Camera $cameraId could not be inspected: ${error.javaClass.simpleName}: ${error.message}"
                null
            }
        }

        if (Build.VERSION.SDK_INT < 28) {
            warnings += "Physical camera IDs require Android 9/API 28; this device only exposes logical/openable IDs to the inspector."
        }
        if (cameras.isEmpty()) warnings += "No inspectable Camera2 devices were found."

        return CameraReport(
            generatedAtUtc = utcTimestamp(),
            device = DeviceInfo(
                manufacturer = Build.MANUFACTURER,
                model = Build.MODEL,
                device = Build.DEVICE,
                androidVersion = Build.VERSION.RELEASE,
                sdkInt = Build.VERSION.SDK_INT
            ),
            cameras = cameras,
            mainRearSelection = MainRearSelector.select(cameras),
            warnings = warnings
        )
    }

    private fun inspectCamera(cameraId: String): CameraCapability {
        val characteristics = cameraManager.getCameraCharacteristics(cameraId)
        val warnings = mutableListOf<String>()
        val streamMap = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        if (streamMap == null) warnings += "SCALER_STREAM_CONFIGURATION_MAP is unavailable."

        val capabilityValues = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            ?.toList().orEmpty()
        val sensorSize = characteristics.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
            ?.let { DimensionF(it.width, it.height) }

        val outputFormats = mutableListOf<OutputFormatCapability>()
        streamMap?.outputFormats?.sorted()?.forEach { format ->
            val sizes = safeSizes(warnings, formatName(format)) { streamMap.getOutputSizes(format) }
            outputFormats += OutputFormatCapability(formatName(format), format, sizes)
        }
        streamMap?.let { map ->
            addClassOutput(outputFormats, warnings, "PRIVATE/SurfaceTexture") {
                map.getOutputSizes(SurfaceTexture::class.java)
            }
            addClassOutput(outputFormats, warnings, "MediaRecorder") {
                map.getOutputSizes(MediaRecorder::class.java)
            }
        }

        val highSpeedModes = streamMap?.highSpeedVideoSizes?.sortedWith(sizeComparator())?.map { size ->
            val ranges = try {
                streamMap.getHighSpeedVideoFpsRangesFor(size).map(::rangeString)
            } catch (error: RuntimeException) {
                warnings += "High-speed FPS lookup failed for ${sizeString(size)}: ${error.message}"
                emptyList()
            }
            HighSpeedMode(sizeString(size), ranges)
        }.orEmpty()

        val physicalIds = if (Build.VERSION.SDK_INT >= 28) {
            characteristics.physicalCameraIds.sorted()
        } else emptyList()

        val zoomRatioRange = if (Build.VERSION.SDK_INT >= 30) {
            characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else null

        return CameraCapability(
            cameraId = cameraId,
            lensFacing = lensFacingName(characteristics.get(CameraCharacteristics.LENS_FACING)),
            sensorOrientationDegrees = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION),
            hardwareLevel = hardwareLevelName(characteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)),
            physicalCameraIds = physicalIds,
            focalLengthsMm = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                ?.toList().orEmpty(),
            sensorSizeMm = sensorSize,
            activeArray = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)?.toShortString(),
            outputFormats = outputFormats,
            aeTargetFpsRanges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
                ?.map(::rangeString).orEmpty(),
            highSpeedModes = highSpeedModes,
            isoRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let(::rangeString),
            exposureTimeRangeNs = characteristics.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)?.let(::rangeString),
            autofocusModes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
                ?.map(::afModeName).orEmpty(),
            minimumFocusDistanceDiopters = characteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE),
            awbModes = characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES)
                ?.map(::awbModeName).orEmpty(),
            awbLockAvailable = characteristics.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE),
            aeCompensationRange = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)?.let(::rangeString),
            aeCompensationStep = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toString(),
            opticalStabilizationModes = characteristics.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)
                ?.map(::opticalStabilizationName).orEmpty(),
            videoStabilizationModes = characteristics.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)
                ?.map(::videoStabilizationName).orEmpty(),
            maxDigitalZoom = characteristics.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM),
            zoomRatioRange = zoomRatioRange?.let(::rangeString),
            supportsControlZoomRatio = zoomRatioRange != null,
            capabilities = capabilityValues.map(::capabilityName),
            supportsRaw = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW in capabilityValues,
            supportsManualSensor = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR in capabilityValues,
            supportsManualPostProcessing = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING in capabilityValues,
            inspectionWarnings = warnings
        )
    }

    private fun addClassOutput(
        outputs: MutableList<OutputFormatCapability>,
        warnings: MutableList<String>,
        name: String,
        getter: () -> Array<Size>?
    ) {
        val sizes = safeSizes(warnings, name, getter)
        if (sizes.isNotEmpty() && outputs.none { it.name == name }) {
            outputs += OutputFormatCapability(name, null, sizes)
        }
    }

    private fun safeSizes(
        warnings: MutableList<String>,
        name: String,
        getter: () -> Array<Size>?
    ): List<String> = try {
        getter().orEmpty().sortedWith(sizeComparator()).map(::sizeString)
    } catch (error: RuntimeException) {
        warnings += "Output sizes for $name could not be read: ${error.message}"
        emptyList()
    }

    private fun sizeComparator() = compareByDescending<Size> { it.width.toLong() * it.height }
        .thenByDescending { it.width }
        .thenByDescending { it.height }

    private fun sizeString(size: Size) = "${size.width}x${size.height}"
    private fun rangeString(range: android.util.Range<*>) = "[${range.lower}, ${range.upper}]"

    private fun utcTimestamp(): String = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply {
        timeZone = TimeZone.getTimeZone("UTC")
    }.format(Date())

    private fun lensFacingName(value: Int?) = when (value) {
        CameraCharacteristics.LENS_FACING_FRONT -> "FRONT"
        CameraCharacteristics.LENS_FACING_BACK -> "BACK"
        CameraCharacteristics.LENS_FACING_EXTERNAL -> "EXTERNAL"
        null -> "UNKNOWN"
        else -> "UNKNOWN($value)"
    }

    private fun hardwareLevelName(value: Int?) = when (value) {
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY -> "LEGACY"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED -> "LIMITED"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL -> "FULL"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_3 -> "LEVEL_3"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL -> "EXTERNAL"
        null -> "UNKNOWN"
        else -> "UNKNOWN($value)"
    }

    private fun afModeName(value: Int) = when (value) {
        0 -> "OFF"
        1 -> "AUTO"
        2 -> "MACRO"
        3 -> "CONTINUOUS_VIDEO"
        4 -> "CONTINUOUS_PICTURE"
        5 -> "EDOF"
        else -> "UNKNOWN($value)"
    }

    private fun awbModeName(value: Int) = when (value) {
        0 -> "OFF"
        1 -> "AUTO"
        2 -> "INCANDESCENT"
        3 -> "FLUORESCENT"
        4 -> "WARM_FLUORESCENT"
        5 -> "DAYLIGHT"
        6 -> "CLOUDY_DAYLIGHT"
        7 -> "TWILIGHT"
        8 -> "SHADE"
        else -> "UNKNOWN($value)"
    }

    private fun opticalStabilizationName(value: Int) = when (value) {
        0 -> "OFF"
        1 -> "ON"
        else -> "UNKNOWN($value)"
    }

    private fun videoStabilizationName(value: Int) = when (value) {
        0 -> "OFF"
        1 -> "ON"
        2 -> "PREVIEW_STABILIZATION"
        else -> "UNKNOWN($value)"
    }

    private fun capabilityName(value: Int) = when (value) {
        0 -> "BACKWARD_COMPATIBLE"
        1 -> "MANUAL_SENSOR"
        2 -> "MANUAL_POST_PROCESSING"
        3 -> "RAW"
        4 -> "PRIVATE_REPROCESSING"
        5 -> "READ_SENSOR_SETTINGS"
        6 -> "BURST_CAPTURE"
        7 -> "YUV_REPROCESSING"
        8 -> "DEPTH_OUTPUT"
        9 -> "CONSTRAINED_HIGH_SPEED_VIDEO"
        10 -> "MOTION_TRACKING"
        11 -> "LOGICAL_MULTI_CAMERA"
        12 -> "MONOCHROME"
        13 -> "SECURE_IMAGE_DATA"
        14 -> "SYSTEM_CAMERA"
        15 -> "OFFLINE_PROCESSING"
        16 -> "ULTRA_HIGH_RESOLUTION_SENSOR"
        17 -> "REMOSAIC_REPROCESSING"
        18 -> "DYNAMIC_RANGE_TEN_BIT"
        19 -> "STREAM_USE_CASE"
        20 -> "COLOR_SPACE_PROFILES"
        else -> "UNKNOWN($value)"
    }

    private fun formatName(value: Int) = when (value) {
        ImageFormat.UNKNOWN -> "UNKNOWN"
        ImageFormat.RGB_565 -> "RGB_565"
        ImageFormat.NV16 -> "NV16"
        ImageFormat.NV21 -> "NV21"
        ImageFormat.YUY2 -> "YUY2"
        ImageFormat.JPEG -> "JPEG"
        ImageFormat.YUV_420_888 -> "YUV_420_888"
        ImageFormat.YUV_422_888 -> "YUV_422_888"
        ImageFormat.YUV_444_888 -> "YUV_444_888"
        ImageFormat.FLEX_RGB_888 -> "FLEX_RGB_888"
        ImageFormat.FLEX_RGBA_8888 -> "FLEX_RGBA_8888"
        ImageFormat.RAW_SENSOR -> "RAW_SENSOR"
        ImageFormat.RAW_PRIVATE -> "RAW_PRIVATE"
        ImageFormat.RAW10 -> "RAW10"
        ImageFormat.RAW12 -> "RAW12"
        ImageFormat.DEPTH16 -> "DEPTH16"
        ImageFormat.DEPTH_POINT_CLOUD -> "DEPTH_POINT_CLOUD"
        ImageFormat.PRIVATE -> "PRIVATE"
        ImageFormat.HEIC -> "HEIC"
        ImageFormat.DEPTH_JPEG -> "DEPTH_JPEG"
        ImageFormat.Y8 -> "Y8"
        else -> "FORMAT_$value"
    }
}
