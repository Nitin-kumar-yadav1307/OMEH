package com.syncwifi

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.core.app.NotificationCompat
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import kotlin.concurrent.thread

/**
 * CLIENT (friend's phone): joins the master's Wi-Fi Direct group and plays
 * the synchronized audio on the bud.
 *
 * QR flow ("SCAN TO JOIN", primary):
 *   1. MainActivity scanned the master's QR -> SSID/passphrase/audio port
 *      sit in pendingSsid/pendingPass/pendingPort.
 *   2. WifiNetworkSpecifier + ConnectivityManager.requestNetwork() joins the
 *      group's WPA2 network automatically (no Settings page). NET_CAPABILITY_
 *      INTERNET is removed because P2P groups have none; the request lives
 *      until unregisterNetworkCallback() in onDestroy(), which tears the
 *      link down again (the system then offers to forget the network).
 *   3. Link verification: UDP PING -> 192.168.49.1:(port+1), master replies
 *      PONG -> linkVerified=true (green in MainActivity) and only then do
 *      the hello/net/play worker threads start.
 *   4. SYNC-HELLO registers us with the master (SYNC-ACK); 1936-byte audio
 *      datagrams feed the sync_wifi JNI engine and are rendered by the
 *      AudioTrack in sync with the master.
 *
 * Legacy fallback (no QR): discoverPeers() -> connect() to a peer ->
 * requestConnectionInfo() -> the same PING/PONG verification + audio.
 *
 * Bug fixed here: netThread/playThread/helloThread were previously declared
 * and joined but NEVER started — the client never received or played audio.
 */
class ClientService : Service() {

    companion object {
        /** The Wi-Fi Direct Group Owner always lives at 192.168.49.1. */
        const val GO_IP = "192.168.49.1"
        /** Cap on joinPeer() attempts before giving up (rescan to retry). */
        private const val MAX_JOIN_ATTEMPTS = 12
        var running = false
            private set
        /** [level, targetMs, ratio, lost, dup, stalls] — see NativeBridge.clientStatus6().
         *  MUST stay 6 elements: the UI indexes it every tick, and a short
         *  default array crashes before the first native poll arrives. */
        @Volatile var status = doubleArrayOf(0.0, 150.0, 1.0, 0.0, 0.0, 0.0)
        /** On-device pipeline health counters (audio-echo/dup debugging):
         *  recvPkts  = 1936-byte audio datagrams actually received
         *  dupPkts   = duplicates the JNI engine skipped (seq <= last)
         *  stalls    = render underruns (ring empty) since start          */
        @Volatile var recvPkts: Long = 0
        @Volatile var dupPkts: Long = 0
        @Volatile var stalls: Long = 0
        @Volatile var helloSent: Long = 0
        @Volatile var helloTargets: String = "-"
        @Volatile var connected: Boolean = false
        @Volatile var lastSeenMs: Long = 0
        @Volatile var masterIp: String = "-"
        /** QR credentials handed over by MainActivity before start. */
        @Volatile var pendingSsid: String? = null
        @Volatile var pendingPass: String? = null
        /** The master's P2P DEVICE name from the QR — what we match discovered
         *  peers against (the group SSID is a different string). */
        @Volatile var pendingMasterName: String? = null
        @Volatile var pendingPort: Int = NativeBridge.PORT
        /** UDP PING/PONG result — MainActivity turns green on true. */
        @Volatile var linkVerified: Boolean = false
        @Volatile var verifiedMs: Long = 0
        @Volatile var pingCount: Long = 0
        @Volatile var joinMode: String = "-"
    }

    private val wifiP2p = WifiP2pManagerWrapper(this)
    private var netThread: Thread? = null
    private var playThread: Thread? = null
    private var helloThread: Thread? = null
    private var verifyThread: Thread? = null
    @Volatile private var stop = false
    private var started = false
    @Volatile private var verifiedOnce = false
    private var discoveredPeers = mutableListOf<WifiP2pDevice>()
    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    /** Guards initWifiP2p(): the receiver + channel must only be set up once,
     *  because the join watchdog may call it after the QR path stalled. */
    private var p2pStarted = false
    /** Join throttle state - see pickAndConnect().
     *  connectedToGo is the ONLY thing that stops further join attempts, and it
     *  is set from real connection state (a formed group), never from "we
     *  called connect()". Latching on the attempt flag left the client with
     *  zero retries whenever connect() succeeded silently - which is exactly
     *  how pairing regressed to "neither device connects". */
    private var connectedToGo = false
    private var joinAttempts = 0
    private var lastJoinAttemptMs = 0L
    /** P2P Network from the specifier request; UDP sockets bind to it. */
    @Volatile private var boundNetwork: Network? = null

