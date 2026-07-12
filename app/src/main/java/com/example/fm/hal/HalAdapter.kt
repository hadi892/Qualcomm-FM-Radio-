package com.example.fm.hal

import android.util.Log
import java.io.File

/**
 * Interface representing a vendor hardware abstraction layer bridge.
 */
interface HalAdapter {
    val type: HalType
    fun isAvailable(): Boolean
}

enum class HalType {
    HIDL,
    AIDL,
    LEGACY_V4L2,
    UNAVAILABLE
}

/**
 * Concrete implementation representing HIDL-based vendor HAL interfaces.
 */
class HidlHalAdapter : HalAdapter {
    override val type: HalType = HalType.HIDL

    override fun isAvailable(): Boolean {
        // Look for typical vendor HIDL library endpoints or system properties
        val rc = File("/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so").exists() ||
                File("/vendor/lib64/vendor.qti.hardware.fm@1.0.so").exists()
        Log.i("HidlHalAdapter", "HIDL FM service implementation found in vendor: $rc")
        return rc
    }
}

/**
 * Concrete implementation representing AIDL-based vendor HAL interfaces.
 */
class AidlHalAdapter : HalAdapter {
    override val type: HalType = HalType.AIDL

    override fun isAvailable(): Boolean {
        // Look for newer AIDL-based service configs
        val rc = File("/vendor/etc/vintf/manifest").listFiles()?.any { file ->
            try {
                file.readText().contains("vendor.qti.hardware.fm")
            } catch (e: Exception) {
                false
            }
        } ?: false
        Log.i("AidlHalAdapter", "AIDL FM manifest definition found: $rc")
        return rc
    }
}

/**
 * Dynamic registry that runs discovery and auto-selects the available platform interface.
 */
object HalRegistry {
    private val adapters = listOf(
        AidlHalAdapter(),
        HidlHalAdapter()
    )

    fun detectActiveHal(): HalAdapter {
        val active = adapters.firstOrNull { it.isAvailable() }
        if (active != null) {
            Log.i("HalRegistry", "Detected active HAL adapter: ${active.type}")
            return active
        }
        
        Log.w("HalRegistry", "No available AIDL/HIDL FM interfaces matched. Falling back to V4L2/Legacy driver checks.")
        return object : HalAdapter {
            override val type: HalType = if (File("/dev/radio0").exists()) HalType.LEGACY_V4L2 else HalType.UNAVAILABLE
            override fun isAvailable(): Boolean = type == HalType.LEGACY_V4L2
        }
    }
}
