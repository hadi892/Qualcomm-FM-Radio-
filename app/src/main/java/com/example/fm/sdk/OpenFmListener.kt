package com.example.fm.sdk

/**
 * Interface to receive asynchronous callbacks about real-time hardware status and updates.
 */
interface OpenFmListener {
    /**
     * Called when the FM radio receiver powers on or off.
     */
    fun onPowerStatusChanged(isPowerOn: Boolean)

    /**
     * Called when the tuned station frequency changes.
     */
    fun onFrequencyChanged(frequencyKHz: Int)

    /**
     * Called when new RDS metadata is parsed.
     */
    fun onRdsUpdated(rds: OpenFmRds)

    /**
     * Called when the received signal strength (RSSI) changes.
     * @param signalStrength Value from 0 to 100.
     */
    fun onSignalStrengthChanged(signalStrength: Int)

    /**
     * Called when an automated scan (seek) completes, or fails with an exception.
     */
    fun onScanFinished(success: Boolean, station: OpenFmStation?)

    /**
     * Called when the active audio output path changes (e.g., Speaker vs. Wired Headset).
     */
    fun onAudioRouteChanged(route: String)

    /**
     * Called when a system error occurs during background execution.
     */
    fun onError(exception: OpenFmException)
}
