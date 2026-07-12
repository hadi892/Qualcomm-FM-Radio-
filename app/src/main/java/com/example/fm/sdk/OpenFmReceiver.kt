package com.example.fm.sdk

import android.util.Log
import com.example.fm.FmNative

/**
 * Direct abstraction layer representing the physical tuner transceiver.
 */
class OpenFmReceiver(private val listener: OpenFmListener?) {

    private var isPowerOn = false

    /**
     * Powers up the physical FM receiver chip.
     */
    fun powerUp(): Boolean {
        if (isPowerOn) return true
        Log.i("OpenFmReceiver", "Powering up receiver hardware...")
        try {
            val result = FmNative.initFm()
            if (result == 0) {
                isPowerOn = true
                listener?.onPowerStatusChanged(true)
                return true
            } else {
                Log.e("OpenFmReceiver", "FmNative initFm returned non-zero code: $result")
                listener?.onError(OpenFmException("Failed to initialize hardware over-the-air HAL", OpenFmException.ERROR_HARDWARE_UNAVAILABLE))
                return false
            }
        } catch (e: Exception) {
            Log.e("OpenFmReceiver", "Exception during hardware power up", e)
            listener?.onError(OpenFmException("Hardware communication failure during power-up: ${e.message}", OpenFmException.ERROR_HAL_COMMUNICATION_FAILURE, e))
            return false
        }
    }

    /**
     * Powers down the receiver chip.
     */
    fun powerDown(): Boolean {
        if (!isPowerOn) return true
        Log.i("OpenFmReceiver", "Powering down receiver hardware...")
        try {
            val result = FmNative.closeFm()
            isPowerOn = false
            listener?.onPowerStatusChanged(false)
            return result == 0
        } catch (e: Exception) {
            Log.e("OpenFmReceiver", "Exception during hardware power down", e)
            return false
        }
    }

    /**
     * Tunes the physical receiver to a frequency in KHz.
     */
    fun tune(frequencyKHz: Int): Boolean {
        if (frequencyKHz < 87500 || frequencyKHz > 108000) {
            throw OpenFmException("Frequency $frequencyKHz KHz is out of FM boundaries (87.5 - 108.0 MHz)", OpenFmException.ERROR_INVALID_FREQUENCY)
        }
        Log.i("OpenFmReceiver", "Tuning to frequency: $frequencyKHz KHz")
        return try {
            val result = FmNative.setFrequency(frequencyKHz)
            if (result == 0) {
                listener?.onFrequencyChanged(frequencyKHz)
                true
            } else {
                false
            }
        } catch (e: Exception) {
            Log.e("OpenFmReceiver", "Exception tuning frequency", e)
            false
        }
    }

    /**
     * Mutes or unmutes FM audio stream playback.
     */
    fun setMute(muted: Boolean): Boolean {
        return try {
            val result = FmNative.setMute(muted)
            result == 0
        } catch (e: Exception) {
            Log.e("OpenFmReceiver", "Exception setting mute status", e)
            false
        }
    }

    /**
     * Checks if the hardware receiver is currently muted.
     */
    fun isMuted(): Boolean {
        return try {
            FmNative.isMuted()
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Sets the region band configuration.
     */
    fun setBand(band: Int): Boolean {
        return try {
            val result = FmNative.setBand(band)
            result == 0
        } catch (e: Exception) {
            false
        }
    }

    /**
     * Checks if the power is active.
     */
    fun isPoweredOn(): Boolean = isPowerOn
}
