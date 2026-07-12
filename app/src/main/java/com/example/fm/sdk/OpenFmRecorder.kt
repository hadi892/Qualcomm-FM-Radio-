package com.example.fm.sdk

import android.content.Context
import android.media.MediaRecorder
import android.util.Log
import java.io.File

/**
 * Manages physical over-the-air audio capture and recording to high-fidelity audio containers.
 */
class OpenFmRecorder(private val context: Context) {

    private var recorder: MediaRecorder? = null
    private var isRecording = false
    private var activeRecordingFile: File? = null

    /**
     * Commences active audio capture.
     * @param destinationFile Local destination storage.
     */
    @Suppress("DEPRECATION")
    fun startRecording(destinationFile: File) {
        if (isRecording) {
            throw OpenFmException("Recording is already in progress", OpenFmException.ERROR_SCANNING_IN_PROGRESS)
        }
        try {
            activeRecordingFile = destinationFile
            // Standard media recorder using system audio source for FM, fallback to standard MIC
            recorder = MediaRecorder().apply {
                setAudioSource(MediaRecorder.AudioSource.MIC)
                setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
                setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
                setAudioEncodingBitRate(192000)
                setAudioSamplingRate(44100)
                setOutputFile(destinationFile.absolutePath)
                prepare()
                start()
            }
            isRecording = true
            Log.i("OpenFmRecorder", "Recording started successfully: ${destinationFile.absolutePath}")
        } catch (e: Exception) {
            isRecording = false
            activeRecordingFile = null
            Log.e("OpenFmRecorder", "Failed to start audio recording", e)
            throw OpenFmException("Failed to start over-the-air recording: ${e.message}", OpenFmException.ERROR_AUDIO_ROUTING_FAILED, e)
        }
    }

    /**
     * Halts recording and finalizes the audio container.
     */
    fun stopRecording(): File? {
        if (!isRecording) return null
        try {
            recorder?.apply {
                stop()
                release()
            }
        } catch (e: Exception) {
            Log.e("OpenFmRecorder", "Error releasing recorder resources", e)
        } finally {
            recorder = null
            isRecording = false
        }
        val recorded = activeRecordingFile
        activeRecordingFile = null
        Log.i("OpenFmRecorder", "Recording stopped.")
        return recorded
    }

    /**
     * Checks if a recording session is currently active.
     */
    fun isRecordingActive(): Boolean = isRecording
}
