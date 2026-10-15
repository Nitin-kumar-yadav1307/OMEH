package com.syncwifi

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Process
import android.util.Log
import com.syncwifi.ui.CrashActivity
import com.syncwifi.util.AppLog
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Global on-device crash tracking (no adb logcat needed):
 * any unhandled exception is (1) appended to filesDir/[SyncWifiApp.CRASH_FILE]
 * and (2) shown full-screen by CrashActivity, which runs in the ":crash"
 * process so it survives the death of the crashing process. The previous
 * (system) handler is chained afterwards so Android still shows its
 * "keeps stopping" dialog and kills the app the normal way.
 */
object CrashHandler {

    private const val TAG = "SyncWifi"

    /**
     * Install the handler. Must NOT be called inside the ":crash" process —
     * that process only displays crashes and must never become a source of
     * them (installing there would recurse into itself).
     */
    fun install(context: Context) {
        val app = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            val time = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(Date())
            val trace = buildString {
                appendLine("FATAL: $throwable")
                appendLine("Thread: ${thread.name} (id=${thread.id})")
                appendLine("Time: $time")
                appendLine("Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
                appendLine("${Build.MANUFACTURER} ${Build.MODEL}")
                appendLine()
                appendLine(Log.getStackTraceString(throwable))
            }
            Log.e(TAG, "Uncaught exception on ${thread.name}", throwable)
            AppLog.e("CRASH on ${thread.name}", throwable)
            // Persist for later inspection (CrashActivity reads this file).
            try {
                File(app.filesDir, SyncWifiApp.CRASH_FILE)
                    .appendText("\n\n==== $time ====\n$trace")
            } catch (t: Throwable) {
                Log.e(TAG, "Cannot persist crash", t)
            }
            // Show the crash screen in the separate :crash process.
            try {
                app.startActivity(
                    Intent(app, CrashActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        .putExtra(CrashActivity.EXTRA_TRACE, trace)
                )
            } catch (t: Throwable) {
                Log.e(TAG, "Cannot launch CrashActivity", t)
            }
            // Chain: let the system kill the app cleanly (original handler).
            if (previous != null) {
                previous.uncaughtException(thread, throwable)
            } else {
                Process.killProcess(Process.myPid())
            }
        }
    }
}