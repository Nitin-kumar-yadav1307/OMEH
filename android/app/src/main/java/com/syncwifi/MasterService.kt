package com.syncwifi

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.IBinder
import androidx.core.app.NotificationCompat
import android.net.wifi.WifiManager
import android.net.wifi.p2p.WifiP2pManager
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.concurrent.thread

/**
 * MASTER (your phone): GROUP OWNER of a Wi-Fi Direct P2P group.
 *
 * The Android system assigns the group its own P2P IP (192.168.49.x).
 * The master:
 *   1. Calls WifiP2pManager.createGroup() (we become the Group Owner),
 *      then reads the group's SSID + passphrase from requestGroupInfo()
 *      and encodes them into a "WIFI:" join QR shown by MainActivity.
 *   2. Reads our own P2P IP from requestConnectionInfo() (GO address).
 *   3. Broadcasts audio packets to the P2P subnet broadcast address
 *      192.168.49.255 on port 48200 (replaces the old 255.255.255.255
 *      broadcast that died on isolating hotspots).
 *   4. Listens on control port 48201 (NativeBridge.CTRL_PORT) for client
 *      "SYNC-HELLO" (unicast; replies SYNC-ACK so the client learns it is
 *      registered) and "PING" (link verification; replies PONG before the
 *      client starts its audio stream).
 *
 * DRM note: apps that opt out (allowAudioPlaybackCapture=false) or play
 * hardware-DRM content (Netflix/Prime/Spotify-DRM) arrive as SILENCE.
 * YouTube / Chrome / local players / games capture fine.
 */
class MasterService : Service() {

    companion object {
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        var running = false
            private set
        @Volatile var clientCount = 0
        @Volatile var packetsSent: Long = 0
        @Volatile var captureFrames: Long = 0
        @Volatile var helloCount: Long = 0
        /** Capture-path diagnostics: without these, "client stuck on
         *  audio=waiting" is undebuggable on-device (no ADB). */
        @Volatile var capReads: Long = 0
        @Volatile var capErrors: Long = 0
        /** AudioRecord.state: 1 = STATE_INITIALIZED, 0 = UNINITIALIZED. */
        @Volatile var capRecState: Int = -1
        /** true when the device only supports MONO playback capture (common on
         *  budget Unisoc/MediaTek SoCs) and we duplicate L->R before sending. */
        @Volatile var capMono: Boolean = false
        @Volatile var capUnicast: Long = 0
        @Volatile var deviceIp: String = "-"
        @Volatile var p2pIp: String = "-"
        // Wi-Fi Direct Group Owner always uses the 192.168.49.0/24 subnet,
        // so the broadcast address is known up-front (confirmed later by
        // the group-info callback). Defaulting avoids resolving "-" at
        // runMaster startup before the callback fires.
        @Volatile var p2pBroadcastIp: String = "192.168.49.255"
        /** Wi-Fi Direct group credentials — encoded into the join QR code. */
        @Volatile var p2pSsid: String = "-"
        @Volatile var p2pPassphrase: String = "-"
        @Volatile var qrReady: Boolean = false
        @Volatile var qrPayload: String = ""
    /** Set once requestConnectionInfo confirms we are the Group Owner. */
    @Volatile var goConfirmed = false
    /** createGroup attempts so far (bounded retries). */
    @Volatile var groupAttempts = 0
    private const val MAX_GROUP_ATTEMPTS = 6
        /** True once a MediaProjection is actually capturing audio. */
        @Volatile var capturing: Boolean = false
        /** UDP PING/PONG link verifications received from clients. */
        @Volatile var pingCount: Long = 0
    }

