package com.syncwifi

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.IBinder
import androidx.core.app.NotificationCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.concurrent.thread

/**
 * MASTER (your phone): captures system audio via AudioPlaybackCapture
 * (MediaProjection consent), broadcasts UDP with absolute master_pts,
 * and plays locally to its own bud through NORMAL Android routing
 * (master is the clock, no warp needed).
 *
 * DRM note: apps that opt out (allowAudioPlaybackCapture=false) or play
 * hardware-DRM content (Netflix/Prime/Spotify-DRM) arrive as SILENCE.
 * YouTube / Chrome / local players / games capture fine. Documented
 * limit, not a bug: play shared audio from an allowed app.
 */
class MasterService : Service() {

    companion object {
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        var running = false
            private set
    }

    private var worker: Thread? = null
    @Volatile private var stop = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForeground(1, notif("SyncWifi master starting…"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (running) return START_STICKY
        val rc = intent?.getIntExtra(EXTRA_RESULT_CODE, -1) ?: -1
        val data: Intent? = intent?.getParcelableExtra(EXTRA_RESULT_DATA)
        if (rc == -1 || data == null) { stopSelf(); return START_NOT_STICKY }
        running = true
        stop = false
        worker = thread(name = "master-stream", isDaemon = true) {
            runMaster(rc, data)
        }
        return START_STICKY
    }

    private fun runMaster(resultCode: Int, resultData: Intent) {
        val pm = getSystemService(MediaProjectionManager::class.java)
        val proj: MediaProjection? = pm.getMediaProjection(resultCode, resultData)
        if (proj == null) { stopSelf(); return }

        // Capture the device's media output (excludes VOICE_CALL etc.).
        val capCfg = AudioPlaybackCaptureConfiguration.Builder(proj)
            .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
            .addMatchingUsage(android.media.AudioAttributes.USAGE_UNKNOWN)
            .build()
        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(48000)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        // 480 frames * 4 bytes = 1920B per 10ms; keep buffer a multiple.
        val rec = AudioRecord.Builder()
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(1920 * 8)
            .setAudioPlaybackCaptureConfig(capCfg)
            .build()
        val sock = DatagramSocket()
        sock.broadcast = true
        val bcast = InetAddress.getByName("255.255.255.255")
        val pcm = ShortArray(480 * 2)
        var seq = 0
        var pts: Long = 0
        rec.startRecording()
        updateNotif("SyncWifi master: broadcasting…")
        while (!stop) {
            var got = 0
            while (got < pcm.size) {
                val n = rec.read(pcm, got, pcm.size - got)
                if (n <= 0) break
                got += n
                if (stop) break
            }
            if (got == pcm.size) {
                val pkt = NativeBridge.masterBuild(seq, pts, pcm)
                sock.send(DatagramPacket(pkt, pkt.size, bcast, NativeBridge.PORT))
                seq++
                pts += 480
            }
        }
        rec.stop()
        rec.release()
        proj.stop()
        sock.close()
        running = false
        stopSelf()
    }

    override fun onDestroy() {
        stop = true
        worker?.join(1000)
        running = false
        super.onDestroy()
    }

    private fun notif(text: String): Notification {
        val ch = "syncwifi"
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(ch, "SyncWifi", NotificationManager.IMPORTANCE_LOW))
        return NotificationCompat.Builder(this, ch)
            .setContentTitle("SyncWifi").setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play).build()
    }

    private fun updateNotif(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(1, notif(text))
    }
}
