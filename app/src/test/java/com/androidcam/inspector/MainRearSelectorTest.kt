package com.androidcam.inspector

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MainRearSelectorTest {
    @Test
    fun selectsConventionalWideInsteadOfUltraWideAndTelephoto() {
        val cameras = listOf(
            camera("ultra", "BACK", 2.0f, DimensionF(6.4f, 4.8f)),
            camera("wide", "BACK", 4.0f, DimensionF(6.4f, 4.8f)),
            camera("tele", "BACK", 8.0f, DimensionF(6.4f, 4.8f)),
            camera("front", "FRONT", 3.5f, DimensionF(5.6f, 4.2f))
        )

        val result = MainRearSelector.select(cameras)

        assertEquals("wide", result.cameraId)
        assertEquals("HIGH", result.confidence)
        assertTrue(result.equivalentFocalLengthMm!! in 20.0..35.0)
    }

    @Test
    fun reportsNoSelectionWhenThereIsNoRearCamera() {
        val result = MainRearSelector.select(
            listOf(camera("front", "FRONT", 3.0f, DimensionF(5f, 4f)))
        )

        assertEquals(null, result.cameraId)
        assertEquals("NONE", result.confidence)
    }

    @Test
    fun usesFocalFallbackWhenSensorDimensionsAreMissing() {
        val result = MainRearSelector.select(
            listOf(
                camera("small-focal", "BACK", 1.8f, null),
                camera("normal-focal", "BACK", 4.4f, null),
                camera("long-focal", "BACK", 9.0f, null)
            )
        )

        assertEquals("normal-focal", result.cameraId)
        assertEquals("LOW", result.confidence)
    }

    private fun camera(
        id: String,
        facing: String,
        focal: Float,
        sensor: DimensionF?
    ) = CameraCapability(
        cameraId = id,
        lensFacing = facing,
        sensorOrientationDegrees = 90,
        hardwareLevel = "FULL",
        physicalCameraIds = emptyList(),
        focalLengthsMm = listOf(focal),
        sensorSizeMm = sensor,
        activeArray = null,
        outputFormats = emptyList(),
        aeTargetFpsRanges = emptyList(),
        highSpeedModes = emptyList(),
        isoRange = null,
        exposureTimeRangeNs = null,
        autofocusModes = emptyList(),
        minimumFocusDistanceDiopters = null,
        awbModes = emptyList(),
        awbLockAvailable = null,
        aeCompensationRange = null,
        aeCompensationStep = null,
        opticalStabilizationModes = emptyList(),
        videoStabilizationModes = emptyList(),
        maxDigitalZoom = null,
        zoomRatioRange = null,
        supportsControlZoomRatio = false,
        capabilities = emptyList(),
        supportsRaw = false,
        supportsManualSensor = false,
        supportsManualPostProcessing = false,
        inspectionWarnings = emptyList()
    )
}
