package com.syncwifi.ui

import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.net.wifi.WifiManager
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import com.syncwifi.stream.ClientService
import com.syncwifi.stream.MasterService
import com.syncwifi.stream.NativeBridge
import com.syncwifi.util.AppLog

/**
 * UI (fully programmatic — this project has no res/layout XML):
 *
 *  - MASTER: "START AS MASTER" -> MediaProjection consent -> P2P group is
 *    created (we are the Group Owner) and its SSID/passphrase are encoded
 *    into a join QR rendered on screen.
 *  - CLIENT: "SCAN TO JOIN" scans that QR -> ClientService joins the group
 *    via WifiNetworkSpecifier (no Settings page) -> UDP PING/PONG proves the
 *    link -> status turns GREEN "CONNECTED & VERIFIED" and audio starts.
 *  - "START AS CLIENT (no QR)" keeps the legacy discover-and-connect path.
 *  - "LAST CRASH" reopens the saved on-device crash trace; the bottom panel
 *    shows the AppLog tail (JNI/UDP errors) — no adb logcat needed.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var masterIpView: TextView
    private lateinit var statusView: TextView
    private lateinit var logView: TextView
    private lateinit var qrView: ImageView
    private lateinit var masterBtn: Button
    private lateinit var clientBtn: Button
    private lateinit var scanBtn: Button
    private lateinit var stopBtn: Button
    private lateinit var crashBtn: Button
    private val handler = Handler(Looper.getMainLooper())
    private val RC_CAPTURE = 1001
    private lateinit var permissionLauncher: ActivityResultLauncher<Array<String>>
    private lateinit var scanLauncher: ActivityResultLauncher<ScanOptions>
    private var lastQrShown = ""

    /**
     * Wi-Fi Direct discovery (legacy fallback path) needs ACCESS_FINE_LOCATION
     * on Android <= 12 and NEARBY_WIFI_DEVICES on Android 13+. Returns false
     * when a permission dialog was just launched so the caller can ask the
     * user to tap again. The QR/specifier path needs none of these.
     */
    private fun ensurePermissions(): Boolean {
        val needed = if (Build.VERSION.SDK_INT >= 33) {
            listOf(android.Manifest.permission.NEARBY_WIFI_DEVICES)
        } else {
            listOf(
                android.Manifest.permission.ACCESS_FINE_LOCATION,
                android.Manifest.permission.ACCESS_COARSE_LOCATION
            )
        }
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) !=
                PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        permissionLauncher.launch(missing.toTypedArray())
        return false
    }

    private fun localIpString(): String {
        return try {
            val wm = applicationContext.getSystemService(WifiManager::class.java)
            val dhcp = wm?.dhcpInfo
            if (dhcp != null && dhcp.ipAddress != 0) {
                val ip = dhcp.ipAddress
                "${ip and 0xFF}.${(ip shr 8) and 0xFF}.${(ip shr 16) and 0xFF}.${(ip shr 24) and 0xFF}"
            } else {
                "-"
            }
        } catch (_: Exception) {
            "-"
        }
    }

    /**
     * Parses the master's join QR:
     *   WIFI:T:WPA;S:<ssid>;P:<pass>;X:syncwifi-port=<port>;;
     * (\X escape sequences are un-escaped.) Returns (ssid, pass, port) or
     * null when this is not a SyncWifi join QR.
     */
    private data class QrCreds(val ssid: String, val pass: String,
                                 val port: Int, val masterName: String?)

    private fun parseWifiQr(raw: String): QrCreds? {
        if (!raw.startsWith("WIFI:")) return null
        val unesc = Regex("\\\\([\\\\;,\":])")
        fun field(key: String): String? =
            Regex("(?:^|;)${Regex.escape(key)}:((?:\\\\.|[^;])*)")
                .find(raw)?.groupValues?.get(1)?.replace(unesc, "$1")
        val ssid = field("S") ?: return null
        val pass = field("P") ?: return null
        val port = field("X")?.substringAfter("syncwifi-port=")?.toIntOrNull()
            ?: NativeBridge.PORT
        // The master's P2P DEVICE name, which is what peer discovery actually
        // exposes. The group SSID ("DIRECT-7w-Galaxy F17 5G") is a DIFFERENT
        // string, so matching it against discovered peers never worked.
        val masterName = field("X")?.substringAfter("syncwifi-master=")
            ?.takeIf { it.isNotBlank() }
        return QrCreds(ssid, pass, port, masterName)
    }

    /** Renders the join QR once per payload change (ZXing -> ImageView). */
    private fun showQr(payload: String) {
        if (payload.isEmpty() ||
            (payload == lastQrShown && qrView.visibility == ImageView.VISIBLE)) return
        try {
            val hints = mapOf(
                EncodeHintType.MARGIN to 1,
                EncodeHintType.CHARACTER_SET to "UTF-8"
            )
            val matrix = QRCodeWriter().encode(payload, BarcodeFormat.QR_CODE, 640, 640, hints)
            val bmp = Bitmap.createBitmap(matrix.width, matrix.height, Bitmap.Config.RGB_565)
            val pixels = IntArray(matrix.width * matrix.height) { i ->
                if (matrix.get(i % matrix.width, i / matrix.width)) Color.BLACK else Color.WHITE
            }
            bmp.setPixels(pixels, 0, matrix.width, 0, 0, matrix.width, matrix.height)
            qrView.setImageBitmap(bmp)
            qrView.visibility = ImageView.VISIBLE
            lastQrShown = payload
            AppLog.i("QR rendered (${matrix.width}px) — client: SCAN TO JOIN")
        } catch (t: Throwable) {
            AppLog.e("QR render failed", t)
        }
    }

    private val statusTick = object : Runnable {
        override fun run() {
            when {
                MasterService.running -> {
                    masterIpView.text =
                        "Master P2P IP: ${MasterService.p2pIp}  broadcast: ${MasterService.p2pBroadcastIp}"
                    statusView.setTextColor(Color.rgb(168, 85, 247))
                    statusView.text = buildString {
                        append(if (MasterService.qrReady) "MASTER (Group Owner) — QR ready"
                               else "MASTER — forming group…")
                        append(if (MasterService.capturing) "  • capturing audio"
                               else "  • no capture consent yet")
                        append("\n")
                        append("devices=${MasterService.clientCount} sent=${MasterService.packetsSent} ")
                        append("captured=${MasterService.captureFrames} hellos=${MasterService.helloCount} ")
                        append("pings=${MasterService.pingCount}\n")
                        append("reads=${MasterService.capReads} errs=${MasterService.capErrors} ")
                        append("recState=${MasterService.capRecState} ")
                        append("ch=${if (MasterService.capMono) "mono" else "stereo"} ")
                        append("ucast=${MasterService.capUnicast}\n")
                        append("SSID=${MasterService.p2pSsid}  IP=${MasterService.p2pIp}\n")
                        append(
                            if (MasterService.qrReady)
                                "Client: tap SCAN TO JOIN and point at this QR."
                            else
                                "Creating Wi-Fi Direct group — QR appears above in a moment…"
                        )
                    }
                    if (MasterService.qrReady) showQr(MasterService.qrPayload)
                    else qrView.visibility = ImageView.GONE
                }
                ClientService.running -> {
                    qrView.visibility = ImageView.GONE
                    masterIpView.text =
                        "Client of ${ClientService.masterIp}  mode=${ClientService.joinMode}"
                    val s = ClientService.status
                    val ppm = (s[2] - 1.0) * 1e6
                    val audioLive = ClientService.connected &&
                        (System.currentTimeMillis() - ClientService.lastSeenMs) < 5000
                    val verified = ClientService.linkVerified
                    // Distinguish "packets haven't arrived yet" from "link is
                    // verified but the master is sending nothing" — the latter
                    // is a MASTER-side problem (no capture consent, or no app
                    // playing audio), and saying just "waiting" hid that.
                    val verifiedIdleMs =
                        if (verified) System.currentTimeMillis() - ClientService.verifiedMs else 0L
                    val audioState = when {
                        audioLive -> "LIVE"
                        !verified -> "waiting for first packet"
                        verifiedIdleMs > 6000 ->
                            "NO AUDIO — master is not capturing (tap Allow + play music on master)"
                        else -> "priming…"
                    }
                    statusView.setTextColor(
                        if (verified) Color.rgb(168, 85, 247) else Color.rgb(240, 90, 140)
                    )
                    statusView.text = buildString {
                        append(if (verified) "CONNECTED & VERIFIED" else "JOINING — NOT VERIFIED")
                        append("\n")
                        append("buffer=${s[1].toInt()}ms  level=%.0f  ratio=%.6f (%+.0fppm)\n"
                            .format(s[0], s[2], ppm))
                        append("audio=$audioState  lost=%.0f dup=%.0f stalls=%.0f\n"
                            .format(s[3], s[4], s[5]))
                        append("recv=${ClientService.recvPkts}  ~${s[1].toInt() + 40}ms behind master\n")
                        append("hello sent=${ClientService.helloSent}  ping=${ClientService.pingCount}\n")
                        append("SSID=${ClientService.pendingSsid ?: "-"}  master=${ClientService.masterIp}")
                    }
                }
                else -> {
                    qrView.visibility = ImageView.GONE
                    masterIpView.text = "Local IP: ${localIpString()}"
                    statusView.setTextColor(Color.rgb(180, 170, 200))
                    statusView.text =
                        "Idle. Master: START AS MASTER + Allow, show QR.\n" +
                        "Client: SCAN TO JOIN and point at the QR."
                }
            }
            logView.text = AppLog.dump(12)
            // Build marker so a stale APK is obvious on the phone (no ADB).
            if (verView.text != BUILD_TAG) {
                verView.text = BUILD_TAG
                verView.setTextColor(Color.rgb(160, 150, 180))
            }
            handler.postDelayed(this, 500)
        }
    }

    /** (no pending state: the master button no longer blocks on permissions) */

    /**
     * Visible build marker — shown at the bottom of the screen so you can tell
     * on the phone (no ADB) whether the APK you installed is the current one.
     * BUMP THIS whenever a fix ships.
     *
     *  v6 = unconditional capture dialog + mono-capture fallback +
     *        retryable capture state + version-safe parcelable extra
     */
    private val BUILD_TAG = "SyncWifi v19 (Wi-Fi Direct band diagnostic: logs group MHz)"
    private lateinit var verView: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissionLauncher = registerForActivityResult(
            ActivityResultContracts.RequestMultiplePermissions()
        ) { grants ->
            val denied = grants.filterValues { !it }.keys.joinToString()
            if (denied.isNotEmpty()) {
                statusView.text = "Permissions denied: $denied — allow them in " +
                    "Settings > Apps > SyncWifi. Wi-Fi Direct needs them; " +
                    "screen capture does not."
                return@registerForActivityResult
            }
            // Permissions only affect Wi-Fi Direct group creation; the master
            // button no longer waits on them (the capture dialog is launched
            // unconditionally), so there is nothing to continue here.
            statusView.text = if (denied.isEmpty())
                "Permissions granted."
            else
                "Permissions denied: $denied - Wi-Fi Direct needs them; " +
                    "grant in Settings > Apps > SyncWifi, then tap START AS MASTER again."
        }
        scanLauncher = registerForActivityResult(ScanContract()) { result ->
            val contents = result.contents
            if (contents.isNullOrBlank()) {
                statusView.text = "Scan cancelled."
                return@registerForActivityResult
            }
            val parsed = parseWifiQr(contents)
            if (parsed == null) {
                AppLog.w("Scan: not a SyncWifi join QR: ${contents.take(80)}")
                statusView.text = "That QR is not a SyncWifi join code."
                return@registerForActivityResult
            }
            ClientService.pendingSsid = parsed.ssid
            ClientService.pendingPass = parsed.pass
            ClientService.pendingPort = parsed.port
            // What the client actually matches discovered peers against.
            ClientService.pendingMasterName = parsed.masterName ?: parsed.ssid
            AppLog.i("Scan: QR ok — looking for master '${ClientService.pendingMasterName}' " +
                     "(audio=${parsed.port}, ctrl=${parsed.port + 1})")
            statusView.setTextColor(Color.rgb(168, 85, 247))
            statusView.text = "QR ok — looking for '${ClientService.pendingMasterName}'…\n" +
                "Joining by Wi-Fi Direct (no Settings needed)."
            startForegroundService(Intent(this, ClientService::class.java))
        }

        qrView = ImageView(this).apply {
            visibility = ImageView.GONE
            scaleType = ImageView.ScaleType.FIT_CENTER
            adjustViewBounds = true
        }
        masterIpView = TextView(this).apply { textSize = 17f }
        statusView = TextView(this).apply { textSize = 15f }
        logView = TextView(this).apply {
            textSize = 11f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextColor(Color.rgb(150, 140, 170))
            text = "(log empty)"
        }
        masterBtn = Button(this).apply { text = "START AS MASTER" }
        clientBtn = Button(this).apply { text = "START AS CLIENT (no QR)" }
        scanBtn = Button(this).apply { text = "SCAN TO JOIN" }
        stopBtn = Button(this).apply { text = "STOP" }
        crashBtn = Button(this).apply { text = "LAST CRASH" }

        val qrSize = (280 * resources.displayMetrics.density).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            addView(qrView, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, qrSize))
            addView(masterIpView)
            addView(masterBtn)
            addView(clientBtn)
            addView(scanBtn)
            addView(stopBtn)
            addView(crashBtn)
            // Build marker (proves which APK is installed — see BUILD_TAG).
            verView = TextView(this@MainActivity).apply {
                textSize = 11f
                setPadding(24, 8, 24, 4)
            }
            addView(verView)
            addView(statusView)
            addView(ScrollView(this@MainActivity).apply {
                isFillViewport = true
                addView(logView)
            }, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(layout)

        masterBtn.setOnClickListener {
            // Screen-capture consent is launched UNCONDITIONALLY further down:
            // it requires NO location/nearby permission, so it must never be
            // gated behind the async permission request.
            // (P2P permissions are requested in parallel below — see the tail
            //  of this handler — so a missing grant can never block capture.)
            // Wi-Fi must be ON for Wi-Fi Direct (P2P) group creation; when it
            // is off, createGroup fails immediately with a confusing code.
            val wifiOn = try {
                val wm = applicationContext.getSystemService(WifiManager::class.java)
                wm?.isWifiEnabled == true
            } catch (_: Exception) { false }
            if (!wifiOn)
                AppLog.w("Master: Wi-Fi is OFF - Wi-Fi Direct group cannot be created (capture still runs)")
            statusView.setTextColor(Color.rgb(180, 170, 200))
            statusView.text = "Starting master + forming Wi-Fi Direct group…"
            // Start the service FIRST without a consent token: the P2P group
            // and the join QR do NOT need screen-capture consent, so the QR
            // appears even if the consent result is delayed or lost. The
            // consent token (when it arrives) upgrades the SAME service to
            // start capturing audio.
            startForegroundService(Intent(this, MasterService::class.java))
            // Consent must be granted BEFORE claiming the mediaProjection FGS
            // type: on Android 14 (targetSDK 34) MasterService may only claim
            // that type once the user has tapped Allow, so the token arrives
            // via a second start intent from onActivityResult.
            //
            // This runs UNCONDITIONALLY and FIRST: nothing above may return
            // early, or the system dialog never renders.
            val pm = getSystemService(MediaProjectionManager::class.java)
            AppLog.i("Master: launching screen-capture consent dialog (unconditional)")
            startActivityForResult(pm.createScreenCaptureIntent(), RC_CAPTURE)

            // P2P permissions: needed only for Wi-Fi Direct group creation.
            // Fire-and-forget so they can never block the capture dialog; the
            // service logs clearly if the group fails until they are granted.
            if (!ensurePermissions())
                AppLog.w("Master: Wi-Fi Direct permissions pending - group forms after grant")

            // Start the service without a token so the P2P group + QR form
            // while the user is still deciding on the capture dialog.
            startForegroundService(Intent(this, MasterService::class.java))
        }
        scanBtn.setOnClickListener {
            // The ZXing scanner activity requests CAMERA itself on first open.
            val opts = ScanOptions()
                .setDesiredBarcodeFormats(BarcodeFormat.QR_CODE.name)
                .setPrompt("Point at the MASTER's join QR")
                .setBeepEnabled(true)
                .setOrientationLocked(false)
            scanLauncher.launch(opts)
        }
        clientBtn.setOnClickListener {
            // Legacy fallback: no QR -> discover P2P peers inside the service.
            if (!ensurePermissions()) {
                statusView.text = "Grant the requested permissions, then tap again."
                return@setOnClickListener
            }
            statusView.text = "Starting legacy client (peer discovery)…"
            startForegroundService(Intent(this, ClientService::class.java))
        }
        stopBtn.setOnClickListener {
            stopService(Intent(this, MasterService::class.java))
            stopService(Intent(this, ClientService::class.java))
        }
        crashBtn.setOnClickListener {
            startActivity(Intent(this, CrashActivity::class.java))
        }
        handler.post(statusTick)
    }

    @Deprecated("use Activity Result API in later versions")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == RC_CAPTURE) {
            if (resultCode == RESULT_OK && data != null) {
                val i = Intent(this, MasterService::class.java)
                i.putExtra(MasterService.EXTRA_RESULT_CODE, resultCode)
                i.putExtra(MasterService.EXTRA_RESULT_DATA, data)
                // startForegroundService (not startService): the service claims
                // its foreground type immediately from this consent token.
                startForegroundService(i)
            } else {
                statusView.setTextColor(Color.rgb(220, 100, 200))
                statusView.text = "Screen capture denied — master runs WITHOUT audio " +
                    "(QR still shown above). Tap START AS MASTER to retry capture."
            }
        }
    }

    override fun onDestroy() {
        handler.removeCallbacks(statusTick)
        super.onDestroy()
    }
}
