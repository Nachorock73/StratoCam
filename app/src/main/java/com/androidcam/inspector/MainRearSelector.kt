package com.androidcam.inspector

import kotlin.math.abs
import kotlin.math.sqrt

object MainRearSelector {
    private const val FULL_FRAME_DIAGONAL_MM = 43.2666153056
    private const val TYPICAL_PHONE_WIDE_EQUIVALENT_MM = 26.0

    fun select(cameras: List<CameraCapability>): MainRearSelection {
        val rear = cameras.filter { it.lensFacing == "BACK" }
        if (rear.isEmpty()) {
            return MainRearSelection(null, "NONE", "No rear-facing camera was reported.", null)
        }

        val candidates = rear.flatMap { camera ->
            val focals = camera.focalLengthsMm.ifEmpty { listOf(Float.NaN) }
            focals.map { focal -> Candidate(camera, focal, equivalentFocal(camera.sensorSizeMm, focal)) }
        }

        val withEquivalent = candidates.filter { it.equivalentMm != null }
        val selected = if (withEquivalent.isNotEmpty()) {
            withEquivalent.minWithOrNull(
                compareBy<Candidate> { abs(it.equivalentMm!! - TYPICAL_PHONE_WIDE_EQUIVALENT_MM) }
                    .thenByDescending { it.camera.physicalCameraIds.isNotEmpty() }
                    .thenBy { it.camera.cameraId }
            )!!
        } else {
            candidates.filter { !it.focalMm.isNaN() }.minWithOrNull(
                compareBy<Candidate> { abs(it.focalMm - 4.5f) }
                    .thenByDescending { it.camera.physicalCameraIds.isNotEmpty() }
                    .thenBy { it.camera.cameraId }
            ) ?: candidates.first()
        }

        val confidence = when {
            selected.equivalentMm != null && selected.equivalentMm in 20.0..35.0 -> "HIGH"
            selected.equivalentMm != null -> "MEDIUM"
            rear.size == 1 -> "MEDIUM"
            else -> "LOW"
        }
        val metric = selected.equivalentMm?.let { "estimated ${"%.1f".format(it)} mm full-frame equivalent" }
            ?: "sensor dimensions unavailable; fallback based on focal length and camera role"
        val logical = if (selected.camera.physicalCameraIds.isNotEmpty()) {
            " It is a logical camera associated with physical IDs ${selected.camera.physicalCameraIds.joinToString()}."
        } else ""

        return MainRearSelection(
            cameraId = selected.camera.cameraId,
            confidence = confidence,
            reason = "Closest rear candidate to a conventional 1x wide camera ($metric).$logical Verify visually before locking this choice for streaming.",
            equivalentFocalLengthMm = selected.equivalentMm
        )
    }

    private fun equivalentFocal(sensor: DimensionF?, focalMm: Float): Double? {
        if (sensor == null || focalMm.isNaN() || focalMm <= 0f) return null
        val diagonal = sqrt(
            sensor.width.toDouble() * sensor.width.toDouble() +
                sensor.height.toDouble() * sensor.height.toDouble()
        )
        if (diagonal <= 0.0) return null
        return focalMm * FULL_FRAME_DIAGONAL_MM / diagonal
    }

    private data class Candidate(
        val camera: CameraCapability,
        val focalMm: Float,
        val equivalentMm: Double?
    )
}
