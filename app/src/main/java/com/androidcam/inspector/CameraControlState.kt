package com.androidcam.inspector

data class ManualControlCapabilities(
    val manualExposureSupported: Boolean,
    val isoMin: Int,
    val isoMax: Int,
    val exposureTimeMinNs: Long,
    val exposureTimeMaxNs: Long,
    val maxFrameDurationNs: Long,
    val manualFocusSupported: Boolean,
    val maxFocusDistanceDiopters: Float,
    val manualWhiteBalanceSupported: Boolean,
    val colorGainMin: Float,
    val colorGainMax: Float,
    val zoomMin: Float,
    val zoomMax: Float,
    val zoomRatioSupported: Boolean,
    val maxAfRegions: Int = 0,
    val activeArrayLeft: Int = 0,
    val activeArrayTop: Int = 0,
    val activeArrayRight: Int = 0,
    val activeArrayBottom: Int = 0
)

data class CameraControlState(
    val autoExposure: Boolean = true,
    val iso: Int = 100,
    val exposureTimeNs: Long = 16_666_667L,
    val autoFocus: Boolean = true,
    val focusDistanceDiopters: Float = 0f,
    val autoWhiteBalance: Boolean = true,
    val redGain: Float = 1f,
    val greenEvenGain: Float = 1f,
    val greenOddGain: Float = 1f,
    val blueGain: Float = 1f,
    val zoomRatio: Float = 1f
)

object CameraControlPolicy {
    fun defaults(capabilities: ManualControlCapabilities): CameraControlState = sanitize(
        CameraControlState(
            iso = 100.coerceIn(capabilities.isoMin, capabilities.isoMax),
            exposureTimeNs = 16_666_667L.coerceIn(
                capabilities.exposureTimeMinNs,
                capabilities.exposureTimeMaxNs
            ),
            zoomRatio = 1f.coerceIn(capabilities.zoomMin, capabilities.zoomMax)
        ),
        capabilities
    )

    fun sanitize(
        state: CameraControlState,
        capabilities: ManualControlCapabilities
    ): CameraControlState = state.copy(
        autoExposure = state.autoExposure || !capabilities.manualExposureSupported,
        iso = state.iso.coerceIn(capabilities.isoMin, capabilities.isoMax),
        exposureTimeNs = state.exposureTimeNs.coerceIn(
            capabilities.exposureTimeMinNs,
            capabilities.exposureTimeMaxNs.coerceAtMost(capabilities.maxFrameDurationNs)
        ),
        autoFocus = state.autoFocus || !capabilities.manualFocusSupported,
        focusDistanceDiopters = state.focusDistanceDiopters.coerceIn(
            0f,
            capabilities.maxFocusDistanceDiopters
        ),
        autoWhiteBalance = state.autoWhiteBalance || !capabilities.manualWhiteBalanceSupported,
        redGain = state.redGain.coerceIn(capabilities.colorGainMin, capabilities.colorGainMax),
        greenEvenGain = state.greenEvenGain.coerceIn(capabilities.colorGainMin, capabilities.colorGainMax),
        greenOddGain = state.greenOddGain.coerceIn(capabilities.colorGainMin, capabilities.colorGainMax),
        blueGain = state.blueGain.coerceIn(capabilities.colorGainMin, capabilities.colorGainMax),
        zoomRatio = state.zoomRatio.coerceIn(capabilities.zoomMin, capabilities.zoomMax)
    )
}
