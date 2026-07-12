package com.example.fm.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import com.example.MainActivity
import com.example.fm.sdk.OpenFmException
import com.example.fm.sdk.OpenFmListener
import com.example.fm.sdk.OpenFmManager
import com.example.fm.sdk.OpenFmRds
import com.example.fm.sdk.OpenFmSession
import com.example.fm.sdk.OpenFmStation

/**
 * Binder-based Android Service that executes background FM processing,
 * manages foreground notifications, and wraps safety checking protocols.
 */
class OpenFmService : Service(), OpenFmListener {

    private val binder = LocalBinder()
    private lateinit var openFmManager: OpenFmManager
    private var activeSession: OpenFmSession? = null
    private val clients = mutableSetOf<OpenFmListener>()

    companion object {
        private const val CHANNEL_ID = "openfm_playback_channel"
        private const val NOTIFICATION_ID = 40401
    }

    inner class LocalBinder : Binder() {
        fun getService(): OpenFmService = this@OpenFmService
    }

    override fun onCreate() {
        super.onCreate()
        Log.i("OpenFmService", "Initializing OpenFM Service platform layer")
        openFmManager = OpenFmManager(applicationContext)
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i("OpenFmService", "OpenFM Service command trigger received")
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i("OpenFmService", "Client bound to OpenFM Service")
        return binder
    }

    override fun onUnbind(intent: Intent?): Boolean {
        Log.i("OpenFmService", "Client unbound from OpenFM Service")
        return super.onUnbind(intent)
    }

    /**
     * Registers a callback listener to receive background over-the-air tuner updates.
     */
    fun registerListener(listener: OpenFmListener) {
        clients.add(listener)
    }

    /**
     * Unregisters a callback listener.
     */
    fun unregisterListener(listener: OpenFmListener) {
        clients.remove(listener)
    }

    /**
     * Attempts to acquire the active session. If none exists, creates it.
     */
    @Synchronized
    fun acquireSession(): OpenFmSession {
        // Double check permission constraints
        if (checkCallingOrSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            throw OpenFmException("RECORD_AUDIO permission is required to capture radio audio loops.", OpenFmException.ERROR_PERMISSION_DENIED)
        }

        val session = activeSession ?: openFmManager.openSession(this)
        activeSession = session
        
        // Elevate service to Android Foreground so background reception isn't suspended by Doze/OOM
        startForeground(NOTIFICATION_ID, buildPlaybackNotification(87500))
        return session
    }

    /**
     * Shuts down the current active FM reception session cleanly.
     */
    @Synchronized
    fun releaseSession() {
        activeSession?.stop()
        activeSession = null
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    /**
     * Check if a session is currently running.
     */
    fun isSessionActive(): Boolean = activeSession?.isActive() ?: false

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val name = "OpenFM Active Reception"
            val descriptionText = "Manages active over-the-air FM playback and controls"
            val importance = NotificationManager.IMPORTANCE_LOW
            val channel = NotificationChannel(CHANNEL_ID, name, importance).apply {
                description = descriptionText
            }
            val notificationManager: NotificationManager =
                getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }

    private fun buildPlaybackNotification(frequencyKHz: Int): Notification {
        val pendingIntent: PendingIntent = Intent(this, MainActivity::class.java).let { notificationIntent ->
            PendingIntent.getActivity(this, 0, notificationIntent, PendingIntent.FLAG_IMMUTABLE)
        }

        val mhz = frequencyKHz / 1000.0
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("OpenFM Radio Active")
            .setContentText(String.format("Tuned to %.1f MHz", mhz))
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    // Forwarding Listener Events to all bound Clients
    override fun onPowerStatusChanged(isPowerOn: Boolean) {
        clients.forEach { it.onPowerStatusChanged(isPowerOn) }
    }

    override fun onFrequencyChanged(frequencyKHz: Int) {
        // Update Foreground Notification content
        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.notify(NOTIFICATION_ID, buildPlaybackNotification(frequencyKHz))
        clients.forEach { it.onFrequencyChanged(frequencyKHz) }
    }

    override fun onRdsUpdated(rds: OpenFmRds) {
        clients.forEach { it.onRdsUpdated(rds) }
    }

    override fun onSignalStrengthChanged(signalStrength: Int) {
        clients.forEach { it.onSignalStrengthChanged(signalStrength) }
    }

    override fun onScanFinished(success: Boolean, station: OpenFmStation?) {
        clients.forEach { it.onScanFinished(success, station) }
    }

    override fun onAudioRouteChanged(route: String) {
        clients.forEach { it.onAudioRouteChanged(route) }
    }

    override fun onError(exception: OpenFmException) {
        clients.forEach { it.onError(exception) }
    }

    override fun onDestroy() {
        releaseSession()
        super.onDestroy()
    }
}
