package com.androidcam.inspector

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class MainActivity : Activity(), CameraController.Listener {
    private lateinit var previewView: AutoFitTextureView
    private lateinit var statusView: TextView
    private lateinit var controlsView: CameraControlsView
    private lateinit var controller: CameraController
    private lateinit var controlServer: CameraControlServer
    private lateinit var videoServer: VideoStreamServer
    private lateinit var deviceStatusReader: DeviceStatusReader
    private var videoEncoder: H264VideoEncoder? = null
    private var configuration: CameraConfiguration? = null
    private var desiredControlState: CameraControlState? = null
    @Volatile private var latestFocusStatus: FocusPointStatus? = null
    private var resumed = false
    private var videoMode = VIDEO_MODE_1080P

    private val surfaceListener = object : TextureView.SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
            Log.i(TAG, "Preview SurfaceTexture available: ${width}x$height")
            openCameraIfReady()
        }

        override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) {
            configuration?.let { configureTransform(width, height, it) }
        }

        override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
            Log.i(TAG, "Preview SurfaceTexture destroyed")
            controller.closeCamera("preview surface destroyed")
            return true
        }

        override fun onSurfaceTextureUpdated(surface: SurfaceTexture) = Unit
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        controller = CameraController(this, this)
        deviceStatusReader = DeviceStatusReader(applicationContext)
        setContentView(createContentView())
        controlServer = CameraControlServer(::handleRemoteRequest)
        controlServer.start()
        videoServer = VideoStreamServer(VIDEO_STREAM_INFO) {
            videoEncoder?.requestKeyFrame()
        }
        videoServer.start()
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        startVideoEncoder()
        controller.startWorker()
        if (previewView.isAvailable) openCameraIfReady()
    }

    override fun onPause() {
        resumed = false
        controller.stopWorker()
        stopVideoEncoder()
        super.onPause()
    }

    override fun onDestroy() {
        controlServer.stop()
        videoServer.stop()
        super.onDestroy()
    }

    private fun handleRemoteRequest(request: RemoteRequest): RemoteResponse {
        var response: RemoteResponse? = null
        val completed = CountDownLatch(1)
        runOnUiThread {
            response = processRemoteRequest(request)
            completed.countDown()
        }
        if (!completed.await(REMOTE_COMMAND_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
            return RemoteResponse(request.id, false, "Timed out waiting for camera UI thread")
        }
        return response ?: RemoteResponse(request.id, false, "No response from camera UI thread")
    }

    private fun processRemoteRequest(request: RemoteRequest): RemoteResponse {
        when (val command = request.command) {
            RemoteCameraCommand.GetDeviceStatus ->
                return RemoteResponse(request.id, true, deviceStatus = deviceStatusReader.read())
            is RemoteCameraCommand.SetVideoMode -> return setVideoMode(request.id, command)
            else -> Unit
        }
        val capabilities = configuration?.manualControls
            ?: return RemoteResponse(request.id, false, "Camera preview is not ready")
        val current = desiredControlState ?: CameraControlPolicy.defaults(capabilities)
        when (val command = request.command) {
            is RemoteCameraCommand.FocusAt -> {
                if (!current.autoFocus) return RemoteResponse(request.id, false, "Click-to-focus requires Auto Focus")
                if (capabilities.maxAfRegions <= 0) return RemoteResponse(request.id, false, "Camera reports CONTROL_MAX_REGIONS_AF=0")
                if (command.sensorX !in capabilities.activeArrayLeft until capabilities.activeArrayRight ||
                    command.sensorY !in capabilities.activeArrayTop until capabilities.activeArrayBottom) {
                    return RemoteResponse(request.id, false, "Focus point is outside SENSOR_INFO_ACTIVE_ARRAY_SIZE")
                }
                val focus = FocusPointStatus(request.id, "focusing", command.sensorX, command.sensorY)
                latestFocusStatus = focus
                controller.focusAt(request.id, command.sensorX, command.sensorY)
                return RemoteResponse(request.id, true, state = current, focusStatus = focus)
            }
            RemoteCameraCommand.GetFocusStatus ->
                return RemoteResponse(request.id, true, state = current, focusStatus = latestFocusStatus)
            RemoteCameraCommand.ResumeContinuousFocus -> {
                val focus = FocusPointStatus(request.id, "idle", 0, 0)
                latestFocusStatus = focus
                controller.resumeContinuousFocus(request.id)
                return RemoteResponse(request.id, true, state = current, focusStatus = focus)
            }
            else -> Unit
        }
        val result = RemoteControlProcessor.apply(request.command, current, capabilities)
        if (result.changed) {
            desiredControlState = result.state
            controlsView.showAppliedState(result.state)
            controller.updateControls(
                result.state,
                preserveAutomaticValuesOnManualTransition = false
            )
            Log.i(TAG, "Remote controls requested: ${result.state}")
        }
        if (request.command == RemoteCameraCommand.ResetAuto) controller.resumeContinuousFocus(request.id)
        return when (request.command) {
            RemoteCameraCommand.Ping -> RemoteResponse(request.id, true, "pong")
            RemoteCameraCommand.GetCapabilities -> RemoteResponse(
                request.id,
                true,
                state = result.state,
                capabilities = capabilities
            )
            else -> RemoteResponse(request.id, true, state = result.state)
        }
    }

    private fun setVideoMode(id: String, command: RemoteCameraCommand.SetVideoMode): RemoteResponse {
        val requested = VideoMode(command.width, command.height, command.fps, command.bitrate)
        val validResolution = (requested.width == 1920 && requested.height == 1080) ||
            (requested.width == 3840 && requested.height == 2160)
        if (!validResolution || requested.fps != 30 || requested.bitrate !in 8_000_000..40_000_000) {
            return RemoteResponse(id, false, "Unsupported video mode")
        }
        if (requested == videoMode) return RemoteResponse(id, true, "Video mode already active")
        Log.i(TAG, "Switching video mode: $videoMode -> $requested")
        controller.closeCamera("video mode switch")
        stopVideoEncoder()
        configuration = null
        videoMode = requested
        videoServer.updateStreamInfo(requested.streamInfo(configured = false))
        if (resumed) {
            startVideoEncoder()
            previewView.postDelayed(::openCameraIfReady, 150)
        }
        return RemoteResponse(id, true, "Video mode switching to ${requested.width}x${requested.height}@${requested.fps}")
    }

    private fun createContentView(): View {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        // Keep the video edge-to-edge, but place every interactive overlay inside safe insets.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
        val overlay = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        overlay.setOnApplyWindowInsetsListener { view, insets ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val safe = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(safe.left, safe.top, safe.right, safe.bottom)
            } else {
                @Suppress("DEPRECATION")
                view.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                    insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        previewView = AutoFitTextureView(this).apply {
            surfaceTextureListener = surfaceListener
        }
        root.addView(
            previewView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER
            )
        )

        statusView = TextView(this).apply {
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.argb(170, 10, 18, 24))
            textSize = 14f
            maxLines = 3
            ellipsize = android.text.TextUtils.TruncateAt.END
            setPadding(dp(12), dp(10), dp(12), dp(10))
            text = getString(R.string.preview_waiting)
        }
        overlay.addView(
            statusView,
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        )

        controlsView = CameraControlsView(this).apply {
            visibility = View.GONE
            listener = CameraControlsView.Listener { state ->
                desiredControlState = state
                controller.updateControls(state)
            }
        }
        // The scrollable controls get only the space left above the measured action bar.
        // This also handles landscape, larger text, cutouts and three-button navigation.
        val controlsArea = object : FrameLayout(this) {
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                controlsView.layoutParams.height = minOf(dp(500), View.MeasureSpec.getSize(heightMeasureSpec))
                super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            }
        }
        controlsArea.addView(
            controlsView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                dp(500),
                Gravity.BOTTOM
            )
        )
        overlay.addView(controlsArea, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.argb(150, 10, 18, 24))
            setPadding(dp(8), dp(4), dp(8), dp(4))
        }
        actions.addView(Button(this).apply {
            text = getString(R.string.retry_preview)
            setOnClickListener {
                controller.closeCamera("manual retry")
                previewView.postDelayed(::openCameraIfReady, 250)
            }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(Button(this).apply {
            text = getString(R.string.show_controls)
            setOnClickListener {
                controlsView.visibility = if (controlsView.visibility == View.VISIBLE) View.GONE else View.VISIBLE
                text = getString(
                    if (controlsView.visibility == View.VISIBLE) R.string.hide_controls else R.string.show_controls
                )
            }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(Button(this).apply {
            text = getString(R.string.show_capabilities)
            setOnClickListener {
                startActivity(Intent(this@MainActivity, CapabilityReportActivity::class.java))
            }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        overlay.addView(
            actions,
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        )
        root.addView(overlay, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        overlay.requestApplyInsets()
        return root
    }

    private fun openCameraIfReady() {
        if (!resumed || !previewView.isAvailable) return
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            statusView.text = getString(R.string.preview_permission_required)
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA_PERMISSION)
            return
        }

        try {
            val cameraManager = getSystemService(CameraManager::class.java)
            val selected = CameraConfigurationReader(cameraManager).read(
                cameraId = REAR_CAMERA_CANDIDATE_ID,
                // Keep the on-device preview at PREVIEW size; the MediaCodec surface carries the selected 4K/1080 mode.
                targetResolution = PreviewMode(1920, 1080),
                targetFps = videoMode.fps
            )
            configuration = selected
            videoServer.updateStreamInfo(
                videoMode.streamInfo(configured = true).copy(
                    sensorOrientationDegrees = selected.sensorOrientationDegrees,
                    captureTimestampSource = selected.captureTimestampSource,
                    configured = true
                )
            )
            val selectedState = desiredControlState?.let {
                CameraControlPolicy.sanitize(it, selected.manualControls)
            } ?: CameraControlPolicy.defaults(selected.manualControls)
            desiredControlState = selectedState
            controlsView.bind(selected.manualControls, selectedState)
            val texture = requireNotNull(previewView.surfaceTexture)
            texture.setDefaultBufferSize(selected.previewMode.width, selected.previewMode.height)
            applyAspectRatio(selected)
            configureTransform(previewView.width, previewView.height, selected)
            statusView.text = getString(
                R.string.preview_opening,
                selected.cameraId,
                selected.previewMode.width,
                selected.previewMode.height,
                selected.fpsRange.lower,
                selected.fpsRange.upper
            )
            val encoderSurfaces = videoEncoder?.let { listOf(it.inputSurface) }.orEmpty()
            controller.open(selected, CameraOutputs(Surface(texture), encoderSurfaces))
        } catch (error: Exception) {
            Log.e(TAG, "Preview configuration failed", error)
            statusView.text = getString(R.string.preview_error, error.message ?: error.javaClass.simpleName)
        }
    }

    private fun applyAspectRatio(configuration: CameraConfiguration) {
        when (currentDisplayRotation()) {
            Surface.ROTATION_90, Surface.ROTATION_270 -> previewView.setAspectRatio(
                configuration.previewMode.width,
                configuration.previewMode.height
            )
            else -> previewView.setAspectRatio(
                configuration.previewMode.height,
                configuration.previewMode.width
            )
        }
    }

    private fun configureTransform(viewWidth: Int, viewHeight: Int, configuration: CameraConfiguration) {
        if (viewWidth == 0 || viewHeight == 0) return
        val rotation = currentDisplayRotation()
        val matrix = Matrix()
        val viewRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
        val centerX = viewRect.centerX()
        val centerY = viewRect.centerY()

        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            val bufferRect = RectF(
                0f,
                0f,
                configuration.previewMode.height.toFloat(),
                configuration.previewMode.width.toFloat()
            )
            bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
            matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
            val scale = maxOf(
                viewHeight.toFloat() / configuration.previewMode.height,
                viewWidth.toFloat() / configuration.previewMode.width
            )
            matrix.postScale(scale, scale, centerX, centerY)
            matrix.postRotate(90f * (rotation - 2), centerX, centerY)
        } else if (rotation == Surface.ROTATION_180) {
            matrix.postRotate(180f, centerX, centerY)
        }
        previewView.setTransform(matrix)
        Log.i(
            TAG,
            "Preview transform applied: displayRotation=$rotation, " +
                "sensorOrientation=${configuration.sensorOrientationDegrees}, view=${viewWidth}x$viewHeight"
        )
    }

    @Suppress("DEPRECATION")
    private fun currentDisplayRotation(): Int = if (Build.VERSION.SDK_INT >= 30) {
        display?.rotation ?: Surface.ROTATION_0
    } else {
        windowManager.defaultDisplay.rotation
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_CAMERA_PERMISSION) return
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) openCameraIfReady()
        else statusView.text = getString(R.string.preview_permission_denied)
    }

    override fun onCameraOpened(configuration: CameraConfiguration) = runOnUiThread {
        statusView.text = getString(R.string.preview_creating_session, configuration.cameraId)
    }

    override fun onSessionConfigured(configuration: CameraConfiguration) = runOnUiThread {
        statusView.text = getString(
            R.string.preview_running,
            configuration.cameraId,
            configuration.previewMode.width,
            configuration.previewMode.height,
            configuration.fpsRange.upper,
            if (configuration.enableVideoStabilization) getString(R.string.enabled) else getString(R.string.disabled)
        )
        desiredControlState?.let { controller.updateControls(it) }
    }

    override fun onEffectiveFps(fps: Double) = runOnUiThread {
        val selected = configuration ?: return@runOnUiThread
        statusView.text = getString(
            R.string.preview_running_effective,
            selected.cameraId,
            selected.previewMode.width,
            selected.previewMode.height,
            String.format(Locale.US, "%.1f", fps),
            if (selected.enableVideoStabilization) getString(R.string.enabled) else getString(R.string.disabled)
        )
    }

    override fun onCaptureStarted(frameNumber: Long, sensorTimestampNs: Long, callbackTimestampNs: Long) {
        videoEncoder?.recordCaptureStarted(
            frameNumber,
            sensorTimestampNs,
            callbackTimestampNs,
            configuration?.captureTimestampSource ?: "UNKNOWN"
        )
    }

    override fun onControlStateApplied(state: CameraControlState) = runOnUiThread {
        desiredControlState = state
        controlsView.showAppliedState(state)
    }

    override fun onFocusPointStateChanged(status: FocusPointStatus) {
        latestFocusStatus = status
        Log.i(TAG, "Point focus state: request=${status.requestId}, status=${status.status}, sensor=${status.sensorX},${status.sensorY}")
    }

    override fun onCameraClosed(reason: String) {
        Log.i(TAG, "Camera closed callback: $reason")
    }

    override fun onCameraError(message: String, error: Throwable?) = runOnUiThread {
        Log.e(TAG, message, error)
        statusView.text = getString(R.string.preview_error, message)
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun startVideoEncoder() {
        if (videoEncoder != null) return
        try {
            videoEncoder = H264VideoEncoder(
                configuration = videoMode.encoderConfiguration(),
                packetSink = videoServer::publish,
                errorListener = { message, error ->
                    Log.e(TAG, message, error)
                }
            ).also { it.start() }
        } catch (error: Exception) {
            videoEncoder = null
            Log.e(TAG, "Could not start USB video encoder; preview will continue without streaming", error)
        }
    }

    private fun stopVideoEncoder() {
        videoEncoder?.stop()
        videoEncoder = null
    }

    companion object {
        private const val TAG = "PreviewActivity"
        private const val REQUEST_CAMERA_PERMISSION = 200
        private const val REAR_CAMERA_CANDIDATE_ID = "0"
        private const val REMOTE_COMMAND_TIMEOUT_SECONDS = 2L
        private val VIDEO_MODE_1080P = VideoMode(1920, 1080, 30, 8_000_000)
        private val VIDEO_STREAM_INFO = VIDEO_MODE_1080P.streamInfo(configured = false)
    }
}

private data class VideoMode(val width: Int, val height: Int, val fps: Int, val bitrate: Int) {
    fun encoderConfiguration() = VideoEncoderConfiguration(width, height, fps, bitrate, 1)
    fun streamInfo(configured: Boolean) = VideoStreamInfo(
        codec = "video/avc",
        width = width,
        height = height,
        fps = fps,
        bitrate = bitrate,
        sensorOrientationDegrees = 0,
        configured = configured
    )
}
