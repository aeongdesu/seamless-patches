@file:JvmName("PicoMotionTrackerBridge")

package app.aeong.steamlink.motiontracker

import android.content.Context
import android.system.Os
import android.util.Log
import app.reseam.runtime.Export
import java.io.File

private const val TAG = "PicoMT-Layer"
private const val DEFAULT_UDP_PORT = 39575
private const val CONFIG_FILE_NAME = "pico_motion_tracker.conf"
private val VALID_PRESETS = setOf("3pt", "6pt", "8pt", "10pt", "all")
private val VALID_MODES = setOf("auto", "body", "object")

@Export
fun configure(
    context: Context?,
    configuredHost: String?,
    port: Int,
    trackerPreset: String?,
    trackingMode: String?,
) {
    try {
        val host = configuredHost?.trim().orEmpty()
        val safePort = if (port in 1..65535) port else DEFAULT_UDP_PORT
        val safePreset = trackerPreset?.trim()?.lowercase()?.takeIf { it in VALID_PRESETS } ?: "6pt"
        val safeMode = trackingMode?.trim()?.lowercase()?.takeIf { it in VALID_MODES } ?: "auto"

        writeConfigFile(context, host, safePort, safePreset, safeMode)

        setEnvSafe("PICO_MT_ENABLED", "1")
        if (host.isNotEmpty()) setEnvSafe("PICO_MT_HOST", host) else unsetEnvSafe("PICO_MT_HOST")
        setEnvSafe("PICO_MT_PORT", safePort.toString())
        setEnvSafe("PICO_MT_TRACKER_PRESET", safePreset)
        setEnvSafe("PICO_MT_TRACKING_MODE", safeMode)

        Log.i(TAG, "Configured PICO Motion Tracker: host=${host.ifEmpty { "<auto>" }} port=$safePort preset=$safePreset mode=$safeMode")
    } catch (t: Throwable) {
        Log.w(TAG, "Failed to apply PICO Motion Tracker config", t)
    }
}

private fun writeConfigFile(context: Context?, host: String, port: Int, preset: String, mode: String) {
    val filesDir = context?.filesDir ?: return
    val configFile = File(filesDir, CONFIG_FILE_NAME)
    val content = buildString {
        append("enabled=1\n")
        append("host=").append(host).append('\n')
        append("port=").append(port).append('\n')
        append("tracker_preset=").append(preset).append('\n')
        append("tracking_mode=").append(mode).append('\n')
    }
    val tmpFile = File(filesDir, "$CONFIG_FILE_NAME.tmp")
    tmpFile.writeText(content)
    if (!tmpFile.renameTo(configFile)) {
        configFile.writeText(content)
        tmpFile.delete()
    }
    setEnvSafe("PICO_MT_CONFIG_PATH", configFile.absolutePath)
}

private fun setEnvSafe(name: String, value: String) {
    try {
        Os.setenv(name, value, true)
    } catch (t: Throwable) {
        Log.w(TAG, "Os.setenv($name) failed", t)
    }
}

private fun unsetEnvSafe(name: String) {
    try {
        Os.unsetenv(name)
    } catch (_: Throwable) {
    }
}