    private val wifiP2p = WifiP2pManagerWrapper(this)
    private var worker: Thread? = null
    private var helloThread: Thread? = null
    @Volatile private var stop = false
    @Volatile private var streamStarted = false
    @Volatile private var projection: MediaProjection? = null
    private var fgStarted = false
    private val clients = java.util.concurrent.ConcurrentHashMap.newKeySet<java.net.InetAddress>()
    private val uiHandler = Handler(Looper.getMainLooper())
    private var groupAttempts = 0
    @Volatile private var retryQueued = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        // startForeground() is deliberately deferred to onStartCommand(): on
        // Android 14 (targetSDK 34) claiming FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        // throws SecurityException unless the user has ALREADY granted
        // screen-capture consent, and that consent token only arrives in the
        // start Intent. Claiming it here — before the consent dialog is even
        // shown — crashed the service at creation time.
        initWifiP2p()
    }

    private fun startForegroundCompat(id: Int, notification: Notification, withProjection: Boolean) {
        if (Build.VERSION.SDK_INT >= 29) {
            val type = if (withProjection)
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            else
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            startForeground(id, notification, type)
        } else {
            startForeground(id, notification)
        }
    }

    private fun initWifiP2p() {
        val wm = getSystemService(WifiP2pManager::class.java)
        if (wm == null) {
            updateNotif("SyncWifi master: WifiP2pManager unavailable")
            return
        }
        // A phone that is JOINED to a router frequently cannot host a Wi-Fi
        // Direct group (no concurrent station+GO on many chipsets). Wi-Fi must
        // be ON but not associated — say so on-device instead of failing with a
        // bare createGroup error code the user cannot interpret.
        val wifi = applicationContext.getSystemService(WifiManager::class.java)
        val ci = try { wifi?.connectionInfo } catch (_: Exception) { null }
        if (ci != null && ci.networkId != -1) {
            val ap = ci.ssid?.trim('"')?.ifEmpty { "a network" } ?: "a network"
            AppLog.w("P2P: master Wi-Fi is CONNECTED to '$ap' — disconnect it (keep Wi-Fi ON)")
            updateNotif("SyncWifi master: disconnect Wi-Fi from '$ap', keep Wi-Fi ON, then restart")
        }
        wifiP2p.initialize(wm)
        if (!wifiP2p.isInitialised()) {
            updateNotif("SyncWifi master: P2P channel init failed")
            return
        }
        // Definitive "am I the Group Owner?" — requestConnectionInfo alone only
        // yields an address, which we previously mistook for success. A master
        // that is not the GO can never serve a joining client.
        wifiP2p.onGroupOwner = { isGo ->
            if (isGo) {
                goConfirmed = true
                AppLog.i("P2P: confirmed Group Owner (192.168.49.1)")
            } else if (!qrReady && groupAttempts < MAX_GROUP_ATTEMPTS) {
                AppLog.w("P2P: formed but NOT the Group Owner — re-creating the group")
                groupAttempts++
                uiHandler.postDelayed({ if (!qrReady) wifiP2p.createGroup() }, 1500)
            }
        }
        // Group Owner: create the P2P group explicitly, then read SSID +
        // passphrase (for the join QR) and our own GO IP.
        wifiP2p.onGroupCreated = {
            AppLog.i("P2P: group created — reading SSID/passphrase")
            wifiP2p.requestGroupInfo()
            wifiP2p.requestConnectionInfo()
        }
        wifiP2p.onMasterIp = { addr ->
            addr?.let {
                p2pIp = it.hostAddress ?: "-"
                AppLog.i("P2P: own GO IP = $p2pIp")
            }
        }
        // Master callbacks: group info (QR + known clients), group formed, errors.
        wifiP2p.onGroupInfo = { g ->
            val ssid = g?.networkName
            val pass = g?.passphrase
            // g.owner.deviceName is the master's own P2P device name — exactly
            // what a scanning client sees in its peer list.
            val ownerName = g?.owner?.deviceName
            if (!ssid.isNullOrEmpty() && !pass.isNullOrEmpty()) {
                p2pSsid = ssid
                p2pPassphrase = pass
                qrPayload = buildWifiQrPayload(ssid, pass, NativeBridge.PORT, ownerName)
                qrReady = true
                AppLog.i("P2P: GO group ready ssid='$ssid' owner='$ownerName' — QR shown")
                updateNotif("SyncWifi master: scan the QR on the client")
            } else if (!qrReady && !retryQueued && groupAttempts < MAX_GROUP_ATTEMPTS) {
                // createGroup can fail transiently (BUSY while a previous
                // group/operation is still settling) and requestGroupInfo may
                // return null before the group exists — retry so the QR
                // eventually appears instead of hanging on "forming group…".
                groupAttempts++
                retryQueued = true
                AppLog.w("P2P: no group info yet (attempt $groupAttempts) — retrying createGroup in 1.5s")
                uiHandler.postDelayed({
                    retryQueued = false
                    if (!qrReady) wifiP2p.createGroup()
                }, 1500)
            }
            val clients = g?.clientList
            if (clients != null && clients.isNotEmpty()) {
                // Broadcast to the P2P subnet (192.168.49.255).
                p2pBroadcastIp = "192.168.49.255"
                updateNotif("SyncWifi master (P2P): group owner ready, ${clients.size} client(s) known")
            }
        }
        wifiP2p.onGroupFormed = { updateNotif("SyncWifi master (P2P): P2P group formed") }
        wifiP2p.onError = { msg ->
            AppLog.w("P2P: $msg")
            updateNotif("SyncWifi master (P2P): $msg")
        }
        wifiP2p.createGroup()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                when (intent.action) {
                    WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION -> {}
                    WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION ->
                        wifiP2p.requestGroupInfo()
                    WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION -> {}
                }
            }
        }
        wifiP2p.registerIntentReceiver(receiver, applicationContext)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Always claim the foreground slot immediately (mediaPlayback type is
        // legal without any consent): on Android 14 the service must call
        // startForeground() within 5s of startForegroundService(), and the
        // P2P-group/QR flow must NOT depend on the media-projection consent
        // arriving — a tokenless start used to stopSelf() before the group
        // even formed, so no QR ever appeared.
        if (!fgStarted) {
            fgStarted = true
            startForegroundCompat(
                1, notif("SyncWifi master: waiting for capture consent…"), false)
        }

        // *** THE bug that made every consent token invisible ***
        // Activity.RESULT_OK is -1 on Android. Using -1 as the "extra
        // missing" sentinel made the two indistinguishable: the user tapped
        // "Start now", MainActivity sent resultCode = RESULT_OK = -1, and
        // the `rc != -1` guard below threw the token away — so
        // getMediaProjection() was NEVER called and recState stayed -1 on
        // every device we tested. RESULT_CANCELED (0) is now the sentinel
        // and RESULT_OK is matched explicitly.
        val rc = intent?.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
            ?: Activity.RESULT_CANCELED
        // Version-safe retrieval: getParcelableExtra(String) is deprecated from
        // API 33 (TIRAMISU) in favour of the typed overload, which also throws
        // instead of silently returning null on a type mismatch.
        val data: Intent? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent?.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent?.getParcelableExtra<Intent>(EXTRA_RESULT_DATA)
        }
        AppLog.d("MasterService: rc=$rc (RESULT_OK=${Activity.RESULT_OK}), " +
                 "dataReceived=${data != null}")
        // Accept a token whenever we have NO working capture: either no
        // projection at all, OR one that never produced audio (capturing==false
        // means runMaster() already exited, e.g. capture unsupported on this
        // device). The old guard was `projection == null`, which made a failed
        // first attempt PERMANENT: projection was non-null, so every later
        // token was ignored and recState stayed at its initial value forever.
        if (rc == Activity.RESULT_OK && data != null && (projection == null || !capturing)) {
            // Android 14 (targetSDK 34) call order:
            //  1. The user has granted consent (extras are present), so claiming
            //     the mediaProjection FGS type in startForeground() is legal now.
            //  2. getMediaProjection() requires that FGS type to be running.
            AppLog.i("Master: consent token received (serviceRunning=$running) — " +
                     "startForeground(mediaProjection) BEFORE getMediaProjection")
            startForegroundCompat(
                1, notif("SyncWifi master (P2P): starting capture…"), true)
            // Release a previous projection so a retry does not leak it.
            projection?.let { old ->
                AppLog.w("Master: replacing a projection that never captured")
                try { old.stop() } catch (_: Throwable) { }
                projection = null
                streamStarted = false
            }
            val mp = getSystemService(MediaProjectionManager::class.java)
                ?.getMediaProjection(rc, data)
            if (mp == null) {
                AppLog.e("Master: getMediaProjection returned null — tap START AS MASTER again")
                updateNotif("SyncWifi master: capture token rejected — tap START AS MASTER again")
                capRecState = 0
                capturing = false
            } else {
                projection = mp
                capturing = true
                mp.registerCallback(object : MediaProjection.Callback() {
                    override fun onStop() {
                        // User stopped sharing from the system status-bar chip.
                        capturing = false
                        // Allow a later consent token to start capture again.
                        streamStarted = false
                        if (!stop) stopSelf()
                    }
                }, Handler(Looper.getMainLooper()))
                AppLog.i("Master: MediaProjection acquired — capture thread starting")
            }
        }

        if (!running) {
            running = true
            stop = false
            clients.clear()
            clientCount = 0
            packetsSent = 0
            captureFrames = 0
            helloCount = 0
            pingCount = 0
            streamStarted = false
            groupAttempts = 0
            refreshNetworkInfo()
            updateNotif("SyncWifi master: forming P2P group / waiting for capture…")
            // Control plane (PING/PONG + SYNC-HELLO) runs even before the
            // projection is granted — the QR can be scanned while the user
            // is still deciding on the capture dialog.
            helloThread = thread(name = "master-hello", isDaemon = true) {
                runHelloListener()
            }
        }
        val proj = projection
        if (!streamStarted && proj != null) {
            streamStarted = true
            updateNotif("SyncWifi master (P2P): capturing + broadcasting on $p2pBroadcastIp")
            worker = thread(name = "master-stream", isDaemon = true) {
                runMaster(proj)
            }
        }
        return START_NOT_STICKY
    }

    private fun refreshNetworkInfo() {
        try {
            val wm = applicationContext.getSystemService(WifiManager::class.java)
            val dhcp = wm?.dhcpInfo
            if (dhcp != null && dhcp.gateway != 0) {
                deviceIp = ipToString(dhcp.ipAddress)
            }
        } catch (_: Exception) { }
    }

    private fun inetToIp(hostOrderInt: Int): String {
        return try {
            val b = byteArrayOf(
                (hostOrderInt and 0xFF).toByte(),
                ((hostOrderInt shr 8) and 0xFF).toByte(),
                ((hostOrderInt shr 16) and 0xFF).toByte(),
                ((hostOrderInt shr 24) and 0xFF).toByte()
            )
            InetAddress.getByAddress(b).hostAddress ?: "-"
        } catch (_: Exception) { "-" }
    }

    private fun ipToString(ip: Int): String {
        if (ip == 0) return "-"
        return "${ip and 0xFF}.${(ip shr 8) and 0xFF}.${(ip shr 16) and 0xFF}.${(ip shr 24) and 0xFF}"
    }

    private fun firstClientAddress(): java.net.InetAddress? = clients.firstOrNull()

    /** Control plane on CTRL_PORT (48201): "PING" -> "PONG" (link
     *  verification) and "SYNC-HELLO" -> "SYNC-ACK" (registration). */
    private fun runHelloListener() {
        try {
            val sock = DatagramSocket(NativeBridge.CTRL_PORT)
            sock.soTimeout = 1000
            val ack = "SYNC-ACK".toByteArray()
            val pong = "PONG".toByteArray()
            val buf = ByteArray(64)
            AppLog.i("CTRL: listening for PING/SYNC-HELLO on udp ${NativeBridge.CTRL_PORT}")
            while (!stop) {
                try {
                    val p = DatagramPacket(buf, buf.size)
                    sock.receive(p)
                    val msg = String(p.data, 0, p.length)
                    when {
                        msg.startsWith("PING") -> {
                            // Link verification (QR flow): the client proves
                            // the P2P route works BEFORE starting audio.
                            pingCount++
                            try {
                                sock.send(DatagramPacket(pong, pong.size, p.address, p.port))
                            } catch (e: Exception) {
                                AppLog.e("UDP: PONG send failed", e)
                            }
                        }
                        msg.startsWith("SYNC-HELLO") -> {
                            helloCount++
                            if (clients.add(p.address)) {
                                clientCount = clients.size
                                updateNotif("SyncWifi master: $clientCount client(s) + broadcasting…")
                            }
                            try {
                                sock.send(DatagramPacket(ack, ack.size, p.address, p.port))
                            } catch (e: Exception) {
                                AppLog.e("UDP: SYNC-ACK send failed", e)
                            }
                        }
                    }
                } catch (_: java.net.SocketTimeoutException) {
                } catch (e: Exception) {
                    if (stop) break
                    AppLog.e("UDP: control-listener error", e)
                }
            }
            sock.close()
        } catch (e: Exception) {
            AppLog.e("UDP: control socket failed", e)
        }
    }

    /** Runs on the master-stream thread; sends audio packets to the
     * P2P subnet broadcast 192.168.49.255 on port 48200.
     */
    /**
     * Build the playback-capture AudioRecord, trying STEREO first and falling
     * back to MONO. Returns (record, isMono); record == null when neither
     * configuration initialises (capture genuinely unsupported on the device).
     *
     * Budget SoCs (Moto E40 / Unisoc T700 and similar) frequently reject
     * CHANNEL_IN_STEREO for AudioPlaybackCaptureConfiguration, which showed up
     * as recState=0 with no explanation.
     */
    private fun buildCapture(mp: MediaProjection): Pair<AudioRecord?, Boolean> {
        fun attempt(stereo: Boolean): AudioRecord = AudioRecord.Builder()
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(48000)
                    .setChannelMask(
                        if (stereo) AudioFormat.CHANNEL_IN_STEREO
                        else AudioFormat.CHANNEL_IN_MONO)
                    .build()
            )
            .setBufferSizeInBytes(1920 * 8)
            .setAudioPlaybackCaptureConfig(
                // MediaProjectionManager.getMediaProjection() — note that
                // getSystemService(MediaProjection::class.java) is NOT a real
                // system service and always returns null.
                AudioPlaybackCaptureConfiguration.Builder(mp)
                    .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
                    .addMatchingUsage(android.media.AudioAttributes.USAGE_UNKNOWN)
                    .build()
            )
            .build()

        val stereoRec = attempt(true)
        if (stereoRec.state == AudioRecord.STATE_INITIALIZED)
            return Pair(stereoRec, false)
        AppLog.w("Capture: 48k STEREO playback capture failed " +
                 "(state=${stereoRec.state}) — retrying with MONO")
        stereoRec.release()

        val monoRec = attempt(false)
        if (monoRec.state == AudioRecord.STATE_INITIALIZED)
            return Pair(monoRec, true)
        AppLog.e("Capture: MONO also failed (state=${monoRec.state})")
        monoRec.release()
        return Pair(null, false)
    }

    private fun runMaster(mp: MediaProjection) {
        val sock = DatagramSocket()
        sock.broadcast = true
        val bcast = try {
            InetAddress.getByName(p2pBroadcastIp)
        } catch (_: Exception) { null }
        val pcm = ShortArray(480 * 2)
        val monoBuf = ShortArray(480)
        var seq = 0
        var pts: Long = 0
        // Several budget devices (Moto E40 / Unisoc T700 and similar) fail to
        // initialise playback capture with CHANNEL_IN_STEREO, so try stereo
        // first and fall back to MONO (duplicated to stereo below). Reporting
        // recState=0 with no fallback made capture look simply "unsupported".
        val cap = buildCapture(mp)
        val rec = cap.first ?: run {
            capRecState = 0
            AppLog.e("Capture: 48k STEREO and MONO playback capture both failed " +
                     "— audio capture is unsupported on this device")
            updateNotif("SyncWifi master: audio capture unsupported on this device")
            return
        }
        capMono = cap.second
        capRecState = rec.state
        AppLog.i("Capture: using ${if (capMono) "MONO (duplicated to stereo)" else "STEREO"} " +
                 "48k playback capture, state=$capRecState")
        // NO local AudioTrack here, on purpose. AudioPlaybackCapture only
        // TAPS the media stream — the music app keeps playing on this
        // phone's own output, so replaying it locally would (a) double
        // every sample (direct + delayed copy = echo/flange) and (b) feed
        // our own USAGE_MEDIA playback straight back into the capture
        // (infinite feedback loop = "looping junk"). The master hears the
        // app directly; we only capture + broadcast.
        capRecState = rec.state
        AppLog.i("Capture: AudioRecord state=$capRecState " +
                 "(1=INITIALIZED, 0=UNINITIALIZED — 0 means capture is dead)")
        rec.startRecording()
        AppLog.i("Master: tap-only mode (no local replay — would double + loop)")
        updateNotif("SyncWifi master (P2P): broadcasting on $p2pBroadcastIp")
        var zeroSince = 0L
        var errStreak = 0
        while (!stop) {
            var got = 0
            if (capMono) {
                // MONO capture: read 480 shorts, then duplicate L->R into the
                // 960-short stereo buffer the wire format expects.
                while (got < 480) {
                    val n = rec.read(monoBuf, got, 480 - got)
                    capReads++
                    if (n < 0) { capErrors++; errStreak++; break }
                    errStreak = 0
                    got += n
                    if (stop) break
                }
                if (got == 480) {
                    for (i in 0 until 480) {
                        pcm[i * 2 + 0] = monoBuf[i]
                        pcm[i * 2 + 1] = monoBuf[i]
                    }
                    got = pcm.size
                }
            } else {
                while (got < pcm.size) {
                    val n = rec.read(pcm, got, pcm.size - got)
                    capReads++
                    if (n < 0) { capErrors++; errStreak++; break }
                    errStreak = 0
                    got += n
                    if (stop) break
                }
            }
            // An uninitialised AudioRecord returns an error INSTANTLY, so
            // without this bail the outer loop spins and pegs the CPU at 100%
            // (battery/heat) while errs climbs into the millions.
            if (errStreak > 50) {
                AppLog.e("Capture: read() keeps failing (errs=$capErrors) — " +
                         "stopping the capture thread instead of spinning")
                updateNotif("SyncWifi master: capture read failed — see in-app log")
                break
            }
            // Watchdog: if capture produces nothing, say so on-device instead
            // of silently leaving every client stuck on "audio=waiting".
            if (captureFrames == 0L) {
                val now = System.currentTimeMillis()
                if (zeroSince == 0L) zeroSince = now
                else if (now - zeroSince > 3000) {
                    AppLog.e("Capture: NO AUDIO after 3s (recState=$capRecState " +
                             "reads=$capReads errors=$capErrors) — did you tap " +
                             "Allow on the capture dialog, and is music playing?")
                    updateNotif("SyncWifi master: no audio — tap Allow + play music")
                    zeroSince = now
                }
            } else {
                zeroSince = 0L
            }
            if (got == pcm.size) {
                captureFrames += 480
                val pkt = try {
                    NativeBridge.masterBuild(seq, pts, pcm)
                } catch (t: Throwable) {
                    AppLog.e("JNI: masterBuild failed", t)
                    null
                }
                if (pkt == null) break
                try {
                    if (clients.isNotEmpty()) {
                        // Unicast-only once clients have registered: the P2P
                        // broadcast delivers the SAME packet twice (broadcast +
                        // unicast). The client's seq dedup absorbs it, but it
                        // doubles receive load and inflates the dup counter.
                        for (c in clients) {
                            sock.send(DatagramPacket(pkt, pkt.size, c, NativeBridge.PORT))
                            capUnicast++
                        }
                    } else if (bcast != null) {
                        // Pre-registration window + hello-loss fallback.
                        sock.send(DatagramPacket(pkt, pkt.size, bcast, NativeBridge.PORT))
                    }
                } catch (e: Exception) {
                    AppLog.e("UDP: audio send failed", e)
                }
                seq++
                pts += 480
                packetsSent = seq.toLong()
            }
        }
        rec.stop()
        rec.release()
        // Clear the "capture is running" state on EVERY exit path. streamStarted
        // was set true before this thread started, so leaving it set made a
        // failed capture permanent (a later consent token would never restart
        // the thread) — and capturing=true with recState=0 made the status line
        // claim audio was flowing when it never was.
        streamStarted = false
        capturing = false
        AppLog.w("Master: capture thread ended (recState=$capRecState " +
                 "captured=$captureFrames errs=$capErrors) — retryable")
        running = false
        stopSelf()
    }

    override fun onDestroy() {
        stop = true
        worker?.join(1000)
        helloThread?.join(1500)
        wifiP2p.cleanup()
        running = false
        capturing = false
        qrReady = false
        qrPayload = ""
        AppLog.i("Master: service destroyed")
        super.onDestroy()
    }

    /** "WIFI:" QR payload (the de-facto standard parsed by Android/iOS
     *  cameras and the Settings QR scanner); the X: key carries the UDP
     *  audio port — foreign scanners ignore it, our client reads it
     *  (control port is derived as port+1 == NativeBridge.CTRL_PORT). */
    private fun buildWifiQrPayload(ssid: String, pass: String, port: Int,
                                   masterName: String?): String {
        fun esc(s: String) = s.replace("\\", "\\\\")
            .replace(";", "\\;").replace(",", "\\,").replace(":", "\\:")
            .replace("\"", "\\\"")
        // The WIFI: keys keep the QR scannable by a phone camera (manual join
        // fallback). The X: keys are ours. syncwifi-master carries the master's
        // P2P DEVICE name — the only string the client can actually match a
        // discovered peer against; the group SSID is unrelated to it.
        val master = if (masterName.isNullOrBlank()) "" else
            ";X:syncwifi-master=${esc(masterName)}"
        return "WIFI:T:WPA;S:${esc(ssid)};P:${esc(pass)}" +
               ";X:syncwifi-port=$port$master;;"
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
