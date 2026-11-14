package com.androidcam.inspector

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Rect
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureFailure
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.RggbChannelVector
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.util.concurrent.atomic.AtomicBoolean

data class CameraOutputs(
    val previewSurface: Surface,
    val additionalSurfaces: List<Surface> = emptyList(),
    val ownsAdditionalSurfaces: Boolean = false
) {
    val all: List<Surface> get() = listOf(previewSurface) + additionalSurfaces

    fun release() {
        previewSurface.release()
        if (ownsAdditionalSurfaces) additionalSurfaces.forEach(Surface::release)
    }
}

class CameraController(
    context: Context,
    private val listener: Listener
) {
    interface Listener {
        fun onCameraOpened(configuration: CameraConfiguration)
        fun onSessionConfigured(configuration: CameraConfiguration)
        fun onEffectiveFps(fps: Double)
        fun onCaptureStarted(frameNumber: Long, sensorTimestampNs: Long, callbackTimestampNs: Long)
        fun onControlStateApplied(state: CameraControlState)
        fun onFocusPointStateChanged(status: FocusPointStatus)
        fun onCameraClosed(reason: String)
        fun onCameraError(message: String, error: Throwable? = null)
    }

    private val appContext = context.applicationContext
    private val cameraManager = appContext.getSystemService(CameraManager::class.java)
    private val running = AtomicBoolean(false)
    private val stateLock = Any()

    private var workerThread: HandlerThread? = null
    private var workerHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var outputs: CameraOutputs? = null
    private var configuration: CameraConfiguration? = null
    private var requestBuilder: CaptureRequest.Builder? = null
    private var controlState: CameraControlState? = null
    private var lastEffectiveIso: Int? = null
    private var lastEffectiveExposureNs: Long? = null
    private var lastEffectiveFocusDiopters: Float? = null
    private var lastAutoColorGains: RggbChannelVector? = null
    private var lastAutoColorTransform: ColorSpaceTransform? = null
    private var activeFocusRequestId: String? = null
    private var activeFocusRegion: MeteringRectangle? = null
    private var focusCompletionReported = false
    private var opening = false

    private var firstSensorTimestampNs: Long? = null
    private var lastSensorTimestampNs: Long? = null
    private var framesInWindow = 0

    fun startWorker() {
        if (!running.compareAndSet(false, true)) return
        val thread = HandlerThread("Camera2Preview").also { it.start() }
        workerThread = thread
        workerHandler = Handler(thread.looper)
        Log.i(TAG, "Camera worker started")
    }

    fun open(configuration: CameraConfiguration, outputs: CameraOutputs) {
        val handler = workerHandler
        if (!running.get() || handler == null) {
            outputs.release()
            listener.onCameraError("Camera worker is not running")
            return
        }
        if (appContext.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            outputs.release()
            listener.onCameraError("Camera permission is not granted")
            return
        }

        synchronized(stateLock) {
            if (cameraDevice != null || opening) {
                outputs.release()
                Log.w(TAG, "Ignoring duplicate open request")
                return
            }
            this.configuration = configuration
            this.outputs = outputs
            this.controlState = CameraControlPolicy.defaults(configuration.manualControls)
            opening = true
        }

        Log.i(
            TAG,
            "Opening cameraId=${configuration.cameraId}, " +
                "size=${configuration.previewMode.width}x${configuration.previewMode.height}, " +
                "requestedFps=${configuration.fpsRange}, sensorOrientation=${configuration.sensorOrientationDegrees}"
        )
        try {
            cameraManager.openCamera(configuration.cameraId, deviceStateCallback, handler)
        } catch (error: CameraAccessException) {
            failOpen("Camera access failed while opening cameraId=${configuration.cameraId}", error)
        } catch (error: SecurityException) {
            failOpen("Camera permission was rejected while opening cameraId=${configuration.cameraId}", error)
        } catch (error: RuntimeException) {
            failOpen("Unexpected failure while opening cameraId=${configuration.cameraId}", error)
        }
    }

    fun closeCamera(reason: String = "lifecycle pause") {
        val session: CameraCaptureSession?
        val device: CameraDevice?
        val ownedOutputs: CameraOutputs?
        synchronized(stateLock) {
            session = captureSession
            device = cameraDevice
            ownedOutputs = outputs
            captureSession = null
            cameraDevice = null
            outputs = null
            configuration = null
            requestBuilder = null
            controlState = null
            lastEffectiveIso = null
            lastEffectiveExposureNs = null
            lastEffectiveFocusDiopters = null
            lastAutoColorGains = null
            lastAutoColorTransform = null
            activeFocusRequestId = null
            activeFocusRegion = null
            focusCompletionReported = false
            opening = false
            resetFpsWindow()
        }
        try {
            session?.stopRepeating()
        } catch (_: Exception) {
            // The device may already be disconnected.
        }
        session?.close()
        device?.close()
        ownedOutputs?.release()
        if (session != null || device != null || ownedOutputs != null) {
            Log.i(TAG, "Camera closed: $reason")
            listener.onCameraClosed(reason)
        }
    }

    fun stopWorker() {
        if (!running.compareAndSet(true, false)) return
        closeCamera("camera worker stopped")
        val thread = workerThread
        workerThread = null
        workerHandler = null
        thread?.quitSafely()
        try {
            thread?.join(2_000)
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            Log.w(TAG, "Interrupted while stopping camera worker", error)
        }
        Log.i(TAG, "Camera worker stopped")
    }

    fun updateControls(
        requestedState: CameraControlState,
        preserveAutomaticValuesOnManualTransition: Boolean = true
    ) {
        val handler = workerHandler ?: return
        handler.post {
            val currentConfiguration = configuration ?: return@post
            val previous = controlState
            var sanitized = CameraControlPolicy.sanitize(requestedState, currentConfiguration.manualControls)
            if (preserveAutomaticValuesOnManualTransition && previous?.autoExposure == true && !sanitized.autoExposure) {
                sanitized = sanitized.copy(
                    iso = lastEffectiveIso ?: sanitized.iso,
                    exposureTimeNs = lastEffectiveExposureNs ?: sanitized.exposureTimeNs
                )
            }
            if (preserveAutomaticValuesOnManualTransition && previous?.autoFocus == true && !sanitized.autoFocus) {
                sanitized = sanitized.copy(
                    focusDistanceDiopters = lastEffectiveFocusDiopters ?: sanitized.focusDistanceDiopters
                )
            }
            if (preserveAutomaticValuesOnManualTransition && previous?.autoWhiteBalance == true && !sanitized.autoWhiteBalance) {
                lastAutoColorGains?.let { gains ->
                    sanitized = sanitized.copy(
                        redGain = gains.red,
                        greenEvenGain = gains.greenEven,
                        greenOddGain = gains.greenOdd,
                        blueGain = gains.blue
                    )
                }
            }
            sanitized = CameraControlPolicy.sanitize(sanitized, currentConfiguration.manualControls)
            controlState = sanitized
            val builder = requestBuilder ?: return@post
            val session = captureSession ?: return@post
            if (previous?.autoFocus != sanitized.autoFocus) {
                try {
                    builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
                    session.capture(builder.build(), captureCallback, handler)
                } catch (error: Exception) {
                    Log.w(TAG, "Could not cancel the previous AF trigger while changing focus mode", error)
                }
                activeFocusRequestId = null
                activeFocusRegion = null
                focusCompletionReported = false
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
            } else if (!sanitized.autoFocus) {
                activeFocusRequestId = null
                activeFocusRegion = null
                focusCompletionReported = false
            }
            applyControls(builder, sanitized, currentConfiguration)
            try {
                session.setRepeatingRequest(builder.build(), captureCallback, workerHandler)
                Log.i(TAG, "Manual controls applied: ${controlSummary(sanitized)}")
                listener.onControlStateApplied(sanitized)
            } catch (error: CameraAccessException) {
                Log.e(TAG, "Failed to update camera controls", error)
                listener.onCameraError("Failed to update camera controls", error)
            } catch (error: IllegalArgumentException) {
                Log.e(TAG, "The HAL rejected camera controls", error)
                listener.onCameraError("The HAL rejected camera controls", error)
            }
        }
    }

    fun focusAt(requestId: String, sensorX: Int, sensorY: Int) {
        val handler = workerHandler ?: run {
            listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
            return
        }
        handler.post {
            val currentConfiguration = configuration ?: run {
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
                return@post
            }
            val capabilities = currentConfiguration.manualControls
            val state = controlState
            val builder = requestBuilder
            val session = captureSession
            if (state == null || builder == null || session == null) {
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
                return@post
            }
            if (!state.autoFocus || capabilities.maxAfRegions <= 0) {
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "unsupported", sensorX, sensorY))
                return@post
            }
            val active = Rect(
                capabilities.activeArrayLeft,
                capabilities.activeArrayTop,
                capabilities.activeArrayRight,
                capabilities.activeArrayBottom
            )
            if (active.width() <= 0 || active.height() <= 0) {
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "unsupported", sensorX, sensorY))
                return@post
            }
            val regionSize = (minOf(active.width(), active.height()) / 12).coerceAtLeast(80)
            val half = regionSize / 2
            val left = (sensorX - half).coerceIn(active.left, (active.right - regionSize).coerceAtLeast(active.left))
            val top = (sensorY - half).coerceIn(active.top, (active.bottom - regionSize).coerceAtLeast(active.top))
            val region = MeteringRectangle(
                Rect(left, top, (left + regionSize).coerceAtMost(active.right), (top + regionSize).coerceAtMost(active.bottom)),
                800
            )
            try {
                activeFocusRequestId = requestId
                activeFocusRegion = region
                focusCompletionReported = false
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
                builder.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
                session.capture(builder.build(), captureCallback, handler)
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                session.setRepeatingRequest(builder.build(), captureCallback, handler)
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                session.capture(builder.build(), captureCallback, handler)
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                session.setRepeatingRequest(builder.build(), captureCallback, handler)
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "focusing", sensorX, sensorY))
                Log.i(TAG, "Point AF started: request=$requestId region=${region.rect} weight=${region.meteringWeight}")
            } catch (error: CameraAccessException) {
                Log.e(TAG, "Point AF request failed", error)
                activeFocusRequestId = null
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
            } catch (error: IllegalArgumentException) {
                Log.e(TAG, "HAL rejected point AF region", error)
                activeFocusRequestId = null
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
            } catch (error: IllegalStateException) {
                Log.e(TAG, "Point AF request could not be submitted", error)
                activeFocusRequestId = null
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", sensorX, sensorY))
            }
        }
    }

    fun resumeContinuousFocus(requestId: String) {
        val handler = workerHandler ?: return
        handler.post {
            val currentConfiguration = configuration ?: return@post
            val state = controlState ?: return@post
            val builder = requestBuilder ?: return@post
            val session = captureSession ?: return@post
            try {
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
                session.capture(builder.build(), captureCallback, handler)
                activeFocusRequestId = null
                activeFocusRegion = null
                focusCompletionReported = false
                builder.set(CaptureRequest.CONTROL_AF_REGIONS, null)
                applyControls(builder, state, currentConfiguration)
                builder.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                session.setRepeatingRequest(builder.build(), captureCallback, handler)
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "idle", 0, 0))
                Log.i(TAG, "Continuous-video AF resumed")
            } catch (error: Exception) {
                Log.e(TAG, "Could not resume continuous-video AF", error)
                listener.onFocusPointStateChanged(FocusPointStatus(requestId, "failed", 0, 0))
            }
        }
    }

    private val deviceStateCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(device: CameraDevice) {
            val currentConfiguration: CameraConfiguration?
            val currentOutputs: CameraOutputs?
            synchronized(stateLock) {
                opening = false
                if (!running.get()) {
                    device.close()
                    return
                }
                cameraDevice = device
                currentConfiguration = configuration
                currentOutputs = outputs
            }
            if (currentConfiguration == null || currentOutputs == null) {
                device.close()
                listener.onCameraError("Camera opened without a valid configuration")
                return
            }
            Log.i(TAG, "CameraDevice opened: cameraId=${device.id}")
            listener.onCameraOpened(currentConfiguration)
            createSession(device, currentConfiguration, currentOutputs)
        }

        override fun onDisconnected(device: CameraDevice) {
            Log.w(TAG, "CameraDevice disconnected: cameraId=${device.id}")
            closeCamera("camera disconnected")
            listener.onCameraError("Camera ${device.id} was disconnected")
        }

        override fun onError(device: CameraDevice, error: Int) {
            val message = "CameraDevice error for cameraId=${device.id}: ${deviceErrorName(error)}"
            Log.e(TAG, message)
            closeCamera(message)
            listener.onCameraError(message)
        }

        override fun onClosed(device: CameraDevice) {
            Log.i(TAG, "CameraDevice callback closed: cameraId=${device.id}")
        }
    }

    @Suppress("DEPRECATION")
    private fun createSession(
        device: CameraDevice,
        configuration: CameraConfiguration,
        outputs: CameraOutputs
    ) {
        try {
            device.createCaptureSession(
                outputs.all,
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        synchronized(stateLock) {
                            if (!running.get() || cameraDevice !== device) {
                                session.close()
                                return
                            }
                            captureSession = session
                        }
                        startRepeatingPreview(device, session, configuration, outputs)
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        val message = "CaptureSession configuration failed for cameraId=${device.id}"
                        Log.e(TAG, message)
                        session.close()
                        listener.onCameraError(message)
                        closeCamera("session configuration failed")
                    }

                    override fun onClosed(session: CameraCaptureSession) {
                        Log.i(TAG, "CaptureSession closed for cameraId=${device.id}")
                    }
                },
                workerHandler
            )
        } catch (error: CameraAccessException) {
            listener.onCameraError("Could not create CaptureSession", error)
            Log.e(TAG, "Could not create CaptureSession", error)
            closeCamera("session creation error")
        }
    }

    private fun startRepeatingPreview(
        device: CameraDevice,
        session: CameraCaptureSession,
        configuration: CameraConfiguration,
        outputs: CameraOutputs
    ) {
        try {
            val builder = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                outputs.all.forEach(::addTarget)
                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, configuration.fpsRange)
                set(
                    CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                    if (configuration.enableVideoStabilization) {
                        CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON
                    } else {
                        CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF
                    }
                )
            }
            requestBuilder = builder
            val initialState = controlState ?: CameraControlPolicy.defaults(configuration.manualControls)
            applyControls(builder, initialState, configuration)
            session.setRepeatingRequest(builder.build(), captureCallback, workerHandler)
            Log.i(
                TAG,
                "CaptureSession configured: cameraId=${configuration.cameraId}, " +
                    "size=${configuration.previewMode.width}x${configuration.previewMode.height}, " +
                    "fps=${configuration.fpsRange}, videoStabilization=${configuration.enableVideoStabilization}"
            )
            listener.onSessionConfigured(configuration)
            listener.onControlStateApplied(initialState)
        } catch (error: CameraAccessException) {
            Log.e(TAG, "Failed to start repeating preview", error)
            listener.onCameraError("Failed to start repeating preview", error)
            closeCamera("repeating request error")
        } catch (error: IllegalArgumentException) {
            Log.e(TAG, "The device rejected the preview CaptureRequest", error)
            listener.onCameraError("The device rejected the preview CaptureRequest", error)
            closeCamera("invalid capture request")
        }
    }

    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureStarted(
            session: CameraCaptureSession,
            request: CaptureRequest,
            timestamp: Long,
            frameNumber: Long
        ) {
            listener.onCaptureStarted(frameNumber, timestamp, SystemClock.elapsedRealtimeNanos())
        }

        override fun onCaptureCompleted(
            session: CameraCaptureSession,
            request: CaptureRequest,
            result: TotalCaptureResult
        ) {
            lastEffectiveIso = result.get(CaptureResult.SENSOR_SENSITIVITY)
            lastEffectiveExposureNs = result.get(CaptureResult.SENSOR_EXPOSURE_TIME)
            lastEffectiveFocusDiopters = result.get(CaptureResult.LENS_FOCUS_DISTANCE)
            reportPointFocusState(result)
            if (controlState?.autoWhiteBalance != false) {
                result.get(CaptureResult.COLOR_CORRECTION_GAINS)?.let { lastAutoColorGains = it }
                result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { lastAutoColorTransform = it }
            }
            val timestamp = result.get(android.hardware.camera2.CaptureResult.SENSOR_TIMESTAMP) ?: return
            updateEffectiveFps(timestamp, result)
        }

        override fun onCaptureFailed(
            session: CameraCaptureSession,
            request: CaptureRequest,
            failure: CaptureFailure
        ) {
            Log.e(
                TAG,
                "Capture failed: reason=${failure.reason}, frame=${failure.frameNumber}, sequence=${failure.sequenceId}"
            )
        }
    }

    private fun reportPointFocusState(result: TotalCaptureResult) {
        val requestId = activeFocusRequestId ?: return
        if (focusCompletionReported) return
        val afState = result.get(CaptureResult.CONTROL_AF_STATE) ?: return
        val status = when (afState) {
            CaptureResult.CONTROL_AF_STATE_FOCUSED_LOCKED -> "focused"
            CaptureResult.CONTROL_AF_STATE_NOT_FOCUSED_LOCKED -> "failed"
            else -> return
        }
        focusCompletionReported = true
        val rect = activeFocusRegion?.rect
        listener.onFocusPointStateChanged(
            FocusPointStatus(requestId, status, rect?.centerX() ?: 0, rect?.centerY() ?: 0)
        )
        Log.i(TAG, "Point AF completed: request=$requestId status=$status afState=$afState")
    }

    private fun updateEffectiveFps(sensorTimestampNs: Long, result: TotalCaptureResult) {
        if (firstSensorTimestampNs == null) {
            firstSensorTimestampNs = sensorTimestampNs
            framesInWindow = 1
            return
        }
        lastSensorTimestampNs = sensorTimestampNs
        framesInWindow += 1
        if (framesInWindow < FPS_WINDOW_FRAMES) return

        val first = firstSensorTimestampNs ?: return
        val last = lastSensorTimestampNs ?: return
        val elapsed = last - first
        if (elapsed > 0) {
            val fps = (framesInWindow - 1) * 1_000_000_000.0 / elapsed
            Log.i(TAG, "Effective preview FPS: ${"%.2f".format(java.util.Locale.US, fps)}")
            Log.i(TAG, "Effective controls: ${effectiveControlSummary(result)}")
            listener.onEffectiveFps(fps)
        }
        firstSensorTimestampNs = sensorTimestampNs
        lastSensorTimestampNs = null
        framesInWindow = 1
    }

    private fun resetFpsWindow() {
        firstSensorTimestampNs = null
        lastSensorTimestampNs = null
        framesInWindow = 0
    }

    private fun failOpen(message: String, error: Throwable) {
        synchronized(stateLock) {
            opening = false
            outputs?.release()
            outputs = null
            configuration = null
        }
        Log.e(TAG, message, error)
        listener.onCameraError(message, error)
    }

    private fun applyControls(
        builder: CaptureRequest.Builder,
        state: CameraControlState,
        configuration: CameraConfiguration
    ) {
        builder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
        if (state.autoExposure) {
            builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            builder.set(CaptureRequest.SENSOR_SENSITIVITY, null)
            builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, null)
            builder.set(CaptureRequest.SENSOR_FRAME_DURATION, null)
        } else {
            builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
            builder.set(CaptureRequest.SENSOR_SENSITIVITY, state.iso)
            builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, state.exposureTimeNs)
            val nominalFrameDuration = 1_000_000_000L / configuration.fpsRange.upper
            val frameDuration = maxOf(nominalFrameDuration, state.exposureTimeNs)
                .coerceAtMost(configuration.manualControls.maxFrameDurationNs)
            builder.set(CaptureRequest.SENSOR_FRAME_DURATION, frameDuration)
        }

        if (state.autoFocus) {
            val pointRegion = activeFocusRegion
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, pointRegion?.let { arrayOf(it) })
            builder.set(CaptureRequest.CONTROL_AF_MODE, if (pointRegion != null) {
                CaptureRequest.CONTROL_AF_MODE_AUTO
            } else if (configuration.enableContinuousVideoAf) {
                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
            } else {
                CaptureRequest.CONTROL_AF_MODE_AUTO
            })
            builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, null)
        } else {
            builder.set(CaptureRequest.CONTROL_AF_REGIONS, null)
            builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
            builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, state.focusDistanceDiopters)
        }

        if (state.autoWhiteBalance) {
            builder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
            builder.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_FAST)
            builder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null)
            builder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, null)
        } else {
            builder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
            builder.set(
                CaptureRequest.COLOR_CORRECTION_MODE,
                CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX
            )
            builder.set(
                CaptureRequest.COLOR_CORRECTION_GAINS,
                RggbChannelVector(
                    state.redGain,
                    state.greenEvenGain,
                    state.greenOddGain,
                    state.blueGain
                )
            )
            builder.set(
                CaptureRequest.COLOR_CORRECTION_TRANSFORM,
                lastAutoColorTransform ?: IDENTITY_COLOR_TRANSFORM
            )
        }

        if (Build.VERSION.SDK_INT >= 30 && configuration.manualControls.zoomRatioSupported) {
            builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, state.zoomRatio)
        }
    }

    private fun controlSummary(state: CameraControlState): String =
        "AE=${if (state.autoExposure) "AUTO" else "MANUAL ISO=${state.iso} shutterNs=${state.exposureTimeNs}"}, " +
            "AF=${if (state.autoFocus) "CONTINUOUS" else "MANUAL ${state.focusDistanceDiopters}D"}, " +
            "AWB=${if (state.autoWhiteBalance) "AUTO" else "MANUAL R=${state.redGain} B=${state.blueGain}"}, " +
            "zoom=${state.zoomRatio}x"

    private fun effectiveControlSummary(result: TotalCaptureResult): String {
        val gains = result.get(CaptureResult.COLOR_CORRECTION_GAINS)
        val gainText = gains?.let {
            "R=${it.getRed()},Ge=${it.getGreenEven()},Go=${it.getGreenOdd()},B=${it.getBlue()}"
        } ?: "N/A"
        return "AE=${result.get(CaptureResult.CONTROL_AE_MODE)}, " +
            "ISO=${result.get(CaptureResult.SENSOR_SENSITIVITY)}, " +
            "shutterNs=${result.get(CaptureResult.SENSOR_EXPOSURE_TIME)}, " +
            "AF=${result.get(CaptureResult.CONTROL_AF_MODE)}, " +
            "focusD=${result.get(CaptureResult.LENS_FOCUS_DISTANCE)}, " +
            "AWB=${result.get(CaptureResult.CONTROL_AWB_MODE)}, gains=[$gainText], " +
            "zoom=${if (Build.VERSION.SDK_INT >= 30) result.get(CaptureResult.CONTROL_ZOOM_RATIO) else null}"
    }

    private fun deviceErrorName(error: Int): String = when (error) {
        CameraDevice.StateCallback.ERROR_CAMERA_IN_USE -> "CAMERA_IN_USE"
        CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE -> "MAX_CAMERAS_IN_USE"
        CameraDevice.StateCallback.ERROR_CAMERA_DISABLED -> "CAMERA_DISABLED"
        CameraDevice.StateCallback.ERROR_CAMERA_DEVICE -> "CAMERA_DEVICE"
        CameraDevice.StateCallback.ERROR_CAMERA_SERVICE -> "CAMERA_SERVICE"
        else -> "UNKNOWN($error)"
    }

    companion object {
        private const val TAG = "CameraController"
        private const val FPS_WINDOW_FRAMES = 60
        private val IDENTITY_COLOR_TRANSFORM = ColorSpaceTransform(
            intArrayOf(
                1, 1, 0, 1, 0, 1,
                0, 1, 1, 1, 0, 1,
                0, 1, 0, 1, 1, 1
            )
        )
    }
}
