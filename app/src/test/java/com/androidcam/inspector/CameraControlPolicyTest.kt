package com.androidcam.inspector

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CameraControlPolicyTest {
    private val supported = ManualControlCapabilities(
        manualExposureSupported = true,
        isoMin = 89,
        isoMax = 9_973,
        exposureTimeMinNs = 14_961L,
        exposureTimeMaxNs = 32_000_000_000L,
        maxFrameDurationNs = 32_000_000_000L,
        manualFocusSupported = true,
        maxFocusDistanceDiopters = 10f,
        manualWhiteBalanceSupported = true,
        colorGainMin = 1f,
        colorGainMax = 3f,
        zoomMin = 1f,
        zoomMax = 8f,
        zoomRatioSupported = true
    )

    @Test
    fun defaultsUseSafeVideoValues() {
        val state = CameraControlPolicy.defaults(supported)

        assertTrue(state.autoExposure)
        assertTrue(state.autoFocus)
        assertTrue(state.autoWhiteBalance)
        assertEquals(100, state.iso)
        assertEquals(16_666_667L, state.exposureTimeNs)
        assertEquals(1f, state.zoomRatio)
    }

    @Test
    fun clampsEveryManualValueToSupportedRange() {
        val state = CameraControlPolicy.sanitize(
            CameraControlState(
                autoExposure = false,
                iso = 50_000,
                exposureTimeNs = Long.MAX_VALUE,
                autoFocus = false,
                focusDistanceDiopters = 50f,
                autoWhiteBalance = false,
                redGain = 0.2f,
                blueGain = 8f,
                zoomRatio = 20f
            ),
            supported
        )

        assertFalse(state.autoExposure)
        assertEquals(9_973, state.iso)
        assertEquals(32_000_000_000L, state.exposureTimeNs)
        assertEquals(10f, state.focusDistanceDiopters)
        assertEquals(1f, state.redGain)
        assertEquals(3f, state.blueGain)
        assertEquals(8f, state.zoomRatio)
    }

    @Test
    fun unavailableManualModesAreForcedBackToAuto() {
        val unavailable = supported.copy(
            manualExposureSupported = false,
            manualFocusSupported = false,
            manualWhiteBalanceSupported = false
        )

        val state = CameraControlPolicy.sanitize(
            CameraControlState(autoExposure = false, autoFocus = false, autoWhiteBalance = false),
            unavailable
        )

        assertTrue(state.autoExposure)
        assertTrue(state.autoFocus)
        assertTrue(state.autoWhiteBalance)
    }

    @Test
    fun exposureCannotExceedMaximumFrameDuration() {
        val limitedFrameDuration = supported.copy(maxFrameDurationNs = 1_000_000_000L)

        val state = CameraControlPolicy.sanitize(
            CameraControlState(autoExposure = false, exposureTimeNs = 5_000_000_000L),
            limitedFrameDuration
        )

        assertEquals(1_000_000_000L, state.exposureTimeNs)
    }
}
