package com.androidcam.inspector

import android.content.Context
import android.graphics.Color
import android.view.View
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.roundToInt

class CameraControlsView(context: Context) : ScrollView(context) {
    fun interface Listener {
        fun onStateChanged(state: CameraControlState)
    }

    var listener: Listener? = null

    private val content = LinearLayout(context)
    private val exposureAuto = Switch(context)
    private val isoLabel = TextView(context)
    private val isoSeek = SeekBar(context)
    private val shutterLabel = TextView(context)
    private val shutterSeek = SeekBar(context)
    private val focusAuto = Switch(context)
    private val focusLabel = TextView(context)
    private val focusSeek = SeekBar(context)
    private val awbAuto = Switch(context)
    private val redGainLabel = TextView(context)
    private val redGainSeek = SeekBar(context)
    private val blueGainLabel = TextView(context)
    private val blueGainSeek = SeekBar(context)
    private val zoomLabel = TextView(context)
    private val zoomSeek = SeekBar(context)

    private var capabilities: ManualControlCapabilities? = null
    private var state = CameraControlState()
    private var binding = false

    init {
        setBackgroundColor(Color.argb(225, 12, 22, 30))
        isFillViewport = true
        content.orientation = LinearLayout.VERTICAL
        content.setPadding(dp(12), dp(8), dp(12), dp(16))
        addView(content, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))

        content.addView(title(context.getString(R.string.manual_controls_title)))
        content.addView(note(context.getString(R.string.manual_controls_note)))

        content.addView(section(context.getString(R.string.exposure_section)))
        exposureAuto.text = context.getString(R.string.auto_exposure)
        content.addView(exposureAuto)
        content.addView(isoLabel)
        content.addView(isoSeek)
        content.addView(shutterLabel)
        content.addView(shutterSeek)

        content.addView(section(context.getString(R.string.focus_section)))
        focusAuto.text = context.getString(R.string.continuous_autofocus)
        content.addView(focusAuto)
        content.addView(focusLabel)
        content.addView(focusSeek)

        content.addView(section(context.getString(R.string.white_balance_section)))
        awbAuto.text = context.getString(R.string.auto_white_balance)
        content.addView(awbAuto)
        content.addView(note(context.getString(R.string.white_balance_gains_note)))
        content.addView(redGainLabel)
        content.addView(redGainSeek)
        content.addView(blueGainLabel)
        content.addView(blueGainSeek)

        content.addView(section(context.getString(R.string.zoom_section)))
        content.addView(zoomLabel)
        content.addView(zoomSeek)

