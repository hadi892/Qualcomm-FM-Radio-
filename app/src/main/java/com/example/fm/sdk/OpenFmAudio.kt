package com.example.fm.sdk

import android.content.Context
import android.media.AudioManager
import android.util.Log

/**
 * Handles audio routing, muting, volume control, and headset physical loop status.
 */
class OpenFmAudio(private val context: Context) {

    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    /**
     * Checks if a wired headset is physically plugged in.
     * Essential for FM as the wire serves as the physical RF antenna.
     */
    @Suppress("DEPRECATION")
    fun isHeadsetConnected(): Boolean {
        return try {
            audioManager.isWiredHeadsetOn
        } catch (e: Exception) {
            Log.e("OpenFmAudio", "Error querying wired headset status", e)
            false
        }
    }

    /**
     * Programs the system audio route between the device's main loudspeaker and wired accessories.
     * @param route "SPEAKER" or "HEADSET".
     */
    @Suppress("DEPRECATION")
    fun setAudioRoute(route: String): Boolean {
        Log.i("OpenFmAudio", "Requesting audio route: $route")
        return try {
            if (route.equals("SPEAKER", ignoreCase = true)) {
                audioManager.isSpeakerphoneOn = true
            } else {
                audioManager.isSpeakerphoneOn = false
            }
            true
        } catch (e: Exception) {
            Log.e("OpenFmAudio", "Failed to apply requested audio route", e)
            false
        }
    }

    /**
     * Gets the currently active audio routing path.
     */
    @Suppress("DEPRECATION")
    fun getActiveRoute(): String {
        return if (audioManager.isSpeakerphoneOn) "SPEAKER" else "HEADSET"
    }

    /**
     * Requests audio focus for FM playback.
     */
    @Suppress("DEPRECATION")
    fun requestPlaybackFocus(): Boolean {
        return try {
            val result = audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN
            )
            result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        } catch (e: Exception) {
            Log.e("OpenFmAudio", "Error requesting audio focus", e)
            false
        }
    }

    /**
     * Deselects audio focus when FM is deactivated.
     */
    @Suppress("DEPRECATION")
    fun abandonPlaybackFocus() {
        try {
            audioManager.abandonAudioFocus(null)
        } catch (e: Exception) {
            Log.e("OpenFmAudio", "Error abandoning audio focus", e)
        }
    }
}
