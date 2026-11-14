package com.androidcam.inspector

import java.util.Locale

object CameraReportFormatter {
    fun format(report: CameraReport): String = buildString {
        appendLine("CAMERA2 CAPABILITY INSPECTOR REPORT")
        appendLine("===================================")
        appendLine("Generated (UTC): ${report.generatedAtUtc}")
        appendLine("Device: ${report.device.manufacturer} ${report.device.model}")
        appendLine("Android: ${report.device.androidVersion} (API ${report.device.sdkInt})")
        appendLine("Device codename: ${report.device.device}")
        appendLine("Camera IDs found: ${report.cameras.size}")
        appendLine()

        appendLine("MAIN REAR 1x CANDIDATE")
        appendLine("----------------------")
        appendLine("cameraId: ${report.mainRearSelection.cameraId ?: "NOT IDENTIFIED"}")
        appendLine("Confidence: ${report.mainRearSelection.confidence}")
        appendLine("Estimated equivalent focal length: ${report.mainRearSelection.equivalentFocalLengthMm?.decimal(1)?.plus(" mm") ?: "N/A"}")
        appendLine("Reason: ${report.mainRearSelection.reason}")
        appendLine()

        report.cameras.forEachIndexed { index, camera ->
            appendLine("CAMERA ${index + 1}: cameraId=${camera.cameraId}")
            appendLine("----------------------------------------")
            field("Facing", camera.lensFacing)
            field("Sensor orientation", camera.sensorOrientationDegrees?.let { "$it degrees" })
            field("Camera2 hardware level", camera.hardwareLevel)
            field("Associated physical camera IDs", camera.physicalCameraIds.listOrUnavailable())
            field("Focal lengths", camera.focalLengthsMm.takeIf { it.isNotEmpty() }
                ?.joinToString { "${it.decimal(3)} mm" })
            field("Sensor physical size", camera.sensorSizeMm?.let {
                "${it.width.decimal(3)} x ${it.height.decimal(3)} mm"
            })
            field("Sensor active array", camera.activeArray)
            field("Camera capabilities", camera.capabilities.listOrUnavailable())
            field("RAW capability", camera.supportsRaw.yesNo())
            field("MANUAL_SENSOR capability", camera.supportsManualSensor.yesNo())
            field("MANUAL_POST_PROCESSING capability", camera.supportsManualPostProcessing.yesNo())
            appendLine()

            appendLine("Exposure")
            field("ISO range", camera.isoRange)
            field("SENSOR_EXPOSURE_TIME range", camera.exposureTimeRangeNs?.let { "$it ns" })
            field("AE target FPS ranges", camera.aeTargetFpsRanges.listOrUnavailable())
            field("EV compensation range", camera.aeCompensationRange)
            field("EV compensation step", camera.aeCompensationStep)
            appendLine()

            appendLine("Focus and lens")
            field("Autofocus modes", camera.autofocusModes.listOrUnavailable())
            field("Minimum focus distance", camera.minimumFocusDistanceDiopters?.let {
                if (it == 0f) "0 diopters (fixed-focus or no manual focusing)" else "${it.decimal(4)} diopters"
            })
            field("Maximum digital zoom", camera.maxDigitalZoom?.let { "${it.decimal(3)}x" })
            field("CONTROL_ZOOM_RATIO supported", camera.supportsControlZoomRatio.yesNo())
            field("CONTROL_ZOOM_RATIO range", camera.zoomRatioRange)
            appendLine()

            appendLine("White balance")
            field("AWB modes", camera.awbModes.listOrUnavailable())
            field("AWB lock available", camera.awbLockAvailable?.yesNo())
            appendLine()

            appendLine("Stabilization")
            field("Optical stabilization modes", camera.opticalStabilizationModes.listOrUnavailable())
            field("Video stabilization modes", camera.videoStabilizationModes.listOrUnavailable())
            appendLine()

            appendLine("Output formats and resolutions")
            if (camera.outputFormats.isEmpty()) {
                appendLine("  NOT AVAILABLE")
            } else {
                camera.outputFormats.forEach { output ->
                    val numeric = output.value?.let { " ($it)" }.orEmpty()
                    appendLine("  ${output.name}$numeric:")
                    appendWrapped(output.sizes)
                }
            }
            appendLine()

            appendLine("Constrained high-speed modes")
            if (camera.highSpeedModes.isEmpty()) {
                appendLine("  NOT AVAILABLE")
            } else {
                camera.highSpeedModes.forEach { mode ->
                    appendLine("  ${mode.size}: ${mode.fpsRanges.joinToString().ifEmpty { "FPS UNKNOWN" }}")
                }
            }

            if (camera.inspectionWarnings.isNotEmpty()) {
                appendLine()
                appendLine("Inspection warnings")
                camera.inspectionWarnings.forEach { appendLine("  - $it") }
            }
            appendLine()
        }

        appendLine("GLOBAL WARNINGS")
        appendLine("---------------")
        if (report.warnings.isEmpty()) appendLine("None")
        else report.warnings.forEach { appendLine("- $it") }
        appendLine()
        appendLine("IMPORTANT")
        appendLine("---------")
        appendLine("The 1x selection is a metadata-based candidate, not an assumption that cameraId 0 is the main lens.")
        appendLine("Validate the proposed camera visually before using it in the streaming milestones.")
        appendLine("Only values reported here should be offered as controls; vendors may still reject some declared stream combinations at runtime.")
    }

    private fun StringBuilder.field(name: String, value: String?) {
        appendLine("$name: ${value ?: "NOT AVAILABLE"}")
    }

    private fun StringBuilder.appendWrapped(values: List<String>) {
        if (values.isEmpty()) {
            appendLine("    NOT AVAILABLE")
            return
        }
        values.chunked(8).forEach { appendLine("    ${it.joinToString()}") }
    }

    private fun List<String>.listOrUnavailable(): String? = takeIf { it.isNotEmpty() }?.joinToString()
    private fun Boolean.yesNo() = if (this) "YES" else "NO"
    private fun Float.decimal(digits: Int) = String.format(Locale.US, "%.${digits}f", this)
    private fun Double.decimal(digits: Int) = String.format(Locale.US, "%.${digits}f", this)
}
