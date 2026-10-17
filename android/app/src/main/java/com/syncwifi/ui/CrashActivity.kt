package com.syncwifi.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.syncwifi.SyncWifiApp
import java.io.File

/**
 * Full-screen, on-device crash viewer: complete stack trace + COPY ERROR.
 *
 * Launch sources:
 *  - SyncWifiApp's UncaughtExceptionHandler (trace via EXTRA_TRACE, shown
 *    immediately, in the :crash process so it outlives the crashed one);
 *  - MainActivity's "LAST CRASH" button (falls back to filesDir/
 *    last_crash.txt when no extra is present).
 */
class CrashActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_TRACE = "trace"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val trace = intent.getStringExtra(EXTRA_TRACE)
            ?: run {
                val f = File(filesDir, SyncWifiApp.CRASH_FILE)
                if (f.exists()) f.readText() else "(no crash recorded yet)"
            }

        val traceView = TextView(this).apply {
            text = trace
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.rgb(255, 200, 240))
            setTextIsSelectable(true)
            setPadding(32, 32, 32, 32)
        }

        val copyBtn = Button(this).apply {
            text = "COPY ERROR"
            setOnClickListener {
                getSystemService(ClipboardManager::class.java)
                    ?.setPrimaryClip(ClipData.newPlainText("SyncWifi crash", trace))
                Toast.makeText(this@CrashActivity, "Copied to clipboard", Toast.LENGTH_SHORT).show()
            }
        }
        val closeBtn = Button(this).apply {
            text = "CLOSE"
            setOnClickListener { finish() }
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(25, 10, 55))
            gravity = Gravity.CENTER_HORIZONTAL
            addView(LinearLayout(this@CrashActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(copyBtn, LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(closeBtn, LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT))
            addView(ScrollView(this@CrashActivity).apply {
                addView(traceView)
            }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
    }
}
