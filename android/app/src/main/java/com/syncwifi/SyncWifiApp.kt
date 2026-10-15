package com.syncwifi

import android.app.Application

/**
 * Application entry point. All behaviour lives in [CrashHandler]; this class
 * only decides whether to install it (never inside the ":crash" viewer
 * process, which must stay alive to display a crash).
 */
class SyncWifiApp : Application() {

    companion object {
        /** Written by CrashHandler, read by CrashActivity. */
        const val CRASH_FILE = "last_crash.txt"
    }

    override fun onCreate() {
        super.onCreate()
        if (!getProcessName().endsWith(":crash")) {
            CrashHandler.install(this)
        }
    }
}
