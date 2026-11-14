package com.androidcam.inspector

import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.util.Range

data class PreviewMode(val width: Int, val height: Int)

data class FpsMode(val lower: Int, val upper: Int)

data class CameraConfiguration(
    val cameraId: String,
    val previewMode: PreviewMode,
    val fpsRange: Range<Int>,
    val sensorOrientationDegrees: Int,
    val captureTimestampSource: String,
    val enableContinuousVideoAf: Boolean,
    val enableAutoWhiteBalance: Boolean,
    val enableVideoStabilization: Boolean,
    val manualControls: ManualControlCapabilities
)

object PreviewModeSelector {
    fun selectResolution(
        available: List<PreviewMode>,
        target: PreviewMode
    ): PreviewMode? {
        available.firstOrNull { it == target }?.let { return it }
        val targetAspect = target.width.toDouble() / target.height
        return available
            .filter { mode ->
                val aspect = mode.width.toDouble() / mode.height
                kotlin.math.abs(aspect - targetAspect) < 0.01
            }
            .minByOrNull { mode ->
                kotlin.math.abs(mode.width.toLong() * mode.height - target.width.toLong() * target.height)
            }
    }

    fun selectFps(available: List<FpsMode>, target: Int): FpsMode? {
        available.firstOrNull { it.lower == target && it.upper == target }?.let { return it }
        return available
            .filter { target in it.lower..it.upper }
            .minWithOrNull(compareBy<FpsMode> { it.upper - it.lower }.thenByDescending { it.lower })
    }
}

class CameraConfigurationReader(private val cameraManager: CameraManager) {
    fun read(
        cameraId: String,
        targetResolution: PreviewMode,
        targetFps: Int
    ): CameraConfiguration {
        val characteristics = cameraManager.getCameraCharacteristics(cameraId)
        require(characteristics.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK) {
            "cameraId=$cameraId is not reported as a rear-facing camera"
        }

        val streamMap = requireNotNull(
            characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        ) { "cameraId=$cameraId has no stream configuration map" }
        val availableModes = streamMap.getOutputSizes(SurfaceTexture::class.java)
            .orEmpty()
            .map { PreviewMode(it.width, it.height) }
            .distinct()
        val previewMode = requireNotNull(
            PreviewModeSelector.selectResolution(availableModes, targetResolution)
        ) { "No 16:9 preview resolution is available near ${targetResolution.width}x${targetResolution.height}" }

        val availableFps = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            .orEmpty()
            .map { FpsMode(it.lower, it.upper) }
        val fpsMode = requireNotNull(PreviewModeSelector.selectFps(availableFps, targetFps)) {
            "No normal capture FPS range contains $targetFps FPS"
        }

        val afModes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        val awbModes = characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf()
        val stabilizationModes = characteristics
            .get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: intArrayOf()
        val aeModes = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES) ?: intArrayOf()
        val requestCapabilities = characteristics
            .get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
        val isoRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
        val exposureRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
        val maxFrameDuration = characteristics.get(CameraCharacteristics.SENSOR_INFO_MAX_FRAME_DURATION)
            ?: exposureRange?.upper ?: 33_333_333L
        val minimumFocusDistance = characteristics
            .get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        val zoomRatioRange = if (android.os.Build.VERSION.SDK_INT >= 30) {
            characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else null
        val maxDigitalZoom = characteristics.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
        val activeArray = characteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        val maxAfRegions = characteristics.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
        val hasManualSensor = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR in requestCapabilities
        val hasManualPost = CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING in requestCapabilities

        return CameraConfiguration(
            cameraId = cameraId,
            previewMode = previewMode,
            fpsRange = Range(fpsMode.lower, fpsMode.upper),
            sensorOrientationDegrees = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
            captureTimestampSource = when (
                characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE)
            ) {
                CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME -> "REALTIME"
                CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN -> "UNKNOWN"
                else -> "UNAVAILABLE"
            },
            enableContinuousVideoAf = CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes,
            enableAutoWhiteBalance = CaptureRequest.CONTROL_AWB_MODE_AUTO in awbModes,
            enableVideoStabilization = CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON in stabilizationModes,
            manualControls = ManualControlCapabilities(
                manualExposureSupported = hasManualSensor &&
                    CaptureRequest.CONTROL_AE_MODE_OFF in aeModes && isoRange != null && exposureRange != null,
                isoMin = isoRange?.lower ?: 100,
                isoMax = isoRange?.upper ?: 100,
                exposureTimeMinNs = exposureRange?.lower ?: 16_666_667L,
                exposureTimeMaxNs = exposureRange?.upper ?: 16_666_667L,
                maxFrameDurationNs = maxFrameDuration,
                manualFocusSupported = minimumFocusDistance > 0f && CaptureRequest.CONTROL_AF_MODE_OFF in afModes,
                maxFocusDistanceDiopters = minimumFocusDistance,
                manualWhiteBalanceSupported = hasManualPost && CaptureRequest.CONTROL_AWB_MODE_OFF in awbModes,
                colorGainMin = 1f,
                colorGainMax = 3f,
                zoomMin = zoomRatioRange?.lower ?: 1f,
                zoomMax = zoomRatioRange?.upper ?: maxDigitalZoom,
                zoomRatioSupported = zoomRatioRange != null,
                maxAfRegions = maxAfRegions,
                activeArrayLeft = activeArray?.left ?: 0,
                activeArrayTop = activeArray?.top ?: 0,
                activeArrayRight = activeArray?.right ?: 0,
                activeArrayBottom = activeArray?.bottom ?: 0
            )
        )
    }
}
