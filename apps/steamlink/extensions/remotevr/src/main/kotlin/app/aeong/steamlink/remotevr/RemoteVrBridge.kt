@file:JvmName("RemoteVrBridge")

package app.aeong.steamlink.remotevr

import android.content.Context
import android.util.Log
import app.reseam.runtime.Export
import java.io.File

private const val TAG = "SteamLink-RemoteVR"
private const val NETWORK_ADDRESSES_FILE = "network_addresses.txt"
private const val MAX_SCAN_DEPTH = 4
private val INVALID_HOST_TOKENS = setOf("", "0.0.0.0", "0.0.0.0:0", "::", "[::]", "[::]:0")

@Export
fun configureHost(context: Context?, configuredHost: String?) {
    val host = configuredHost?.trim().orEmpty()
    if (host.isEmpty()) return
    val ipOnly = stripPort(host)
    if (ipOnly.isEmpty()) return

    try {
        val filesDir = context?.filesDir ?: return
        val addressFiles = findNetworkAddressFiles(filesDir)
        var updated = 0
        for (file in addressFiles) {
            if (ensureHostInFile(file, ipOnly)) {
                updated++
            }
        }
        Log.i(TAG, "Configured remote VR host=$ipOnly (updated $updated/${addressFiles.size} cached host file(s))")
    } catch (t: Throwable) {
        Log.w(TAG, "Failed to update cached host addresses for remote VR", t)
    }
}

@Export
fun rewriteStartVrArgs(context: Context?, rawArgs: String?, configuredHost: String?): String? {
    if (rawArgs == null) return null
    val tildeIdx = rawArgs.indexOf('~')
    if (tildeIdx < 0) return rawArgs

    return try {
        val originalPrefix = rawArgs.substring(0, tildeIdx)
        val suffix = rawArgs.substring(tildeIdx)
        val addresses = originalPrefix
            .split(';')
            .map { it.trim() }
            .filter { it.lowercase() !in INVALID_HOST_TOKENS }
            .toMutableList()

        val host = configuredHost?.trim().orEmpty()
        if (host.isNotEmpty()) {
            addresses.removeAll { it.equals(host, ignoreCase = true) }
            // SVLRoot::SVLRoot() selects the LAST entry in UDPServerAddresses as primary.
            addresses.add(host)
        } else {
            if (addresses.isEmpty()) {
                addresses.addAll(collectCachedAddresses(context?.filesDir))
            }
            if (addresses.size > 1) {
                // SVLRoot::SVLRoot() uses the LAST entry as primary (*). Put real LAN/VPN IPs
                // (192.168.0.0/16, 10.0.0.0/8) last so public WAN IPs or WSL/Docker 172.16-31.x.x
                // virtual adapters do not shadow the reachable PC LAN IP over WireGuard.
                addresses.sortBy { addressPriority(it) }
            }
        }

        val newPrefix = addresses.joinToString(";")
        if (newPrefix != originalPrefix) {
            Log.i(TAG, "Rewrote VRLink target addresses: '$originalPrefix' -> '$newPrefix'")
        } else {
            Log.i(TAG, "Launching VRLink with target addresses: '$newPrefix'")
        }
        newPrefix + suffix
    } catch (t: Throwable) {
        Log.w(TAG, "Failed to rewrite VRLink start args; using original", t)
        rawArgs
    }
}

private fun findNetworkAddressFiles(root: File): List<File> {
    val results = mutableListOf<File>()
    fun walk(dir: File, depth: Int) {
        if (depth > MAX_SCAN_DEPTH) return
        val children = dir.listFiles() ?: return
        for (child in children) {
            if (child.isFile && child.name == NETWORK_ADDRESSES_FILE) {
                results.add(child)
            } else if (child.isDirectory) {
                walk(child, depth + 1)
            }
        }
    }
    walk(root, 0)
    results.sortBy { it.lastModified() }
    return results
}

private fun ensureHostInFile(file: File, hostIp: String): Boolean {
    val existingLines = try {
        file.readLines()
    } catch (_: Throwable) {
        return false
    }
    val alreadyPresent = existingLines.any { line ->
        stripPort(line.trim()).equals(hostIp, ignoreCase = true)
    }
    if (alreadyPresent) return false

    val updatedText = buildString {
        for (line in existingLines) {
            val trimmed = line.trim()
            if (trimmed.isNotEmpty()) {
                append(trimmed).append('\n')
            }
        }
        append(hostIp).append('\n')
    }
    val tmp = File(file.parentFile, "$NETWORK_ADDRESSES_FILE.tmp")
    tmp.writeText(updatedText)
    if (!tmp.renameTo(file)) {
        file.writeText(updatedText)
        tmp.delete()
    }
    return true
}

private fun collectCachedAddresses(filesDir: File?): List<String> {
    if (filesDir == null) return emptyList()
    val seen = LinkedHashSet<String>()
    for (file in findNetworkAddressFiles(filesDir)) {
        val lines = try {
            file.readLines()
        } catch (_: Throwable) {
            continue
        }
        for (line in lines) {
            val ip = stripPort(line.trim())
            if (ip.isNotEmpty() && ip.lowercase() !in INVALID_HOST_TOKENS) {
                seen.add(ip)
            }
        }
    }
    return seen.toList()
}

private fun addressPriority(raw: String): Int {
    val ip = stripPort(raw.trim())
    val parts = ip.split('.')
    if (parts.size != 4) return 0 // IPv6 or unknown
    val a = parts[0].toIntOrNull() ?: return 0
    val b = parts[1].toIntOrNull() ?: return 0
    return when {
        a == 192 && b == 168 -> 3 // Home LAN (highest preference -> last in list)
        a == 10 -> 3 // Private LAN / WireGuard subnet
        a == 100 && b in 64..127 -> 2 // CGNAT / Tailscale / overlay VPN
        a == 172 && b in 16..31 -> 1 // Often WSL2/Hyper-V/Docker virtual bridge on Windows
        else -> 0 // Public WAN IP (lowest preference -> first in list)
    }
}

private fun stripPort(raw: String): String {
    if (raw.isEmpty()) return ""
    // Handle bracketed IPv6 "[addr]:port" or "[addr]"
    if (raw.startsWith("[")) {
        val close = raw.indexOf(']')
        if (close > 1) return raw.substring(1, close)
        return raw
    }
    // Plain IPv6 without brackets contains multiple colons
    val firstColon = raw.indexOf(':')
    val lastColon = raw.lastIndexOf(':')
    if (firstColon != -1 && firstColon == lastColon) {
        return raw.substring(0, firstColon)
    }
    val comma = raw.indexOf(',')
    if (comma > 0) {
        return raw.substring(0, comma)
    }
    return raw
}
