package com.example.fm.sdk

import android.content.Context
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.io.File

/**
 * Manages an active FM radio reception session, coordinating receiver control,
 * audio routing, scanning, and recording.
 */
class OpenFmSession(
    private val context: Context,
    private val listener: OpenFmListener?
) {
    private val coroutineScope = CoroutineScope(Dispatchers.Main + Job())

    val audio = OpenFmAudio(context)
    val receiver = OpenFmReceiver(listener)
    val scanner = OpenFmScanner(coroutineScope, listener)
    val recorder = OpenFmRecorder(context)

    private var isActive = false

    /**
     * Activates the session: gains audio focus and powers up hardware.
     */
    fun start() {
        if (isActive) return
        Log.i("OpenFmSession", "Starting OpenFM Session")
        
        // Request audio focus
        if (!audio.requestPlaybackFocus()) {
            Log.w("OpenFmSession", "Audio focus was denied, proceeding anyway")
        }

        // Power on hardware receiver
        val success = receiver.powerUp()
        if (success) {
            isActive = true
        } else {
            audio.abandonPlaybackFocus()
            throw OpenFmException("Failed to start session: Tuner HAL failed to initialize", OpenFmException.ERROR_HARDWARE_UNAVAILABLE)
        }
    }

    /**
     * Deactivates the session: stops scans, recording, powers down hardware, and releases audio focus.
     */
    fun stop() {
        if (!isActive) return
        Log.i("OpenFmSession", "Stopping OpenFM Session")
        
        try {
            if (scanner.isScanningActive()) {
                scanner.cancelSeek()
            }
            if (recorder.isRecordingActive()) {
                recorder.stopRecording()
            }
            receiver.powerDown()
            audio.abandonPlaybackFocus()
        } catch (e: Exception) {
            Log.e("OpenFmSession", "Error during session stop cleanup", e)
        } finally {
            isActive = false
        }
    }

    /**
     * Determines whether this session is currently alive and active.
     */
    fun isActive(): Boolean = isActive
}
