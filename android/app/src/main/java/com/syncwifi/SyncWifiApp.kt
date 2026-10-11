package com.syncwifi

import android.app.Application
import android.content.Intent
import android.os.Build
import android.os.Process
import android.util.Log
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Global on-device crash tracking (no adb logcat needed):
 * any unhandled exception is (1) appended to filesDir/last_crash.txt and
 * (2) shown full-screen by CrashActivity, which runs in the ":crash"
 * process so it survives the death of the crashing process. The previous
 * (system) handler is chained afterwards so Android still shows its
 * "keeps stopping" dialog and kills the app the normal way.
 */
class SyncWifiApp : Application() {

    companion object {
        const val CRASH_FILE = "last_crash.txt"
    }

    override fun onCreate() {
        super.onCreate()
        // Application.onCreate runs in every process — only install the
        // handler in the main process (never inside the :crash viewer).
        if (!getProcessName().endsWith(":crash")) {
            installCrashHandler()
        }
    }

    private fun installCrashHandler() {
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
            Log.e("SyncWifi", "Uncaught exception on ${thread.name}", throwable)
            AppLog.e("CRASH on ${thread.name}", throwable)
            // Persist for later inspection (CrashActivity reads this file).
            try {
                File(filesDir, CRASH_FILE)
                    .appendText("\n\n==== $time ====\n$trace")
            } catch (t: Throwable) {
                Log.e("SyncWifi", "Cannot persist crash", t)
            }
            // Show the crash screen in the separate :crash process.
            try {
                startActivity(
                    Intent(this, CrashActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        .putExtra(CrashActivity.EXTRA_TRACE, trace)
                )
            } catch (t: Throwable) {
                Log.e("SyncWifi", "Cannot launch CrashActivity", t)
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
