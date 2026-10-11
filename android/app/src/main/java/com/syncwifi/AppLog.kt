package com.syncwifi

import android.util.Log
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentLinkedDeque

/**
 * In-memory debug log, readable directly on the device (MainActivity's log
 * panel) so socket/JNI problems can be diagnosed without adb logcat.
 *
 * - Thread-safe (written from service worker threads + UI, read by the UI).
 * - Bounded ring: keeps the newest [MAX] entries.
 * - Rate-limited dedup: an identical message within [DEDUP_MS] of the last
 *   one only bumps a repeat counter (UDP send failures fire at ~100 Hz and
 *   must not drown the log or the UI).
 * - Every entry is also mirrored to logcat under the "SyncWifi" tag.
 */
object AppLog {

    private const val TAG = "SyncWifi"
    private const val MAX = 250
    private const val DEDUP_MS = 2000L

    private class Entry(val ts: Long, val level: String, val text: String, var repeats: Int)

    private val entries = ConcurrentLinkedDeque<Entry>()
    private val fmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    fun d(msg: String) = log("D", msg)
    fun i(msg: String) = log("I", msg)
    fun w(msg: String) = log("W", msg)
    fun e(msg: String, t: Throwable? = null) =
        log("E", if (t != null) "$msg :: $t" else msg)

    private fun log(level: String, msg: String) {
        val now = System.currentTimeMillis()
        val last = entries.peekLast()
        if (last != null && last.text == msg && now - last.ts < DEDUP_MS) {
            last.repeats++
        } else {
            entries.addLast(Entry(now, level, msg, 0))
            while (entries.size > MAX) entries.pollFirst()
            Log.println(prio(level), TAG, msg)
        }
    }

    /** Newest-last snapshot of the last [max] lines for on-screen display. */
    fun dump(max: Int = 12): String {
        val all = entries.toList().takeLast(max)
        if (all.isEmpty()) return "(log empty)"
        return all.joinToString("\n") { e ->
            val rep = if (e.repeats > 0) " (+${e.repeats}x)" else ""
            "${fmt.format(Date(e.ts))} ${e.level} ${e.text}$rep"
        }
    }

    private fun prio(level: String) = when (level) {
        "E" -> Log.ERROR
        "W" -> Log.WARN
        "D" -> Log.DEBUG
        else -> Log.INFO
    }
}
