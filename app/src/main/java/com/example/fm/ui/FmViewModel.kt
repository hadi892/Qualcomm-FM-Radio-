package com.example.fm.ui

import android.app.Application
import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.example.fm.FmNative
import com.example.fm.data.FmPreset
import com.example.fm.data.FmRepository
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

class FmViewModel(
    application: Application,
    private val repository: FmRepository
) : AndroidViewModel(application) {

    private val audioManager = application.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    // UI state flows
    private val _isPowerOn = MutableStateFlow(false)
    val isPowerOn: StateFlow<Boolean> = _isPowerOn.asStateFlow()

    private val _currentFreqKHz = MutableStateFlow(98100) // Default 98.1 MHz
    val currentFreqKHz: StateFlow<Int> = _currentFreqKHz.asStateFlow()

    private val _isMuted = MutableStateFlow(false)
    val isMuted: StateFlow<Boolean> = _isMuted.asStateFlow()

    private val _rssi = MutableStateFlow(0)
    val rssi: StateFlow<Int> = _rssi.asStateFlow()

    private val _rdsText = MutableStateFlow<String?>(null)
    val rdsText: StateFlow<String?> = _rdsText.asStateFlow()

    private val _isScanning = MutableStateFlow(false)
    val isScanning: StateFlow<Boolean> = _isScanning.asStateFlow()

    private val _isHwSupported = MutableStateFlow(false)
    val isHwSupported: StateFlow<Boolean> = _isHwSupported.asStateFlow()

    private val _audioRoute = MutableStateFlow("Wired Headset")
    val audioRoute: StateFlow<String> = _audioRoute.asStateFlow()

    private val _headsetConnected = MutableStateFlow(false)
    val headsetConnected: StateFlow<Boolean> = _headsetConnected.asStateFlow()

    private val _diagnosticsReport = MutableStateFlow("Loading low-level JNI diagnostics report...")
    val diagnosticsReport: StateFlow<String> = _diagnosticsReport.asStateFlow()

    private val _lastPowerError = MutableStateFlow<String?>(null)
    val lastPowerError: StateFlow<String?> = _lastPowerError.asStateFlow()

    // Observe saved presets reactively from Room
    val savedPresets: StateFlow<List<FmPreset>> = repository.allPresets
        .stateIn(
            scope = viewModelScope,
            started = SharingStarted.WhileSubscribed(5000),
            initialValue = emptyList()
        )

    private var pollJob: Job? = null

    init {
        checkHardwareSupport()
        checkHeadsetConnection()
        refreshDiagnostics()
    }

    fun refreshDiagnostics() {
        viewModelScope.launch {
            try {
                _diagnosticsReport.value = FmNative.getDiagnosticsReport()
            } catch (e: Exception) {
                Log.e("FmViewModel", "Error fetching diagnostics", e)
                _diagnosticsReport.value = "Failed to run diagnostics: ${e.message}"
            }
        }
    }

    fun checkHardwareSupport() {
        viewModelScope.launch {
            try {
                _isHwSupported.value = FmNative.isHardwareSupported()
            } catch (e: Exception) {
                Log.e("FmViewModel", "Error checking hardware support", e)
                _isHwSupported.value = false
            }
        }
    }

    fun checkHeadsetConnection() {
        try {
            val devices = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS or AudioManager.GET_DEVICES_OUTPUTS)
            var connected = false
            for (device in devices) {
                if (device.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                    device.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
                ) {
                    connected = true
                    break
                }
            }
            _headsetConnected.value = connected
        } catch (e: Exception) {
            Log.e("FmViewModel", "Error checking headset connection", e)
            _headsetConnected.value = false
        }
    }

    fun clearPowerError() {
        _lastPowerError.value = null
    }

    fun togglePower() {
        viewModelScope.launch {
            if (_isPowerOn.value) {
                try {
                    FmNative.closeFm()
                } catch (e: UnsatisfiedLinkError) {
                    Log.e("FmViewModel", "Native closeFm UnsatisfiedLinkError", e)
                }
                _isPowerOn.value = false
                _rdsText.value = null
                _rssi.value = 0
                _lastPowerError.value = null
                stopPolling()
            } else {
                _lastPowerError.value = null
                try {
                    val res = FmNative.initFm()
                    if (res >= 0) {
                        _isPowerOn.value = true
                        _lastPowerError.value = null
                        val freq = FmNative.getFrequency()
                        if (freq in 87500..108000) {
                            _currentFreqKHz.value = freq
                        } else {
                            FmNative.setFrequency(_currentFreqKHz.value)
                        }
                        _isMuted.value = FmNative.isMuted()
                        startPolling()
                    } else {
                        Log.e("FmViewModel", "FM hardware init failed")
                        _lastPowerError.value = "FM Driver Init Failed: No accessible hardware interface found (No /dev/radio0 and no loadable QTI HAL libraries on this Samsung SoC)."
                        refreshDiagnostics()
                    }
                } catch (e: UnsatisfiedLinkError) {
                    Log.e("FmViewModel", "Native initFm UnsatisfiedLinkError", e)
                    _lastPowerError.value = "Native Link Error: JNI shared library symbols could not be resolved (${e.message})."
                    refreshDiagnostics()
                } catch (e: Exception) {
                    Log.e("FmViewModel", "Error turning FM on", e)
                    _lastPowerError.value = "Error: ${e.message}"
                    refreshDiagnostics()
                }
            }
        }
    }

    fun setFrequency(freqKHz: Int) {
        if (freqKHz !in 87500..108000) return
        viewModelScope.launch {
            _currentFreqKHz.value = freqKHz
            if (_isPowerOn.value) {
                try {
                    FmNative.setFrequency(freqKHz)
                } catch (e: UnsatisfiedLinkError) {
                    Log.e("FmViewModel", "Native setFrequency failure", e)
                }
                _rdsText.value = null
            }
        }
    }

    fun tuneStep(up: Boolean) {
        val step = 100 // 100 KHz spacing
        val current = _currentFreqKHz.value
        val next = if (up) current + step else current - step
        if (next in 87500..108000) {
            setFrequency(next)
        } else {
            if (up) setFrequency(87500) else setFrequency(108000)
        }
    }

    fun startScan(up: Boolean) {
        if (!_isPowerOn.value) return
        viewModelScope.launch {
            _isScanning.value = true
            _rdsText.value = null
            val direction = if (up) 1 else 0
            try {
                FmNative.startSearch(direction)
            } catch (e: UnsatisfiedLinkError) {
                Log.e("FmViewModel", "Native startSearch failure", e)
            }
            
            // Allow the hardware tuner up to 1.5 seconds to scan and lock
            delay(1500)
            try {
                val freq = FmNative.getFrequency()
                if (freq in 87500..108000) {
                    _currentFreqKHz.value = freq
                }
            } catch (e: UnsatisfiedLinkError) {
                Log.e("FmViewModel", "Native getFrequency failure during scan", e)
            }
            _isScanning.value = false
        }
    }

    fun toggleMute() {
        if (!_isPowerOn.value) return
        viewModelScope.launch {
            val nextMute = !_isMuted.value
            try {
                FmNative.setMute(nextMute)
                _isMuted.value = nextMute
            } catch (e: UnsatisfiedLinkError) {
                Log.e("FmViewModel", "Native setMute failure", e)
            }
        }
    }

    fun toggleAudioRoute() {
        try {
            if (_audioRoute.value.startsWith("Wired")) {
                audioManager.isSpeakerphoneOn = true
                _audioRoute.value = "Speaker"
            } else {
                audioManager.isSpeakerphoneOn = false
                _audioRoute.value = "Wired Headset"
            }
        } catch (e: Exception) {
            Log.e("FmViewModel", "Routing toggle failed", e)
        }
    }

    fun toggleFavorite() {
        val freq = _currentFreqKHz.value
        viewModelScope.launch {
            val existing = repository.getPresetByFreq(freq)
            if (existing != null) {
                repository.deletePresetByFreq(freq)
            } else {
                val freqMhz = freq / 1000f
                val label = "Station ${String.format("%.1f", freqMhz)} MHz"
                repository.insertPreset(FmPreset(freq, label, true))
            }
        }
    }

    fun selectPreset(preset: FmPreset) {
        setFrequency(preset.frequencyKHz)
    }

    private fun startPolling() {
        stopPolling()
        pollJob = viewModelScope.launch {
            while (true) {
                if (_isPowerOn.value) {
                    try {
                        _rssi.value = FmNative.getSignalStrength()
                        val rds = FmNative.getRdsData()
                        if (rds != null && rds != _rdsText.value) {
                            _rdsText.value = rds
                        }
                    } catch (e: UnsatisfiedLinkError) {
                        Log.e("FmViewModel", "Native polling failure", e)
                    }
                }
                delay(2000)
            }
        }
    }

    private fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
    }

    override fun onCleared() {
        super.onCleared()
        stopPolling()
        if (_isPowerOn.value) {
            try {
                FmNative.closeFm()
            } catch (e: UnsatisfiedLinkError) {
                Log.e("FmViewModel", "Native closeFm onCleared failure", e)
            }
        }
    }
}

class FmViewModelFactory(
    private val application: Application,
    private val repository: FmRepository
) : ViewModelProvider.Factory {
    override fun <T : ViewModel> create(modelClass: Class<T>): T {
        if (modelClass.isAssignableFrom(FmViewModel::class.java)) {
            @Suppress("UNCHECKED_CAST")
            return FmViewModel(application, repository) as T
        }
        throw IllegalArgumentException("Unknown ViewModel class")
    }
}