    /** QR port is the audio port; the control plane (PING/HELLO) sits at +1
     *  (== NativeBridge.CTRL_PORT; the master builds its QR from PORT). */
    private val audioPort: Int get() = pendingPort
    private val ctrlPort: Int get() = pendingPort + 1

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        // Safe on Android 14: mediaPlayback FGS needs no runtime consent
        // (unlike MasterService's mediaProjection type).
        startForegroundCompat(2, notif("SyncWifi client (P2P) starting…"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (!started) {
            started = true
            running = true
            stop = false
            connectedToGo = false
            joinAttempts = 0
            lastJoinAttemptMs = 0
            linkVerified = false
            verifiedOnce = false
            connected = false
            val ssid = pendingSsid
            // ONE join path for every case: discover peers, then connect to the
            // master BY MAC. WifiNetworkSpecifier is deliberately NOT used: it
            // fails silently when the group SSID contains spaces (Android names
            // groups "DIRECT-xx-<model>" and OEMs embed the model), and Android
            // 15+ will not let an app name its own group to dodge that. Peer
            // discovery is immune to the SSID's contents, works on every API
            // level, and needs no credentials.
            if (ssid.isNullOrBlank() && pendingMasterName.isNullOrBlank()) {
                AppLog.w("Client: no QR data — will adopt any discovered master")
            }
            joinMode = "MAC"
            updateNotif("SyncWifi client: looking for the master…")
            AppLog.i("Client: target master='${pendingMasterName ?: "(any)"}' " +
                     "audio=$audioPort ctrl=$ctrlPort")
            initWifiP2p()
        }
        return START_NOT_STICKY
    }

    /**
     * QR path: join the master's P2P group with WifiNetworkSpecifier. The GO's
     * SSID is an ordinary WPA2 network, so the standard Wi-Fi stack joins it
     * for us — no Settings page, no manual IP. (Some Android versions show a
     * small "connect to network" confirmation sheet first; one tap and the
     * join proceeds.)
     */
    

    /** UDP PING -> master; PONG proves the link before the audio starts. */
    private fun runVerify() {
        val target = masterIp.takeIf { it.isNotBlank() && it != "-" } ?: GO_IP
        for (attempt in 1..15) {
            if (stop) return
            var sock: DatagramSocket? = null
            try {
                sock = DatagramSocket()
                boundNetwork?.bindSocket(sock) // pin UDP to the P2P network
                sock.soTimeout = 1000
                val master = InetAddress.getByName(target)
                val ping = "PING".toByteArray()
                sock.send(DatagramPacket(ping, ping.size, master, ctrlPort))
                pingCount++
                val buf = ByteArray(64)
                val p = DatagramPacket(buf, buf.size)
                sock.receive(p)
                val msg = String(p.data, 0, p.length)
                if (msg.startsWith("PONG")) {
                    linkVerified = true
                    verifiedMs = System.currentTimeMillis()
                    masterIp = target
                    AppLog.i("Client: PONG from ${p.address.hostAddress} — link VERIFIED (attempt $attempt)")
                    updateNotif("SyncWifi client: Connected & Verified — starting audio…")
                    onVerified()
                    return
                }
                AppLog.w("Client: verify attempt $attempt got '$msg'")
            } catch (t: Throwable) {
                AppLog.e("Client: verify attempt $attempt failed", t)
            } finally {
                try { sock?.close() } catch (_: Throwable) { }
            }
            try { Thread.sleep(700) } catch (_: InterruptedException) { return }
        }
        updateNotif("SyncWifi client: no PONG from master — is the master running?")
    }

    /** Called exactly once (first PONG): start hello + audio workers. */
    private fun onVerified() {
        if (verifiedOnce) return
        verifiedOnce = true
        try {
            NativeBridge.clientReset()
        } catch (t: Throwable) {
            AppLog.e("JNI: clientReset failed", t)
        }
        helloThread = thread(name = "client-hello", isDaemon = true) { runHello() }
        netThread = thread(name = "client-net", isDaemon = true) { runNet() }
        playThread = thread(name = "client-play", isDaemon = true) { runPlay() }
        AppLog.i("Client: audio workers started")
    }

    /** Unicast SYNC-HELLO to the master; it ACKs and adds us to its list. */
    private fun runHello() {
        try {
            val sock = DatagramSocket()
            boundNetwork?.bindSocket(sock)
            sock.soTimeout = 1000
            val msg = "SYNC-HELLO".toByteArray()
            helloTargets = masterIp
            while (!stop) {
                try {
                    val a = InetAddress.getByName(masterIp)
                    sock.send(DatagramPacket(msg, msg.size, a, ctrlPort))
                    helloSent++
                    try { Thread.sleep(2000) } catch (_: InterruptedException) { break }
                } catch (t: Throwable) {
                    // After STOP the WifiNetworkSpecifier network is gone, so
                    // send() throws ENETUNREACH forever. That is teardown
                    // noise, not a fault — only log it while we are live.
                    if (!stop) AppLog.e("UDP: hello send failed", t)
                    try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
                }
            }
            sock.close()
        } catch (t: Throwable) {
            AppLog.e("UDP: hello socket failed", t)
        }
    }

    /** LEGACY fallback (no QR): discover P2P peers and join the master. */
    private fun initWifiP2p() {
        val wm = getSystemService(WifiP2pManager::class.java)
        if (wm == null) {
            AppLog.e("Client: WifiP2pManager unavailable")
            updateNotif("SyncWifi client: WifiP2pManager unavailable")
            return
        }
        wifiP2p.initialize(wm)
        if (!wifiP2p.isInitialised()) {
            AppLog.e("Client: P2P channel init failed")
            updateNotif("SyncWifi client: P2P channel init failed")
            return
        }
        p2pStarted = true
        joinMode = "MAC"
        wifiP2p.discoverPeers()
        wifiP2p.onPeersAvailable = { peers ->
            if (peers.isNotEmpty()) {
                discoveredPeers.clear()
                discoveredPeers.addAll(peers)
                if (discoveredPeers.size > 16)
                    discoveredPeers = discoveredPeers.take(16).toMutableList()
                pickAndConnect()
            }
        }
        wifiP2p.onMasterIp = { addr ->
            connectedToGo = true     // group really formed — stop retrying
            addr?.let { inet ->
                masterIp = inet.hostAddress ?: "-"
                AppLog.i("Client: P2P GO address = $masterIp — verifying link")
                updateNotif("SyncWifi client (P2P): master IP = $masterIp")
                // Bind the UDP sockets to the P2P interface. Without this the
                // packets can leave via another network (or none), which is why
                // PING timed out even with a correct master IP.
                boundNetwork = findP2pNetwork()
                verifyThread = thread(name = "client-verify", isDaemon = true) { runVerify() }
            }
        }
        wifiP2p.onError = { msg ->
            // A failed connect() frees us to retry (throttled); anything else
            // is informational.
            if (msg.startsWith("connect failed"))
                AppLog.w("Client: connect rejected — retrying in 3s")
            AppLog.w("Client P2P: $msg")
            updateNotif("SyncWifi client (P2P): $msg")
        }
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                when (intent.action) {
                    WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION ->
                        wifiP2p.requestPeers()
                    WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION ->
                        wifiP2p.requestConnectionInfo()
                    WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION -> {}
                }
            }
        }
        wifiP2p.registerIntentReceiver(receiver, applicationContext)
    }

    /** Pick the best discovered peer and join it BY MAC ADDRESS.
     *
     *  Matching the scanned SSID against each peer's deviceName makes the join
     *  immune to the SSID's contents (spaces, unusual characters) — unlike
     *  WifiNetworkSpecifier, which silently fails to associate with a spaced
     *  SSID. deviceAddress is a MAC, so there is no subnet/IP reasoning here. */
    private fun pickAndConnect() {
        if (discoveredPeers.isEmpty()) return
        // THROTTLE. PEERS_CHANGED fires continuously while scanning, and an
        // unthrottled joinPeer() hammers connect() hundreds of times — the
        // framework then rejects every later call with ERROR(0) and the join
        // never completes. So: at most one attempt every 3 s.
        //
        // Retrying is UNBOUNDED on purpose. We stop only when a group has
        // actually formed (connectedToGo, set from WifiP2pInfo.groupFormed),
        // because connect() can succeed without ever reporting an address, and
        // any attempt-based latch then kills all further retries.
        if (connectedToGo) return
        val now = System.currentTimeMillis()
        if (now - lastJoinAttemptMs < 3000) return

        val want = pendingSsid
        // The peer advertises its DEVICE name ("Sahej's F17"); the QR carries
        // the group SSID ("DIRECT-7w-Galaxy F17 5G"). Those differ, so match
        // loosely on either side containing the other, else take any peer that
        // is available (in practice there is exactly one master in range).
        val byName: WifiP2pDevice? = if (!want.isNullOrBlank()) {
            val needle = want.lowercase()
            discoveredPeers.firstOrNull {
                val n = it.deviceName?.lowercase() ?: return@firstOrNull false
                n == needle || n.contains(needle) || needle.contains(n)
            }
        } else null
        val candidate = byName
            ?: discoveredPeers.firstOrNull { it.status == WifiP2pDevice.AVAILABLE }
            ?: discoveredPeers.firstOrNull()
            ?: return

        lastJoinAttemptMs = now
        joinAttempts++
        AppLog.i("Client: MAC join '${candidate.deviceName}' -> ${candidate.deviceAddress}" +
                 " (attempt $joinAttempts)")
        wifiP2p.joinPeer(candidate.deviceAddress)
    }

    /**
     * Locate the Network that carries the Wi-Fi Direct group (192.168.49.0/24)
     * so UDP sockets can be bound to it. The specifier path receives its
     * Network in onAvailable; the discovery/MAC path gets no callback at all,
     * so we find it by scanning each network's own addresses.
     */
    @Suppress("DEPRECATION") // allNetworks is deprecated but remains the only
    // way to scan for the P2P interface across the API levels we support.
    private fun findP2pNetwork(): Network? {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return null
        for (net in cm.allNetworks) {
            val caps = try { cm.getNetworkCapabilities(net) } catch (_: Exception) { null }
                ?: continue
            if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue
            val lp = try { cm.getLinkProperties(net) } catch (_: Exception) { null } ?: continue
            val onP2pSubnet = lp.linkAddresses.any {
                val a = it.address.hostAddress ?: return@any false
                a.startsWith("192.168.49.")
            }
            if (onP2pSubnet) {
                AppLog.i("Client: P2P interface found — binding sockets to it")
                return net
            }
        }
        AppLog.w("Client: no 192.168.49.x interface yet — using the default route")
        return null
    }

    /**
     * Safety net for the QR/specifier path: if the join has not verified within
     * 10 s, fall back to MAC-based peer discovery. Some OEMs reject the
     * specifier request without ever firing onUnavailable(), which previously
     * left the client hanging on "JOINING" forever with no diagnostic.
     */
    

    /** Receive 1936-byte audio datagrams on the audio port. */
    private fun runNet() {
        val sock = DatagramSocket(audioPort)
        boundNetwork?.bindSocket(sock)
        sock.soTimeout = 1000
        val buf = ByteArray(1936) // SYNC_WIFI_PACKET_BYTES
        AppLog.i("Client: audio listener on udp $audioPort")
        while (!stop) {
            try {
                val p = DatagramPacket(buf, buf.size)
                sock.receive(p)
                if (p.length == 1936) {
                    recvPkts++
                    NativeBridge.clientFeed(p.data, p.length)
                    connected = true
                    lastSeenMs = System.currentTimeMillis()
                    status = NativeBridge.clientStatus6()
                }
            } catch (_: java.net.SocketTimeoutException) {
            } catch (t: Throwable) {
                if (stop) break
                AppLog.e("UDP: audio receive error", t)
            }
        }
        sock.close()
    }

    private fun runPlay() {
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            // Defensive: this playback can never be captured by anything
            // on this phone (would only matter if a capture ran here).
            .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
            .build()
        val fmt = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(48000)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        val track = AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(1920 * 4)   // 40 ms: tight (was 80 ms); the ring's
                                             // 40 ms target absorbs WiFi jitter
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track.play()
        updateNotif("SyncWifi client: playing in sync…")
        val out = ShortArray(480 * 2)
        var ticks = 0
        var stalled = false
        while (!stop) {
            val n = NativeBridge.clientRender(out, 480)
            if (n > 0) {
                // Recovering from a stall: drop the silence queued during the
                // gap so fresh packets reach the bud immediately instead of
                // sitting behind a buffer of stale silence.
                if (stalled) {
                    stalled = false
                    try { track.flush() } catch (_: Throwable) { }
                }
                track.write(out, 0, n * 2)
            } else {
                stalled = true
                stalls++
                track.write(ShortArray(480 * 2), 0, 480 * 2)
                try { Thread.sleep(10) } catch (_: InterruptedException) { break }
            }
            if (++ticks % 200 == 0) {
                status = NativeBridge.clientStatus6()
                dupPkts = status[4].toLong()
            }
        }
        track.stop()
        track.release()
        running = false
        stopSelf()
    }

    override fun onDestroy() {
        stop = true
        // Release the WifiNetworkSpecifier request — this disconnects the
        // peer-to-peer link the system joined for us.
        try {
            networkCallback?.let {
                getSystemService(ConnectivityManager::class.java)?.unregisterNetworkCallback(it)
            }
        } catch (_: Throwable) { }
        verifyThread?.join(1500)
        netThread?.join(1500)
        playThread?.join(1500)
        helloThread?.join(1500)
        // join(1500) cannot kill a daemon thread that is mid-sleep/receive;
        // interrupt() hits the `catch (InterruptedException) { break }` paths
        // in every worker loop, so nothing keeps running after teardown.
        listOf(verifyThread, netThread, playThread, helloThread).forEach { t ->
            if (t?.isAlive == true) t.interrupt()
        }
        wifiP2p.cleanup()
        running = false
        connected = false
        linkVerified = false
        AppLog.i("Client: service destroyed")
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

    private fun startForegroundCompat(id: Int, notification: Notification) {
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(id, notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(id, notification)
        }
    }
}
