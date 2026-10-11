package com.syncwifi

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pDevice
import android.net.wifi.p2p.WifiP2pDeviceList
import android.net.wifi.p2p.WifiP2pGroup
import android.net.wifi.p2p.WifiP2pInfo
import android.net.wifi.p2p.WifiP2pManager
import android.os.Handler
import android.os.Looper
import java.net.InetAddress
import java.net.UnknownHostException

/**
 * Lifecycle-safe wrapper around WifiP2pManager.
 *
 * Roles:
 *   MASTER (Group Owner): create P2P group via discoverPeers(), then read
 *                          our own P2P IP from group info (requestGroupInfo).
 *                          Outbound UDP goes to the P2P subnet broadcast
 *                          (192.168.49.255 on port 48200).
 *   CLIENT:               discover peers, pick the master, joinPeer() to
 *                          connect, then requestConnectionInfo() to learn
 *                          the master's groupOwnerAddress (P2P IP). Then
 *                          unicast "SYNC-HELLO" + audio directly to that IP.
 *
 * Wi-Fi Direct is asynchronous/event-driven: no synchronous "connect()".
 * All state arrives via callbacks + P2P broadcast intents.
 */
class WifiP2pManagerWrapper(private val context: Context) {

    private var mWifiP2pManager: WifiP2pManager? = null
    private var mChannel: WifiP2pManager.Channel? = null

