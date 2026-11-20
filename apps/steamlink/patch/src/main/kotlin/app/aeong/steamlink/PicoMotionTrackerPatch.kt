package app.aeong.steamlink

import app.reseam.patch.CodeScope
import app.reseam.patch.PatchRuntime
import app.reseam.patch.addMethod
import app.reseam.patch.before
import app.reseam.patch.dex.AccessFlags
import app.reseam.patch.klass
import app.reseam.patch.method
import app.reseam.patch.patch
import app.aeong.steamlink.motiontracker.bindings.PicoMotionTrackerBridge

private const val STEAMLINK_PACKAGE = "com.valvesoftware.steamlinkvr"
private const val DEFAULT_UDP_PORT = 39575L
private const val LAYER_BUNDLE_PATH = "resources/steamlink/libXrApiLayer_pico_motion_tracker.so"
private const val LAYER_APK_PATH = "lib/arm64-v8a/libXrApiLayer_pico_motion_tracker.so"
private const val LAYER_MANIFEST_APK_PATH = "assets/openxr/1/api_layers/implicit.d/pico_motion_tracker.json"
private const val PICO_SWIFT_META_KEY = "PICO.swift.feature"

private val IPV4_REGEX = Regex("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})$")

private const val LAYER_MANIFEST_JSON = """{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_SEAMLESS_pico_motion_tracker",
    "library_path": "libXrApiLayer_pico_motion_tracker.so",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "Streams PICO Motion Tracker body and object poses to the Seamless SteamVR OpenVR driver",
    "disable_environment": "DISABLE_XR_APILAYER_SEAMLESS_PICO_MT"
  }
}
"""

private val vrLinkClass = klass("com.valvesoftware.steamlink.VRLink")

private data class MotionTrackerConfig(
    val host: String,
    val port: Int,
    val trackerPreset: String,
    val trackingMode: String,
)

private fun isValidIpv4(ip: String): Boolean {
    val match = IPV4_REGEX.matchEntire(ip) ?: return false
    return (1..4).all { match.groupValues[it].toIntOrNull() in 0..255 }
}

private fun PatchRuntime.enablePicoSwiftFeature() {
    manifest.edit {
        val application = findByTag("application").firstOrNull()
            ?: error("AndroidManifest.xml has no <application> element")
        val existing = application.children.firstOrNull {
            it.tag == "meta-data" && it["android:name"] == PICO_SWIFT_META_KEY
        }
        if (existing != null) {
            existing.setInt("android:value", 1)
        } else {
            val meta = createElement("meta-data")
            meta["android:name"] = PICO_SWIFT_META_KEY
            meta.setInt("android:value", 1)
            application.appendChild(meta)
        }
    }
}

private fun PatchRuntime.installOpenXrApiLayer() {
    files.copy(LAYER_BUNDLE_PATH, LAYER_APK_PATH)
    files.write(LAYER_MANIFEST_APK_PATH, LAYER_MANIFEST_JSON.toByteArray(Charsets.UTF_8))
}

private fun CodeScope.applyConfig(cfg: MotionTrackerConfig) {
    PicoMotionTrackerBridge.configure(
        thisObject,
        string(cfg.host),
        int(cfg.port),
        string(cfg.trackerPreset),
        string(cfg.trackingMode),
    )
}

val picoMotionTracker = patch(SteamLinkPatchStrings.picoMotionTracker) {
    description(SteamLinkPatchStrings.picoMotionTrackerDescription)
    compatibleWith(STEAMLINK_PACKAGE)

    val hostOption = stringOption(
        key = "host",
        title = SteamLinkPatchStrings.optHostTitle,
        description = SteamLinkPatchStrings.optHostDescription,
        default = "",
    )
    val portOption = intOption(
        key = "port",
        title = SteamLinkPatchStrings.optPortTitle,
        description = SteamLinkPatchStrings.optPortDescription,
        default = DEFAULT_UDP_PORT,
    )
    val trackerPresetOption = stringOption(
        key = "trackerPreset",
        title = SteamLinkPatchStrings.optTrackerPresetTitle,
        description = SteamLinkPatchStrings.optTrackerPresetDescription,
        default = "6pt",
        validValues = listOf("3pt", "6pt", "8pt", "10pt", "all"),
    )
    val trackingModeOption = stringOption(
        key = "trackingMode",
        title = SteamLinkPatchStrings.optTrackingModeTitle,
        description = SteamLinkPatchStrings.optTrackingModeDescription,
        default = "auto",
        validValues = listOf("auto", "body", "object"),
    )

    execute {
        val host = options[hostOption].trim()
        require(host.isEmpty() || isValidIpv4(host)) {
            "host must be a valid IPv4 address or blank for LAN auto-discovery (got '$host')"
        }
        val port = options[portOption].toInt()
        require(port in 1..65535) { "port must be between 1 and 65535 (got $port)" }

        val cfg = MotionTrackerConfig(
            host = host,
            port = port,
            trackerPreset = options[trackerPresetOption],
            trackingMode = options[trackingModeOption],
        )

        enablePicoSwiftFeature()
        installOpenXrApiLayer()

        // Write env/config before NativeActivity.onCreate loads libvrlink_scene.so and OpenXR.
        if (vrLinkClass.classDef.method("onCreate", "(Landroid/os/Bundle;)V") != null) {
            vrLinkClass.method("onCreate") { params("android.os.Bundle") }.before {
                applyConfig(cfg)
            }
        } else {
            vrLinkClass.addMethod("onCreate", "(Landroid/os/Bundle;)V", AccessFlags.PROTECTED) {
                applyConfig(cfg)
                thisObject.callSuper("android.app.NativeActivity", "onCreate", "(Landroid/os/Bundle;)V", param(0))
                returnVoid()
            }
        }

        log.info(
            "Installed PICO Motion Tracker OpenXR API layer (host=${cfg.host.ifEmpty { "auto" }}, " +
                "port=${cfg.port}, preset=${cfg.trackerPreset}, mode=${cfg.trackingMode})",
        )
    }
}