        listOf(isoSeek, shutterSeek, focusSeek, redGainSeek, blueGainSeek, zoomSeek).forEach {
            it.max = SLIDER_MAX
        }
        attachListeners()
    }

    fun bind(capabilities: ManualControlCapabilities, initialState: CameraControlState) {
        this.capabilities = capabilities
        state = CameraControlPolicy.sanitize(initialState, capabilities)
        binding = true
        exposureAuto.isChecked = state.autoExposure
        exposureAuto.isEnabled = capabilities.manualExposureSupported
        focusAuto.isChecked = state.autoFocus
        focusAuto.isEnabled = capabilities.manualFocusSupported
        awbAuto.isChecked = state.autoWhiteBalance
        awbAuto.isEnabled = capabilities.manualWhiteBalanceSupported

        isoSeek.progress = logValueToProgress(state.iso.toDouble(), capabilities.isoMin.toDouble(), capabilities.isoMax.toDouble())
        shutterSeek.progress = logValueToProgress(
            state.exposureTimeNs.toDouble(),
            capabilities.exposureTimeMinNs.toDouble(),
            capabilities.exposureTimeMaxNs.coerceAtMost(capabilities.maxFrameDurationNs).toDouble()
        )
        focusSeek.progress = linearValueToProgress(state.focusDistanceDiopters, 0f, capabilities.maxFocusDistanceDiopters)
        redGainSeek.progress = linearValueToProgress(state.redGain, capabilities.colorGainMin, capabilities.colorGainMax)
        blueGainSeek.progress = linearValueToProgress(state.blueGain, capabilities.colorGainMin, capabilities.colorGainMax)
        zoomSeek.progress = linearValueToProgress(state.zoomRatio, capabilities.zoomMin, capabilities.zoomMax)
        binding = false
        refreshLabelsAndEnabledState()
    }

    fun appliedState(): CameraControlState = state

    fun showAppliedState(appliedState: CameraControlState) {
        val caps = capabilities ?: return
        bind(caps, appliedState)
    }

    private fun attachListeners() {
        exposureAuto.setOnCheckedChangeListener { _, checked ->
            if (binding) return@setOnCheckedChangeListener
            update(state.copy(autoExposure = checked))
        }
        focusAuto.setOnCheckedChangeListener { _, checked ->
            if (binding) return@setOnCheckedChangeListener
            update(state.copy(autoFocus = checked))
        }
        awbAuto.setOnCheckedChangeListener { _, checked ->
            if (binding) return@setOnCheckedChangeListener
            update(state.copy(autoWhiteBalance = checked))
        }

        isoSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    state = state.copy(
                        iso = logProgressToValue(progress, caps.isoMin.toDouble(), caps.isoMax.toDouble()).roundToInt()
                    )
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
        shutterSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    val maximum = caps.exposureTimeMaxNs.coerceAtMost(caps.maxFrameDurationNs)
                    state = state.copy(
                        exposureTimeNs = logProgressToValue(
                            progress,
                            caps.exposureTimeMinNs.toDouble(),
                            maximum.toDouble()
                        ).toLong()
                    )
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
        focusSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    state = state.copy(
                        focusDistanceDiopters = linearProgressToValue(progress, 0f, caps.maxFocusDistanceDiopters)
                    )
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
        redGainSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    state = state.copy(redGain = linearProgressToValue(progress, caps.colorGainMin, caps.colorGainMax))
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
        blueGainSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    state = state.copy(blueGain = linearProgressToValue(progress, caps.colorGainMin, caps.colorGainMax))
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
        zoomSeek.attach(
            onProgress = { progress ->
                capabilities?.let { caps ->
                    state = state.copy(zoomRatio = linearProgressToValue(progress, caps.zoomMin, caps.zoomMax))
                    refreshLabelsAndEnabledState()
                }
            },
            onCommit = ::commit
        )
    }

    private fun update(newState: CameraControlState) {
        val caps = capabilities ?: return
        state = CameraControlPolicy.sanitize(newState, caps)
        refreshLabelsAndEnabledState()
        listener?.onStateChanged(state)
    }

    private fun commit() {
        val caps = capabilities ?: return
        state = CameraControlPolicy.sanitize(state, caps)
        listener?.onStateChanged(state)
    }

    private fun refreshLabelsAndEnabledState() {
        val caps = capabilities ?: return
        isoLabel.text = context.getString(R.string.iso_value, state.iso, caps.isoMin, caps.isoMax)
        shutterLabel.text = context.getString(
            R.string.shutter_value,
            shutterText(state.exposureTimeNs),
            caps.exposureTimeMinNs,
            caps.exposureTimeMaxNs.coerceAtMost(caps.maxFrameDurationNs)
        )
        focusLabel.text = context.getString(
            R.string.focus_value,
            state.focusDistanceDiopters,
            caps.maxFocusDistanceDiopters
        )
        redGainLabel.text = context.getString(R.string.red_gain_value, state.redGain)
        blueGainLabel.text = context.getString(R.string.blue_gain_value, state.blueGain)
        zoomLabel.text = context.getString(R.string.zoom_value, state.zoomRatio, caps.zoomMin, caps.zoomMax)

        isoSeek.isEnabled = caps.manualExposureSupported && !state.autoExposure
        shutterSeek.isEnabled = caps.manualExposureSupported && !state.autoExposure
        focusSeek.isEnabled = caps.manualFocusSupported && !state.autoFocus
        redGainSeek.isEnabled = caps.manualWhiteBalanceSupported && !state.autoWhiteBalance
        blueGainSeek.isEnabled = caps.manualWhiteBalanceSupported && !state.autoWhiteBalance
        zoomSeek.isEnabled = caps.zoomMax > caps.zoomMin
    }

    private fun shutterText(ns: Long): String {
        val seconds = ns / 1_000_000_000.0
        return if (seconds >= 1.0) {
            context.getString(R.string.seconds_value, seconds)
        } else {
            val denominator = (1.0 / seconds).roundToInt().coerceAtLeast(1)
            context.getString(R.string.shutter_fraction_value, denominator, ns / 1_000_000.0)
        }
    }

    private fun title(text: String) = TextView(context).apply {
        this.text = text
        textSize = 20f
        setTextColor(Color.WHITE)
    }

    private fun section(text: String) = TextView(context).apply {
        this.text = text
        textSize = 17f
        setTextColor(Color.rgb(130, 200, 255))
        setPadding(0, dp(12), 0, dp(4))
    }

    private fun note(text: String) = TextView(context).apply {
        this.text = text
        textSize = 12f
        setTextColor(Color.LTGRAY)
    }

    private fun SeekBar.attach(onProgress: (Int) -> Unit, onCommit: () -> Unit) {
        setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar, progress: Int, fromUser: Boolean) {
                if (fromUser) onProgress(progress)
            }

            override fun onStartTrackingTouch(seekBar: SeekBar) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar) = onCommit()
        })
    }

    private fun logProgressToValue(progress: Int, minimum: Double, maximum: Double): Double {
        if (minimum == maximum) return minimum
        val fraction = progress.toDouble() / SLIDER_MAX
        return exp(ln(minimum) + fraction * (ln(maximum) - ln(minimum)))
    }

    private fun logValueToProgress(value: Double, minimum: Double, maximum: Double): Int {
        if (minimum == maximum) return 0
        return (((ln(value) - ln(minimum)) / (ln(maximum) - ln(minimum))) * SLIDER_MAX)
            .roundToInt().coerceIn(0, SLIDER_MAX)
    }

    private fun linearProgressToValue(progress: Int, minimum: Float, maximum: Float): Float =
        minimum + (maximum - minimum) * progress / SLIDER_MAX

    private fun linearValueToProgress(value: Float, minimum: Float, maximum: Float): Int {
        if (minimum == maximum) return 0
        return (((value - minimum) / (maximum - minimum)) * SLIDER_MAX).roundToInt().coerceIn(0, SLIDER_MAX)
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        private const val SLIDER_MAX = 1_000
    }
}