    var onPeersAvailable: ((List<WifiP2pDevice>) -> Unit)? = null
    var onMasterIp: ((InetAddress?) -> Unit)? = null
    var onGroupInfo: ((WifiP2pGroup?) -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onGroupFormed: (() -> Unit)? = null
    /** true = this device is the Group Owner of a formed group. */
    var onGroupOwner: ((Boolean) -> Unit)? = null
    var onGroupCreated: (() -> Unit)? = null

    private val mHandler = Handler(Looper.getMainLooper())

    fun initialize(wm: WifiP2pManager) {
        mWifiP2pManager = wm
        mChannel = wm.initialize(
            context,
            Looper.getMainLooper(),
            object : WifiP2pManager.ChannelListener {
                override fun onChannelDisconnected() { /* channel dropped */ }
            })
    }

    fun isInitialised(): Boolean = mChannel != null

    /** Start discovery + create the P2P group (MASTER) or begin scanning (CLIENT). */
    fun discoverPeers() {
        mWifiP2pManager?.discoverPeers(mChannel!!, object : WifiP2pManager.ActionListener {
            override fun onSuccess() { /* discovery started */ }
            override fun onFailure(reason: Int) {
                onError?.invoke("discoverPeers failed: $reason")
            }
        })
    }

    /** MASTER ONLY: create the P2P group; we become the Group Owner (GO).
     *  On success read SSID/passphrase via requestGroupInfo() to build the
     *  join QR; on failure (often "busy" = a group already exists) still
     *  try to read the existing group so the QR can be shown. */
    fun createGroup() {
        createGroupNamed(GROUP_SSID, GROUP_PASS)
    }

    /**
     * MASTER: create the group with an explicit, SPACE-FREE SSID.
     *
     * Android's default group name is "DIRECT-xx-<device model>", and several
     * OEMs (Samsung: "DIRECT-7w-Galaxy F17 5G") embed the model name verbatim,
     * SPACES included. A client joining that network through
     * WifiNetworkSpecifier then fails to match and never associates — the join
     * looks successful, requestConnectionInfo() never yields a GO address, and
     * every PING times out with no clue as to why.
     *
     * Naming the group ourselves makes the SSID deterministic and safe on every
     * device, so the QR always carries a string any client can join.
     */
    fun createGroupNamed(ssid: String, pass: String) {
        val wm = mWifiP2pManager
        val ch = mChannel
        if (wm == null || ch == null) return

        // NOTE: an empty WifiP2pConfig.Builder().build() THROWS on Android 15+
        // ("peer address must be set if network name and passphrase are not
        // set"), so the fallback must be the no-config createGroup overload,
        // never a blank Builder. (A blank Builder here previously crashed
        // MasterService.onCreate on Android 16.)
        val named = try {
            WifiP2pConfig.Builder()
                .setNetworkName(ssid)
                .setPassphrase(pass)
                .build()
        } catch (t: Throwable) {
            // Newer Android releases restrict naming a group to privileged
            // callers; the values are dropped and build() rejects the config.
            AppLog.e("P2P: cannot name the group ($ssid) — using platform default", t)
            null
        }

        val listener = object : WifiP2pManager.ActionListener {
            override fun onSuccess() { onGroupCreated?.invoke() }
            override fun onFailure(reason: Int) {
                onError?.invoke("createGroup failed: ${reasonName(reason)}")
                requestGroupInfo()
            }
        }

        if (named != null) wm.createGroup(ch, named, listener)
        else wm.createGroup(ch, listener)
    }

    /** Human-readable WifiP2pManager action-failure reason codes — the raw
     *  int ("failed: 2") is useless when diagnosing on-device from AppLog. */
    private fun reasonName(reason: Int) = when (reason) {
        WifiP2pManager.ERROR -> "ERROR(0)"
        WifiP2pManager.BUSY -> "BUSY(1)"
        WifiP2pManager.P2P_UNSUPPORTED -> "P2P_UNSUPPORTED(2 — is Wi-Fi on & P2P supported?)"
        3 -> "NO_SERVICE_REQUESTS(3)"
        else -> "DISALLOWED/other($reason)"
    }

    /** Query the current peer list (fires onPeersAvailable). Called when the
     *  PEERS_CHANGED broadcast arrives — discovery alone never invokes the
     *  callback, so without this the legacy path never found the master. */
    fun requestPeers() {
        val wm = mWifiP2pManager
        val ch = mChannel
        if (wm == null || ch == null) return
        wm.requestPeers(ch, object : WifiP2pManager.PeerListListener {
            override fun onPeersAvailable(peers: WifiP2pDeviceList?) {
                val list = peers?.deviceList?.toList() ?: emptyList()
                onPeersAvailable?.invoke(list)
            }
        })
    }

    /** CLIENT ONLY: join the master's existing group (via 4-way handshake). */
    fun joinPeer(deviceAddress: String) {
        val config = WifiP2pConfig().apply {
            this.deviceAddress = deviceAddress
        }
        mWifiP2pManager?.connect(mChannel!!, config, object : WifiP2pManager.ActionListener {
            override fun onSuccess() {
                // Handshake started; when the group forms,
                // requestConnectionInfo() returns the group owner's IP.
            }
            override fun onFailure(reason: Int) {
                onError?.invoke("connect failed: $reason")
            }
        })
    }

    /** MASTER ONLY: read our P2P group details (self = Group Owner). */
    fun requestGroupInfo() {
        mWifiP2pManager?.requestGroupInfo(mChannel!!, object : WifiP2pManager.GroupInfoListener {
            override fun onGroupInfoAvailable(group: WifiP2pGroup?) {
                onGroupInfo?.invoke(group)
                if (group != null && group.clientList.size > 0) {
                    onGroupFormed?.invoke()
                }
            }
        })
    }

    /** CLIENT ONLY: after joining, query the master's connection info
     * (groupOwnerAddress = master's P2P IP). */
    fun requestConnectionInfo() {
        mWifiP2pManager?.requestConnectionInfo(mChannel!!, object : WifiP2pManager.ConnectionInfoListener {
            override fun onConnectionInfoAvailable(info: WifiP2pInfo) {
                // groupFormed is the only trustworthy "we are in a group"
                // signal: it is false throughout negotiation and flips only
                // when the group really exists. Reporting a GO address while
                // it is false made the caller believe it had connected.
                if (info.groupFormed) {
                    onMasterIp?.invoke(info.groupOwnerAddress)
                    onGroupOwner?.invoke(info.isGroupOwner)
                } else {
                    AppLog.d("P2P: connection changed but group NOT formed yet")
                }
            }
        })
    }

    fun cleanup() {
        mWifiP2pManager?.removeGroup(mChannel!!, null)
        mChannel?.let { mWifiP2pManager?.removeGroup(it, null); mChannel = null }
        mWifiP2pManager = null
    }

    fun registerIntentReceiver(receiver: BroadcastReceiver, context: Context) {
        val filter = IntentFilter()
        filter.addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION)
        filter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION)
        filter.addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION)
        context.registerReceiver(receiver, filter)
    }

    fun unregisterIntentReceiver(receiver: BroadcastReceiver) {
        try { context.unregisterReceiver(receiver) } catch (_: Exception) { }
    }

    fun inetFromHostOrder(hostOrderInt: Int): InetAddress? {
        return try {
            InetAddress.getByAddress(intToByteArray(hostOrderInt))
        } catch (_: UnknownHostException) { null }
    }

    private fun intToByteArray(i: Int): ByteArray {
        return byteArrayOf(
            (i and 0xFF).toByte(),
            ((i shr 8) and 0xFF).toByte(),
            ((i shr 16) and 0xFF).toByte(),
            ((i shr 24) and 0xFF).toByte()
        )
    }

    companion object {
        /** Fixed, space-free group identity — see createGroupNamed(). */
        const val GROUP_SSID = "SyncWifi-48200"
        const val GROUP_PASS = "syncwifi48200"
    }
}
