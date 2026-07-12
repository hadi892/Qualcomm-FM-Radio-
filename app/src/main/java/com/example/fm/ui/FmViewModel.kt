package com.example.fm.ui

import android.app.Application
import android.content.Context
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.example.fm.data.FmPreset
import com.example.fm.data.FmRepository
import com.example.fm.diagnostics.OpenFmDiagnostics
import com.example.fm.sdk.OpenFmException
import com.example.fm.sdk.OpenFmListener
import com.example.fm.sdk.OpenFmManager
import com.example.fm.sdk.OpenFmRds
import com.example.fm.sdk.OpenFmSession
import com.example.fm.sdk.OpenFmStation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
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

    private val openFmManager = OpenFmManager(application)
    private var openFmSession: OpenFmSession? = null

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
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val report = OpenFmDiagnostics.executeAssessment(getApplication()).readableReport
                _diagnosticsReport.value = report
            } catch (e: Throwable) {
                Log.e("FmViewModel", "Error fetching diagnostics", e)
                _diagnosticsReport.value = "Failed to run diagnostics: ${e.message}"
            }
        }
    }

    fun checkHardwareSupport() {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val supported = openFmManager.isHardwareSupported()
                _isHwSupported.value = supported
            } catch (e: Throwable) {
                Log.e("FmViewModel", "Error checking hardware support", e)
                _isHwSupported.value = false
            }
        }
    }

    fun checkHeadsetConnection() {
        try {
            val session = openFmSession
            val connected = if (session != null) {
                session.audio.isHeadsetConnected()
            } else {
                // Fallback local check
                val audioManager = getApplication<Application>().getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
                @Suppress("DEPRECATION")
                audioManager.isWiredHeadsetOn
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
        viewModelScope.launch(Dispatchers.IO) {
            if (_isPowerOn.value) {
                try {
                    openFmSession?.stop()
                } catch (e: Throwable) {
                    Log.e("FmViewModel", "SDK Session stop failure", e)
                }
                openFmSession = null
                _isPowerOn.value = false
                _rdsText.value = null
                _rssi.value = 0
                _lastPowerError.value = null
                stopPolling()
            } else {
                _lastPowerError.value = null
                try {
                    val session = openFmManager.openSession(object : OpenFmListener {
                        override fun onPowerStatusChanged(isPowerOn: Boolean) {
                            _isPowerOn.value = isPowerOn
                        }

                        override fun onFrequencyChanged(frequencyKHz: Int) {
                            _currentFreqKHz.value = frequencyKHz
                        }

                        override fun onRdsUpdated(rds: OpenFmRds) {
                            _rdsText.value = rds.rt ?: rds.ps
                        }

                        override fun onSignalStrengthChanged(signalStrength: Int) {
                            _rssi.value = signalStrength
                        }

                        override fun onScanFinished(success: Boolean, station: OpenFmStation?) {
                            _isScanning.value = false
                            if (success && station != null) {
                                _currentFreqKHz.value = station.frequencyKHz
                            }
                        }

                        override fun onAudioRouteChanged(route: String) {
                            _audioRoute.value = route
                        }

                        override fun onError(exception: OpenFmException) {
                            _lastPowerError.value = exception.message
                        }
                    })

                    openFmSession = session
                    _isPowerOn.value = true
                    _lastPowerError.value = null
                    
                    // Inquire initial tuned freq
                    val initialFreq = session.receiver.isMuted() // probe
                    session.receiver.tune(_currentFreqKHz.value)
                    _isMuted.value = session.receiver.isMuted()
                    _audioRoute.value = session.audio.getActiveRoute()
                    startPolling()
                } catch (e: Throwable) {
                    Log.e("FmViewModel", "Error turning FM on via SDK", e)
                    _lastPowerError.value = "OpenFM SDK Session Failed: " + (e.message ?: "Tuner hardware or custom libraries are restricted on this firmware.")
                    refreshDiagnostics()
                }
            }
        }
    }

    fun setFrequency(freqKHz: Int) {
        if (freqKHz !in 87500..108000) return
        viewModelScope.launch(Dispatchers.IO) {
            _currentFreqKHz.value = freqKHz
            if (_isPowerOn.value) {
                try {
                    openFmSession?.receiver?.tune(freqKHz)
                } catch (e: Throwable) {
                    Log.e("FmViewModel", "SDK tune failure", e)
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
        viewModelScope.launch(Dispatchers.IO) {
            _isScanning.value = true
            _rdsText.value = null
            try {
                openFmSession?.scanner?.seek(if (up) 1 else 0)
            } catch (e: Throwable) {
                Log.e("FmViewModel", "SDK Scanner seek failure", e)
                _isScanning.value = false
            }
        }
    }

    fun toggleMute() {
        if (!_isPowerOn.value) return
        viewModelScope.launch(Dispatchers.IO) {
            val nextMute = !_isMuted.value
            try {
                val success = openFmSession?.receiver?.setMute(nextMute) ?: false
                if (success) {
                    _isMuted.value = nextMute
                }
            } catch (e: Throwable) {
                Log.e("FmViewModel", "SDK setMute failure", e)
            }
        }
    }

    fun toggleAudioRoute() {
        val session = openFmSession ?: return
        try {
            val current = session.audio.getActiveRoute()
            val next = if (current.equals("SPEAKER", ignoreCase = true)) "HEADSET" else "SPEAKER"
            if (session.audio.setAudioRoute(next)) {
                _audioRoute.value = session.audio.getActiveRoute()
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
        pollJob = viewModelScope.launch(Dispatchers.IO) {
            while (true) {
                if (_isPowerOn.value) {
                    try {
                        val session = openFmSession
                        if (session != null) {
                            _rssi.value = com.example.fm.FmNative.getSignalStrength()
                            val rdsTextStr = com.example.fm.FmNative.getRdsData()
                            if (rdsTextStr != null && rdsTextStr != _rdsText.value) {
                                _rdsText.value = rdsTextStr
                            }
                        }
                    } catch (e: Throwable) {
                        Log.e("FmViewModel", "Native polling failure", e)
                    }
                }
                delay(1000)
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
            CoroutineScope(Dispatchers.IO).launch {
                try {
                    openFmSession?.stop()
                } catch (e: Throwable) {
                    Log.e("FmViewModel", "Native closeFm onCleared failure", e)
                }
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
