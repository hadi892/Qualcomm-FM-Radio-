package com.example.fm.data

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "fm_presets")
data class FmPreset(
    @PrimaryKey val frequencyKHz: Int,
    val stationName: String,
    val isFavorite: Boolean = false,
    val timestamp: Long = System.currentTimeMillis()
) {
    val frequencyMhz: Float
        get() = frequencyKHz / 1000f
}
