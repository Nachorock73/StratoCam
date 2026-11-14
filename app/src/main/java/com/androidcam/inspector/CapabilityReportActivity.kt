package com.androidcam.inspector

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.util.concurrent.Executors

class CapabilityReportActivity : Activity() {
    private val executor = Executors.newSingleThreadExecutor()
    private lateinit var reportView: TextView
    private lateinit var inspectButton: Button
    private lateinit var saveButton: Button
    private lateinit var copyButton: Button
    private var reportText: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = getString(R.string.app_name)
        setContentView(createContentView())
        beginInspectionWithPermission()
    }

    private fun createContentView(): View {
        val padding = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding, padding, padding)
            setBackgroundColor(Color.rgb(245, 247, 250))
        }

        root.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 24f
            setTextColor(Color.rgb(16, 24, 32))
        })
        root.addView(TextView(this).apply {
            text = getString(R.string.description)
            textSize = 14f
            setPadding(0, dp(4), 0, dp(12))
            setTextColor(Color.DKGRAY)
        })

        val actions = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        inspectButton = Button(this).apply {
            text = getString(R.string.inspect_again)
            setOnClickListener { beginInspectionWithPermission() }
        }
        copyButton = Button(this).apply {
            text = getString(R.string.copy)
            isEnabled = false
            setOnClickListener { copyReport() }
        }
        saveButton = Button(this).apply {
            text = getString(R.string.save_txt)
            isEnabled = false
            setOnClickListener { selectReportDestination() }
        }
        actions.addView(inspectButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(copyButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        actions.addView(saveButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(actions)

        reportView = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 12f
            setTextColor(Color.rgb(20, 28, 36))
            setTextIsSelectable(true)
            text = getString(R.string.waiting_permission)
            setPadding(0, dp(12), 0, dp(20))
        }
        root.addView(ScrollView(this).apply {
            addView(reportView)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        return root
    }

    private fun beginInspectionWithPermission() {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            inspectCameras()
        } else {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA_PERMISSION)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_CAMERA_PERMISSION) {
            if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) inspectCameras()
            else {
                reportView.text = getString(R.string.permission_denied)
                inspectButton.isEnabled = true
            }
        }
    }

    private fun inspectCameras() {
        inspectButton.isEnabled = false
        saveButton.isEnabled = false
        copyButton.isEnabled = false
        reportView.text = getString(R.string.inspecting)
        executor.execute {
            val result = try {
                CameraReportFormatter.format(CameraCapabilityInspector(this).inspect())
            } catch (error: Exception) {
                getString(R.string.inspection_failed, error.javaClass.simpleName, error.message)
            }
            runOnUiThread {
                reportText = result
                reportView.text = result
                inspectButton.isEnabled = true
                saveButton.isEnabled = true
                copyButton.isEnabled = true
            }
        }
    }

    private fun copyReport() {
        val text = reportText ?: return
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText(getString(R.string.report_clip_label), text))
        Toast.makeText(this, R.string.report_copied, Toast.LENGTH_SHORT).show()
    }

    private fun selectReportDestination() {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "text/plain"
            putExtra(Intent.EXTRA_TITLE, "camera2-capabilities.txt")
        }
        startActivityForResult(intent, REQUEST_SAVE_REPORT)
    }

    @Deprecated("Uses the platform Activity API to avoid an AndroidX runtime dependency.")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_SAVE_REPORT || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            contentResolver.openOutputStream(uri, "wt")?.bufferedWriter(Charsets.UTF_8).use { writer ->
                requireNotNull(writer) { "The selected destination could not be opened." }
                writer.write(reportText.orEmpty())
            }
            Toast.makeText(this, R.string.report_saved, Toast.LENGTH_SHORT).show()
        } catch (error: Exception) {
            Toast.makeText(this, getString(R.string.save_failed, error.message), Toast.LENGTH_LONG).show()
        }
    }

    override fun onDestroy() {
        executor.shutdownNow()
        super.onDestroy()
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        private const val REQUEST_CAMERA_PERMISSION = 100
        private const val REQUEST_SAVE_REPORT = 101
    }
}
