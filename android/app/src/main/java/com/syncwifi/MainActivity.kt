package com.syncwifi

import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * One-button UI (same philosophy as START_HERE.bat):
 *  - MASTER phone: "START AS MASTER" -> MediaProjection consent ->
 *    broadcasts + plays locally to its own bud.
 *  - CLIENT phone: "START AS CLIENT" -> listens + plays to its bud.
 *  - STOP ends whichever role is running.
 * Status line mirrors the desktop: level/target/ratio/lost.
 *
 * Setup for friends: join the same WiFi hotspot, connect ONE bud each,
 * master plays from YouTube/Chrome (not Netflix: DRM = silence).
 */
class MainActivity : AppCompatActivity() {

    private lateinit var statusView: TextView
    private lateinit var masterBtn: Button
    private lateinit var clientBtn: Button
    private lateinit var stopBtn: Button
    private val handler = Handler(Looper.getMainLooper())
    private val RC_CAPTURE = 1001

    private val statusTick = object : Runnable {
        override fun run() {
            if (ClientService.running) {
                val s = ClientService.status
                val ppm = (s[2] - 1.0) * 1e6
                statusView.text =
                    "CLIENT  level=%.0f target=%.0f ratio=%.6f (%+.0fppm) lost=%.0f".format(
                        s[0], s[1], s[2], ppm, s[3])
            } else if (MasterService.running) {
                statusView.text = "MASTER broadcasting… play from YouTube/Chrome (not Netflix)."
            } else {
                statusView.text = "Idle. Same WiFi, one bud each, then START."
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        statusView = TextView(this).apply { textSize = 15f }
        masterBtn = Button(this).apply { text = "START AS MASTER" }
        clientBtn = Button(this).apply { text = "START AS CLIENT" }
        stopBtn = Button(this).apply { text = "STOP" }
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(masterBtn)
            addView(clientBtn)
            addView(stopBtn)
            addView(statusView)
        }
        setContentView(layout)

        masterBtn.setOnClickListener {
            val pm = getSystemService(MediaProjectionManager::class.java)
            startActivityForResult(pm.createScreenCaptureIntent(), RC_CAPTURE)
        }
        clientBtn.setOnClickListener {
            startForegroundService(Intent(this, ClientService::class.java))
        }
        stopBtn.setOnClickListener {
            stopService(Intent(this, MasterService::class.java))
            stopService(Intent(this, ClientService::class.java))
        }
        handler.post(statusTick)
    }

    @Deprecated("use Activity Result API in later versions")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == RC_CAPTURE && resultCode == RESULT_OK && data != null) {
            val i = Intent(this, MasterService::class.java)
            i.putExtra(MasterService.EXTRA_RESULT_CODE, resultCode)
            i.putExtra(MasterService.EXTRA_RESULT_DATA, data)
            startForegroundService(i)
        }
    }

    override fun onDestroy() {
        handler.removeCallbacks(statusTick)
        super.onDestroy()
    }
}
