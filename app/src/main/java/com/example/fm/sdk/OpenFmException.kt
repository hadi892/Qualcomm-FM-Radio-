package com.example.fm.sdk

/**
 * Custom Exception thrown by the OpenFM Platform for all SDK-related and HAL failures.
 */
class OpenFmException(
    message: String,
    val errorCode: Int = ERROR_UNKNOWN,
    cause: Throwable? = null
) : Exception("OpenFM Error [$errorCode]: $message", cause) {

    companion object {
        const val ERROR_UNKNOWN = 1000
        const val ERROR_HARDWARE_UNAVAILABLE = 1001
        const val ERROR_PERMISSION_DENIED = 1002
        const val ERROR_HAL_COMMUNICATION_FAILURE = 1003
        const val ERROR_INVALID_FREQUENCY = 1004
        const val ERROR_MUTED = 1005
        const val ERROR_AUDIO_ROUTING_FAILED = 1006
        const val ERROR_SCANNING_IN_PROGRESS = 1007
        const val ERROR_SELINUX_BLOCKED = 1008
        const val ERROR_DRIVER_NODE_MISSING = 1009
    }
}
