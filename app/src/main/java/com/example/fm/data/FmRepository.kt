package com.example.fm.data

import kotlinx.coroutines.flow.Flow

class FmRepository(private val fmPresetDao: FmPresetDao) {
    val allPresets: Flow<List<FmPreset>> = fmPresetDao.getAllPresets()

    suspend fun insertPreset(preset: FmPreset) {
        fmPresetDao.insertPreset(preset)
    }

    suspend fun deletePresetByFreq(freqKHz: Int) {
        fmPresetDao.deletePresetByFreq(freqKHz)
    }

    suspend fun getPresetByFreq(freqKHz: Int): FmPreset? {
        return fmPresetDao.getPresetByFreq(freqKHz)
    }
}
