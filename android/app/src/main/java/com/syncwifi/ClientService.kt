package com.syncwifi

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.IBinder
import androidx.core.app.NotificationCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import kotlin.concurrent.thread

/**
 * CLIENT (friend's phone): receives UDP, feeds the NATIVE ring
 * (sync_wifi.h, same drift PI as desktop), renders via AudioTrack
 * in low-latency mode to its ONE bud.
 *
 * Render path: AudioTrack streams MODE_STREAM, 10ms writes (480f).
 * Per-write warping happens in native clientRender(); the small
 * residual fractional phase stays inside the native engine, exactly
 * like sync_client.cpp. Status polls native clientStatus().
 */
class ClientService : Service() {

    companion object {
        var running = false
            private set
        // Latest status for MainActivity: level/target/ratio/lost.
        @Volatile var status = doubleArrayOf(0.0, 1920.0, 1.0, 0.0)
    }

    private var netThread: Thread? = null
    private var playThread: Thread? = null
    @Volatile private var stop = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForeground(2, notif("SyncWifi client starting…"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (running) return START_STICKY
        running = true
        stop = false
        NativeBridge.clientReset()
        netThread = thread(name = "client-net", isDaemon = true) { runNet() }
        playThread = thread(name = "client-play", isDaemon = true) { runPlay() }
        return START_STICKY
    }

    private fun runNet() {
        val sock = DatagramSocket(NativeBridge.PORT)
        sock.soTimeout = 1000
        val buf = ByteArray(1936) // SYNC_WIFI_PACKET_BYTES
        while (!stop) {
            try {
                val p = DatagramPacket(buf, buf.size)
                sock.receive(p)
                if (p.length == 1936)
                    NativeBridge.clientFeed(p.data, p.length)
            } catch (_: java.net.SocketTimeoutException) {
            } catch (_: Exception) {
                if (stop) break
            }
        }
        sock.close()
    }

    private fun runPlay() {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(48000)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        val track = AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(1920 * 8)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track.play()
        updateNotif("SyncWifi client: playing in sync…")
        val out = ShortArray(480 * 2)
        var ticks = 0
        while (!stop) {
            val n = NativeBridge.clientRender(out, 480)
            if (n > 0) {
                track.write(out, 0, n * 2)
            } else {
                // Not primed yet (or gap): play silence, retry next quantum.
                track.write(ShortArray(480 * 2), 0, 480 * 2)
                try { Thread.sleep(10) } catch (_: InterruptedException) { break }
            }
            if (++ticks % 200 == 0) status = NativeBridge.clientStatus()
        }
        track.stop()
        track.release()
        running = false
        stopSelf()
    }

    override fun onDestroy() {
        stop = true
        netThread?.join(1500)
        playThread?.join(1500)
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
        nm.notify(2, notif(text))
    }
}
