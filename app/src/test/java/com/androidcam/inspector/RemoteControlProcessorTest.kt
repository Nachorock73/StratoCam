package com.androidcam.inspector

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RemoteControlProcessorTest {
    private val capabilities = ManualControlCapabilities(
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
    fun appliesManualSettingsWithoutReplacingUnrelatedValues() {
        val initial = CameraControlPolicy.defaults(capabilities).copy(zoomRatio = 2f)
        val result = RemoteControlProcessor.apply(
            RemoteCameraCommand.SetExposure(false, 200, 10_000_000L),
            initial,
            capabilities
        )

        assertFalse(result.state.autoExposure)
        assertEquals(200, result.state.iso)
        assertEquals(10_000_000L, result.state.exposureTimeNs)
        assertEquals(2f, result.state.zoomRatio)
        assertTrue(result.changed)
    }

    @Test
    fun clampsRemoteValuesWithTheCameraPolicy() {
        val result = RemoteControlProcessor.apply(
            RemoteCameraCommand.SetZoom(50f),
            CameraControlPolicy.defaults(capabilities),
            capabilities
        )

        assertEquals(8f, result.state.zoomRatio)
    }

    @Test
    fun resetReturnsAllAutomaticModesAndOneTimesZoom() {
        val manual = CameraControlState(
            autoExposure = false,
            autoFocus = false,
            autoWhiteBalance = false,
            zoomRatio = 4f
        )
        val result = RemoteControlProcessor.apply(RemoteCameraCommand.ResetAuto, manual, capabilities)

        assertTrue(result.state.autoExposure)
        assertTrue(result.state.autoFocus)
        assertTrue(result.state.autoWhiteBalance)
        assertEquals(1f, result.state.zoomRatio)
    }

    @Test
    fun queryDoesNotChangeState() {
        val initial = CameraControlPolicy.defaults(capabilities)
        val result = RemoteControlProcessor.apply(RemoteCameraCommand.GetState, initial, capabilities)

        assertEquals(initial, result.state)
        assertFalse(result.changed)
    }

    @Test
    fun automaticModesTransitionIndependentlyAndRepeatedly() {
        var state = CameraControlPolicy.defaults(capabilities)
        fun apply(command: RemoteCameraCommand) {
            state = RemoteControlProcessor.apply(command, state, capabilities).state
        }

        apply(RemoteCameraCommand.SetFocus(false, 2f))
        assertTrue(state.autoExposure)
        assertFalse(state.autoFocus)
        assertTrue(state.autoWhiteBalance)
        apply(RemoteCameraCommand.SetFocus(true))
        assertTrue(state.autoFocus)

        apply(RemoteCameraCommand.SetExposure(false, 200, 10_000_000L))
        apply(RemoteCameraCommand.SetFocus(false, 3f))
        assertFalse(state.autoExposure)
        assertFalse(state.autoFocus)
        assertTrue(state.autoWhiteBalance)
        apply(RemoteCameraCommand.SetFocus(true))
        assertFalse(state.autoExposure)
        assertTrue(state.autoFocus)
        apply(RemoteCameraCommand.SetExposure(true))

        apply(RemoteCameraCommand.SetFocus(false, 1f))
        apply(RemoteCameraCommand.SetWhiteBalance(false, 2f, 1.8f))
        assertTrue(state.autoExposure)
        assertFalse(state.autoFocus)
        assertFalse(state.autoWhiteBalance)
        apply(RemoteCameraCommand.SetFocus(true))
        apply(RemoteCameraCommand.SetWhiteBalance(true))
        assertTrue(state.autoExposure)
        assertTrue(state.autoFocus)
        assertTrue(state.autoWhiteBalance)

        repeat(10) {
            apply(RemoteCameraCommand.SetExposure(false, 200, 10_000_000L))
            apply(RemoteCameraCommand.SetExposure(true))
            apply(RemoteCameraCommand.SetFocus(false, 2f))
            apply(RemoteCameraCommand.SetFocus(true))
            apply(RemoteCameraCommand.SetWhiteBalance(false, 2f, 1.8f))
            apply(RemoteCameraCommand.SetWhiteBalance(true))
        }
        assertTrue(state.autoExposure)
        assertTrue(state.autoFocus)
        assertTrue(state.autoWhiteBalance)

        apply(RemoteCameraCommand.SetFocus(false, 2f))
        apply(RemoteCameraCommand.ResetAuto)
        assertTrue(state.autoExposure)
        assertTrue(state.autoFocus)
        assertTrue(state.autoWhiteBalance)
    }
}
