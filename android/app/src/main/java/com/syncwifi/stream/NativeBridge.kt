package com.syncwifi.stream

/** JNI to native-lib.cpp (proven sync_wifi.h engine). */
object NativeBridge {
    init {
        System.loadLibrary("native-lib")
    }

    // Client engine
    external fun clientReset()
    /** Feed one UDP datagram; returns ring level after write (<0 = reject). */
    external fun clientFeed(data: ByteArray, len: Int): Int
    /** Render nframes stereo s16 into out (returns frames written). */
    external fun clientRender(out: ShortArray, nframes: Int): Int
    /** [level, target, ratio, lost, dup] for the status line. */
    external fun clientStatus(): DoubleArray

    /**
     * Status, normalised to exactly 6 fields:
     * [level(frames), targetMs, ratio, lost, dup, stalls]
     *
     * The native array is padded/truncated rather than trusted, so the UI can
     * never die on an ABI skew between the .so and this Kotlin (e.g. a stale
     * native build after a field is added, which previously crashed the status
     * tick with ArrayIndexOutOfBounds before the first poll returned).
     */
    fun clientStatus6(): DoubleArray {
        val a = clientStatus()
        return if (a.size >= 6) a
        else DoubleArray(6) { i -> if (i < a.size) a[i] else 0.0 }
    }

    // Master packet builder
    /** Build one wire packet; seq/pts owned by Kotlin counters. */
    external fun masterBuild(seq: Int, pts: Long, pcm: ShortArray): ByteArray

    const val PORT = 48200
    /** Control plane: UDP PING/PONG link verification + SYNC-HELLO/ACK. */
    const val CTRL_PORT = 48201
}
