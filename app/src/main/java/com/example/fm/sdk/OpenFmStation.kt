package com.example.fm.sdk

/**
 * Data class representing an FM Radio Station with metadata and reception parameters.
 */
data class OpenFmStation(
    val frequencyKHz: Int,
    val name: String? = null,
    val rdsText: String? = null,
    val signalStrength: Int = 0,
    val isStereo: Boolean = true,
    val isFavorite: Boolean = false
) {
    val frequencyMHz: Double
        get() = frequencyKHz / 1000.0

    override fun toString(): String {
        return String.format("%.1f MHz%s", frequencyMHz, if (name != null) " - $name" else "")
    }
}
