package com.androidcam.inspector

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PreviewModeSelectorTest {
    @Test
    fun selectsExact1080pModeWhenAvailable() {
        val result = PreviewModeSelector.selectResolution(
            available = listOf(PreviewMode(3840, 2160), PreviewMode(1920, 1080), PreviewMode(1280, 720)),
            target = PreviewMode(1920, 1080)
        )

        assertEquals(PreviewMode(1920, 1080), result)
    }

    @Test
    fun fallbackPreservesAspectRatio() {
        val result = PreviewModeSelector.selectResolution(
            available = listOf(PreviewMode(1600, 1200), PreviewMode(1280, 720), PreviewMode(640, 480)),
            target = PreviewMode(1920, 1080)
        )

        assertEquals(PreviewMode(1280, 720), result)
    }

    @Test
    fun rejectsFallbackWithWrongAspectRatio() {
        val result = PreviewModeSelector.selectResolution(
            available = listOf(PreviewMode(1600, 1200), PreviewMode(640, 480)),
            target = PreviewMode(1920, 1080)
        )

        assertNull(result)
    }

    @Test
    fun prefersFixedThirtyFpsRange() {
        val result = PreviewModeSelector.selectFps(
            available = listOf(FpsMode(7, 30), FpsMode(30, 30), FpsMode(15, 30)),
            target = 30
        )

        assertEquals(FpsMode(30, 30), result)
    }

    @Test
    fun choosesNarrowestRangeContainingTarget() {
        val result = PreviewModeSelector.selectFps(
            available = listOf(FpsMode(7, 30), FpsMode(15, 30), FpsMode(24, 30)),
            target = 30
        )

        assertEquals(FpsMode(24, 30), result)
    }
}
