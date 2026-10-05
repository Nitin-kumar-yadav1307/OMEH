package com.syncwifi

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
    /** [level, target, ratio, lost] for the status line. */
    external fun clientStatus(): DoubleArray

    // Master packet builder
    /** Build one wire packet; seq/pts owned by Kotlin counters. */
    external fun masterBuild(seq: Int, pts: Long, pcm: ShortArray): ByteArray

    const val PORT = 48200
}
