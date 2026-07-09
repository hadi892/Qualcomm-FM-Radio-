package com.example.fm

import android.util.Log

object FmNative {
    init {
        try {
            System.loadLibrary("fm_jni")
            Log.i("FmNative", "Successfully loaded native fm_jni library")
        } catch (e: UnsatisfiedLinkError) {
            Log.e("FmNative", "Failed to load native fm_jni library. This is expected if NDK compile has not run or running on non-Android platform.", e)
        }
    }

    /**
     * Checks if the device possesses physical Qualcomm FM Radio receiver hardware
     * or a standard Linux kernel V4L2 FM driver (/dev/radio0).
     */
    external fun isHardwareSupported(): Boolean

    /**
     * Initializes the Qualcomm FM hardware or `/dev/radio0` driver via HAL.
     * @return 0 on success, negative values on error (e.g., -1 for hardware not accessible).
     */
    external fun initFm(): Int

    /**
     * Shuts down the Qualcomm FM HAL and closes any driver/hardware handles.
     */
    external fun closeFm(): Int

    /**
     * Tunes the FM receiver to a specific station frequency in KHz.
     * E.g., 98.1 MHz is passed as 98100.
     * @return 0 on success, negative values on error.
     */
    external fun setFrequency(freqKHz: Int): Int

    /**
     * Gets the currently tuned hardware frequency in KHz.
     */
    external fun getFrequency(): Int

    /**
     * Starts an automatic frequency scan (seek) on the physical receiver.
     * @param direction 0 for seek down, 1 for seek up.
     * @return 0 on success, negative values on error.
     */
    external fun startSearch(direction: Int): Int

    /**
     * Cancels any active hardware station scan.
     */
    external fun cancelSearch(): Int

    /**
     * Queries current RDS (Radio Data System) text or Program Service (PS) info.
     * @return Decoded RDS string if available, null otherwise.
     */
    external fun getRdsData(): String?

    /**
     * Checks if the hardware FM audio output is currently muted.
     */
    external fun isMuted(): Boolean

    /**
     * Mutes or unmutes the hardware FM receiver audio stream.
     * @return 0 on success, negative values on error.
     */
    external fun setMute(mute: Boolean): Int

    /**
     * Queries the received signal strength indicator (RSSI) of the current station.
     * @return Signal strength percentage (0-100) or dBuV indicator.
     */
    external fun getSignalStrength(): Int

    /**
     * Sets the FM band frequency region.
     * @param band 0 for US/EU (87.5 - 108.0 MHz), 1 for Japan, 2 for other.
     * @return 0 on success, negative values on error.
     */
    external fun setBand(band: Int): Int
}
