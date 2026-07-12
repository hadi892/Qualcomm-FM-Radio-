package com.example.fm.sdk

import android.content.Context
import android.util.Log
import com.example.fm.FmNative

/**
 * Main public entry point of the OpenFM Platform SDK.
 * Use this to query hardware capability, run diagnostic assessments, and create sessions.
 */
class OpenFmManager(private val context: Context) {

    /**
     * Queries whether physical over-the-air FM hardware, drivers, or HAL endpoints are accessible.
     */
    fun isHardwareSupported(): Boolean {
        return try {
            FmNative.isHardwareSupported()
        } catch (e: Throwable) {
            Log.e("OpenFmManager", "Failed to query native hardware support flag", e)
            false
        }
    }

    /**
     * Executes diagnostic scripts in JNI/HAL layers to generate human-readable details.
     */
    fun getDiagnosticsReport(): String {
        return try {
            FmNative.getDiagnosticsReport()
        } catch (e: Throwable) {
            "SDK Diagnostic Error: Native engine failed to execute. Details: ${e.localizedMessage}"
        }
    }

    /**
     * Starts and returns a new stable FM Reception Session.
     * @param listener Callback handler for real-time radio events.
     */
    fun openSession(listener: OpenFmListener?): OpenFmSession {
        Log.i("OpenFmManager", "Creating a new OpenFM Session")
        val session = OpenFmSession(context, listener)
        try {
            session.start()
        } catch (e: OpenFmException) {
            Log.e("OpenFmManager", "Failed to start requested FM session", e)
            throw e
        } catch (e: Exception) {
            Log.e("OpenFmManager", "Unexpected exception starting FM session", e)
            throw OpenFmException("Session start crashed: ${e.message}", OpenFmException.ERROR_UNKNOWN, e)
        }
        return session
    }
}
