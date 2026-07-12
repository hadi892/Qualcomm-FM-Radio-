package com.example.fm.plugins

import android.os.Build
import android.util.Log
import com.example.fm.FmNative

/**
 * Interface representing a vendor-specific hardware driver plugin.
 */
interface VendorPlugin {
    val name: String
    val vendorName: String

    fun isSupported(): Boolean
    fun initialize(): Int
    fun shutdown(): Int
    fun setFrequency(freqKHz: Int): Int
    fun getFrequency(): Int
    fun startSearch(direction: Int): Int
    fun cancelSearch(): Int
    fun getSignalStrength(): Int
    fun getRdsData(): String?
}

/**
 * Qualcomm-specific hardware driver plugin.
 */
class QualcommPlugin : VendorPlugin {
    override val name: String = "Qualcomm FM BSP Plugin"
    override val vendorName: String = "Qualcomm Technologies Inc."

    override fun isSupported(): Boolean {
        // Broad check for Qualcomm hardware / platforms
        val hardware = Build.HARDWARE.lowercase()
        val board = Build.BOARD.lowercase()
        return hardware.contains("qcom") || board.contains("msm") || board.contains("sdm") || board.contains("sm")
    }

    override fun initialize(): Int = FmNative.initFm()
    override fun shutdown(): Int = FmNative.closeFm()
    override fun setFrequency(freqKHz: Int): Int = FmNative.setFrequency(freqKHz)
    override fun getFrequency(): Int = FmNative.getFrequency()
    override fun startSearch(direction: Int): Int = FmNative.startSearch(direction)
    override fun cancelSearch(): Int = FmNative.cancelSearch()
    override fun getSignalStrength(): Int = FmNative.getSignalStrength()
    override fun getRdsData(): String? = FmNative.getRdsData()
}

/**
 * Samsung-specific hardware driver plugin (handling custom Samsung FM libraries).
 */
class SamsungPlugin : VendorPlugin {
    override val name: String = "Samsung Proprietary HAL Plugin"
    override val vendorName: String = "Samsung Electronics"

    override fun isSupported(): Boolean {
        return Build.MANUFACTURER.lowercase().contains("samsung")
    }

    override fun initialize(): Int {
        Log.w("SamsungPlugin", "Samsung custom HAL initialization requires platform signature")
        return FmNative.initFm()
    }
    override fun shutdown(): Int = FmNative.closeFm()
    override fun setFrequency(freqKHz: Int): Int = FmNative.setFrequency(freqKHz)
    override fun getFrequency(): Int = FmNative.getFrequency()
    override fun startSearch(direction: Int): Int = FmNative.startSearch(direction)
    override fun cancelSearch(): Int = FmNative.cancelSearch()
    override fun getSignalStrength(): Int = FmNative.getSignalStrength()
    override fun getRdsData(): String? = FmNative.getRdsData()
}

/**
 * MediaTek-specific hardware driver plugin.
 */
class MediaTekPlugin : VendorPlugin {
    override val name: String = "MediaTek FM HAL Plugin"
    override val vendorName: String = "MediaTek Inc."

    override fun isSupported(): Boolean {
        val hardware = Build.HARDWARE.lowercase()
        return hardware.contains("mtk") || hardware.contains("mediatek")
    }

    override fun initialize(): Int = -1 // Not supported on current hardware platform
    override fun shutdown(): Int = -1
    override fun setFrequency(freqKHz: Int): Int = -1
    override fun getFrequency(): Int = 0
    override fun startSearch(direction: Int): Int = -1
    override fun cancelSearch(): Int = -1
    override fun getSignalStrength(): Int = 0
    override fun getRdsData(): String? = null
}

/**
 * Broadcom-specific hardware driver plugin.
 */
class BroadcomPlugin : VendorPlugin {
    override val name: String = "Broadcom FM HCI Plugin"
    override val vendorName: String = "Broadcom Corp"

    override fun isSupported(): Boolean {
        val hardware = Build.HARDWARE.lowercase()
        return hardware.contains("bcm") || hardware.contains("brcm")
    }

    override fun initialize(): Int = -1 // Not supported on current hardware platform
    override fun shutdown(): Int = -1
    override fun setFrequency(freqKHz: Int): Int = -1
    override fun getFrequency(): Int = 0
    override fun startSearch(direction: Int): Int = -1
    override fun cancelSearch(): Int = -1
    override fun getSignalStrength(): Int = 0
    override fun getRdsData(): String? = null
}

/**
 * Fallback generic V4L2 kernel driver plugin.
 */
class GenericPlugin : VendorPlugin {
    override val name: String = "Standard Linux V4L2 Plugin"
    override val vendorName: String = "Linux Kernel Mainline"

    override fun isSupported(): Boolean = FmNative.isHardwareSupported()
    override fun initialize(): Int = FmNative.initFm()
    override fun shutdown(): Int = FmNative.closeFm()
    override fun setFrequency(freqKHz: Int): Int = FmNative.setFrequency(freqKHz)
    override fun getFrequency(): Int = FmNative.getFrequency()
    override fun startSearch(direction: Int): Int = FmNative.startSearch(direction)
    override fun cancelSearch(): Int = FmNative.cancelSearch()
    override fun getSignalStrength(): Int = FmNative.getSignalStrength()
    override fun getRdsData(): String? = FmNative.getRdsData()
}

/**
 * Plugin Registry & Manager responsible for dynamic vendor detection.
 */
object PluginManager {
    private val plugins = listOf(
        QualcommPlugin(),
        SamsungPlugin(),
        MediaTekPlugin(),
        BroadcomPlugin(),
        GenericPlugin()
    )

    fun getActivePlugin(): VendorPlugin {
        val active = plugins.firstOrNull { it.isSupported() }
        if (active != null) {
            Log.i("PluginManager", "Loaded active plugin: ${active.name} (${active.vendorName})")
            return active
        }
        
        Log.w("PluginManager", "No matching SoC vendor plugin discovered. Using fallback interface.")
        return object : VendorPlugin {
            override val name: String = "Generic Legacy Adapter"
            override val vendorName: String = "Unknown Vendor"
            override fun isSupported(): Boolean = false
            override fun initialize(): Int = -1
            override fun shutdown(): Int = -1
            override fun setFrequency(freqKHz: Int): Int = -1
            override fun getFrequency(): Int = 0
            override fun startSearch(direction: Int): Int = -1
            override fun cancelSearch(): Int = -1
            override fun getSignalStrength(): Int = 0
            override fun getRdsData(): String? = null
        }
    }
}
