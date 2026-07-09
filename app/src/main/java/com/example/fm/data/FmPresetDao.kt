package com.example.fm.data

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

@Dao
interface FmPresetDao {
    @Query("SELECT * FROM fm_presets ORDER BY frequencyKHz ASC")
    fun getAllPresets(): Flow<List<FmPreset>>

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertPreset(preset: FmPreset)

    @Query("DELETE FROM fm_presets WHERE frequencyKHz = :frequencyKHz")
    suspend fun deletePresetByFreq(frequencyKHz: Int)

    @Query("SELECT * FROM fm_presets WHERE frequencyKHz = :frequencyKHz LIMIT 1")
    suspend fun getPresetByFreq(frequencyKHz: Int): FmPreset?
}
