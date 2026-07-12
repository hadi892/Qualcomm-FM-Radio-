package com.example.fm.diagnostics

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import com.example.fm.FmNative
import com.example.fm.hal.HalRegistry
import com.example.fm.plugins.PluginManager
import java.io.File

/**
 * Diagnostic record representing the system health check results for FM subsystems.
 */
data class DiagnosticRecord(
    val hasRadioNode: Boolean,
    val radioNodePermissions: String,
    val hasBtfmslimNode: Boolean,
    val isHwSupportedByJni: Boolean,
    val activeHalType: String,
    val isHalLibFound: Boolean,
    val activePluginName: String,
    val activePluginVendor: String,
    val hasRecordAudioPermission: Boolean,
    val deviceModel: String,
    val androidVersion: String,
    val socModel: String,
    val selinuxBlocked: Boolean,
    val readableReport: String
)

/**
 * Executes a holistic platform sweep to capture current hardware permissions,
 * drivers, platform properties, and HAL access blocks.
 */
object OpenFmDiagnostics {

    fun executeAssessment(context: Context): DiagnosticRecord {
        val radioNode = File("/dev/radio0")
        val btfmslimNode = File("/dev/btfmslim")

        val hasRadio = radioNode.exists()
        val radioPermissions = if (hasRadio) {
            val r = if (radioNode.canRead()) "R" else "-"
            val w = if (radioNode.canWrite()) "W" else "-"
            "$r$w"
        } else {
            "NOT_FOUND"
        }

        val hasSlim = btfmslimNode.exists()
        
        // Dynamic HAL checking via our registry
        val activeHal = HalRegistry.detectActiveHal()
        val halTypeStr = activeHal.type.name

        val hasHalLib = File("/vendor/lib64/hw/vendor.qti.hardware.fm@1.0-impl.so").exists() ||
                        File("/vendor/lib64/vendor.qti.hardware.fm@1.0.so").exists()

        // Dynamic SoC plugin checking
        val activePlugin = PluginManager.getActivePlugin()

        // Local permission check
        val hasRecordAudio = context.checkCallingOrSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

        // Core JNI checks
        val jniHwSupported = try {
            FmNative.isHardwareSupported()
        } catch (e: Exception) {
            false
        }

        // SELinux policy blocks (often returns false for third party APKs even if files are physically present)
        val selinuxBlocked = jniHwSupported && !hasRadio

        val reportBuilder = StringBuilder()
        reportBuilder.append("=== OpenFM Diagnostic Health Assessment ===\n")
        reportBuilder.append("Device Model: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})\n")
        reportBuilder.append("SoC / Board: ${Build.HARDWARE} / ${Build.BOARD}\n")
        reportBuilder.append("Android Release: Version ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})\n")
        reportBuilder.append("\n--- Hardware Nodes & Drivers ---\n")
        reportBuilder.append("Kernel Node /dev/radio0: ${if (hasRadio) "FOUND (Permissions: $radioPermissions)" else "NOT PRESENT/RESTRICTED"}\n")
        reportBuilder.append("Kernel Node /dev/btfmslim: ${if (hasSlim) "FOUND" else "NOT PRESENT/RESTRICTED"}\n")
        reportBuilder.append("\n--- HAL & System Integrations ---\n")
        reportBuilder.append("Active Binder HAL Adapter: $halTypeStr\n")
        reportBuilder.append("Qualcomm FM Vendor Shared Library: ${if (hasHalLib) "DETECTED" else "NOT PRESENT"}\n")
        reportBuilder.append("Active Dynamic Vendor Plugin: ${activePlugin.name} (${activePlugin.vendorName})\n")
        reportBuilder.append("\n--- App Permissions & Sandbox ---\n")
        reportBuilder.append("RECORD_AUDIO Permission: ${if (hasRecordAudio) "GRANTED" else "MISSING"}\n")
        reportBuilder.append("SELinux Sandbox Context Block: ${if (selinuxBlocked) "YES (Kernel prevents direct HAL access)" else "NO (Platform-aligned)"}\n")
        reportBuilder.append("\n--- Summary Diagnostic Verdict ---\n")

        if (hasRadio && hasRecordAudio) {
            reportBuilder.append("VERDICT: Standard Linux FM hardware paths are open and functional.\n")
        } else if (hasHalLib && !hasRadio) {
            reportBuilder.append("VERDICT: Qualcomm FM hardware architecture is present, but missing /dev/radio0 driver links or blocked by Android SELinux kernel policies. Only pre-installed system signature apps can activate the receiver.\n")
        } else {
            reportBuilder.append("VERDICT: No active over-the-air FM tuner hardware components matched on this host firmware.\n")
        }

        return DiagnosticRecord(
            hasRadioNode = hasRadio,
            radioNodePermissions = radioPermissions,
            hasBtfmslimNode = hasSlim,
            isHwSupportedByJni = jniHwSupported,
            activeHalType = halTypeStr,
            isHalLibFound = hasHalLib,
            activePluginName = activePlugin.name,
            activePluginVendor = activePlugin.vendorName,
            hasRecordAudioPermission = hasRecordAudio,
            deviceModel = "${Build.MANUFACTURER} ${Build.MODEL}",
            androidVersion = "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
            socModel = Build.HARDWARE,
            selinuxBlocked = selinuxBlocked,
            readableReport = reportBuilder.toString()
        )
    }
}
