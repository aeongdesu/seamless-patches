package app.aeong.steamlink

import app.aeong.steamlink.remotevr.bindings.RemoteVrBridge
import app.reseam.patch.PatchRuntime
import app.reseam.patch.before
import app.reseam.patch.klass
import app.reseam.patch.method
import app.reseam.patch.patch

private const val STEAMLINK_VR_PACKAGE = "com.valvesoftware.steamlinkvr"
private const val LIB_SHELL_PATH = "lib/arm64-v8a/libshell_arm64-v8a.so"
private const val LIB_VRLINK_PATH = "lib/arm64-v8a/libvrlink_scene.so"

private const val SYM_ON_UPDATE_CONNECT_BUTTON = "_ZN10CPanelMain26OnUpdateConnectButtonStateEv"
private const val SYM_GET_ADDRESS = "_ZN14CServerManager10GetAddressEy"
private const val SYM_HAS_IP = "_ZNK10CIPAddress5HasIPEv"
private const val LOCAL_NETWORK_ONLY_MSG = "Streaming VR is only supported on the local network"

private const val SYM_GUESS_BEST_PEER =
    "_ZN15SVLDataLinkUber30GuessBestCorrespondenceAddressER16sockaddr_storageRtPKS0_i"
private const val SYM_ECHO_SEARCH =
    "_ZN22SVLDataLinkTransferUDP33EchoSearchCorrespondenceAddressesERK16sockaddr_storageitPKhi"
private const val SYM_UDP_RESTART = "_ZN22SVLDataLinkTransferUDP7RestartEv"
private const val SYM_MUTEX_UNLOCK = "_ZNSt6__ndk115recursive_mutex6unlockEv"

private const val SOL_SOCKET = 1
private const val SO_BINDTODEVICE = 25

private const val COND_NE = 0x1
private const val COND_LT = 0xb

private val HOST_IPV4_REGEX = Regex("^(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})([:,]\\d{1,5})?$")

private val steamLinkClass = klass("com.valvesoftware.steamlink.SteamLink")
private val steamLinkOnCreate = method {
    inClass(steamLinkClass)
    name("onCreate")
    params("android.os.Bundle")
}
private val steamLinkStartVrLink = method {
    inClass(steamLinkClass)
    name("startVRLink")
    params("java.lang.String")
}

private fun isValidIpv6Host(host: String): Boolean {
    val addr: String
    if (host.startsWith('[')) {
        val close = host.indexOf(']')
        if (close <= 1) return false
        addr = host.substring(1, close)
        val rest = host.substring(close + 1)
        if (rest.isNotEmpty()) {
            if ((rest[0] != ':' && rest[0] != ',') || rest.length < 2) return false
            if (rest.substring(1).toIntOrNull() !in 1..65535) return false
        }
    } else {
        addr = host
    }
    return addr.contains(':') && addr.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' || it == ':' }
}

private fun isValidHostAddress(host: String): Boolean {
    val v4 = HOST_IPV4_REGEX.matchEntire(host)
    if (v4 != null) {
        val octetsOk = (1..4).all { v4.groupValues[it].toIntOrNull() in 0..255 }
        val portSuffix = v4.groupValues[5]
        val portOk = portSuffix.isEmpty() || portSuffix.substring(1).toIntOrNull() in 1..65535
        return octetsOk && portOk
    }
    return isValidIpv6Host(host)
}

private fun PatchRuntime.shouldStoreNativeLibsUncompressed(): Boolean =
    manifest.document().use { doc ->
        val app = doc.findByTag("application").firstOrNull()
        app?.get("android:extractNativeLibs")?.equals("false", ignoreCase = true) == true
    }

private fun computeShellLocalNetworkPatches(elf: Elf64Arm64Image): List<ElfBytePatch> {
    val fn = elf.requireSymbol(SYM_ON_UPDATE_CONNECT_BUTTON)
    require(fn.size in 64L..16384L) { "Unexpected size ${fn.size} for $SYM_ON_UPDATE_CONNECT_BUTTON" }
    val code = elf.readBytesAtVaddr(fn.vaddr, fn.size.toInt())
    val insnCount = code.size / 4

    val matches = mutableListOf<Long>()
    for (idx in 0 until insnCount) {
        val pc = fn.vaddr + idx * 4L
        val target = decodeBlTarget(pc, readI32LE(code, idx * 4)) ?: continue
        if (elf.resolveCallTargetSymbol(target) != SYM_GET_ADDRESS) continue

        // Expect HasIP() within the next 4 instructions.
        var hasIpIdx = -1
        for (k in (idx + 1) until minOf(insnCount, idx + 5)) {
            val kTarget = decodeBlTarget(fn.vaddr + k * 4L, readI32LE(code, k * 4)) ?: continue
            if (elf.resolveCallTargetSymbol(kTarget) == SYM_HAS_IP) {
                hasIpIdx = k
                break
            }
        }
        if (hasIpIdx < 0) continue

        // Expect TBZ wN, #0, <localNetworkOnlyBlock> within 8 instructions after HasIP().
        for (k in (hasIpIdx + 1) until minOf(insnCount, hasIpIdx + 9)) {
            val kPc = fn.vaddr + k * 4L
            val branchTarget = decodeTbz(kPc, readI32LE(code, k * 4), expectedBit = 0) ?: continue
            val targetIdx = ((branchTarget - fn.vaddr) / 4L).toInt()
            if (targetIdx !in 0 until insnCount) continue

            val referencedAddrs = findAdrpAddTargets(code, fn.vaddr, targetIdx, targetIdx + 8)
            val hitsLocalNetworkMsg = referencedAddrs.any { addr ->
                elf.readCStringAtVaddr(addr, LOCAL_NETWORK_ONLY_MSG.length + 1) == LOCAL_NETWORK_ONLY_MSG
            }
            if (hitsLocalNetworkMsg) {
                matches.add(kPc)
            }
        }
    }

    require(matches.size == 1) {
        "Expected exactly 1 local-network TBZ guard in $SYM_ON_UPDATE_CONNECT_BUTTON, found ${matches.size}"
    }
    val patchVaddr = matches.single()
    return listOf(
        ElfBytePatch(
            fileOffset = elf.vaddrToFileOffset(patchVaddr),
            bytes = encodeInt32LE(AARCH64_NOP),
            description = "CPanelMain::OnUpdateConnectButtonState bypass local-network guard at 0x${patchVaddr.toString(16)}",
        ),
    )
}

private fun computeVrLinkSubnetPatches(elf: Elf64Arm64Image): List<ElfBytePatch> {
    val patches = mutableListOf<ElfBytePatch>()
    patches.addAll(computeGuessBestPeerPatches(elf))
    patches.addAll(computeEchoSearchPatches(elf))
    patches.addAll(computeRestartSkipBindToDevicePatches(elf))
    return patches
}

private fun computeGuessBestPeerPatches(elf: Elf64Arm64Image): List<ElfBytePatch> {
    val fn = elf.requireSymbol(SYM_GUESS_BEST_PEER)
    require(fn.size in 64L..8192L) { "Unexpected size ${fn.size} for $SYM_GUESS_BEST_PEER" }
    val code = elf.readBytesAtVaddr(fn.vaddr, fn.size.toInt())
    val insnCount = code.size / 4

    // Locate `cmp wBest, wPrefix; cset wRet, ge; bl recursive_mutex::unlock`
    data class ExitCheck(val cmpIdx: Int, val bestReg: Int, val prefixReg: Int)

    val exitChecks = mutableListOf<ExitCheck>()
    for (idx in 0 until insnCount - 2) {
        val cmpPair = decodeCmpWReg(readI32LE(code, idx * 4)) ?: continue
        if (!isCsetWGe(readI32LE(code, (idx + 1) * 4))) continue
        val nextBl = decodeBlTarget(fn.vaddr + (idx + 2) * 4L, readI32LE(code, (idx + 2) * 4)) ?: continue
        if (elf.resolveCallTargetSymbol(nextBl) != SYM_MUTEX_UNLOCK) continue
        exitChecks.add(ExitCheck(idx, cmpPair.first, cmpPair.second))
    }
    require(exitChecks.size == 1) {
        "Expected exactly 1 prefix-length exit check in $SYM_GUESS_BEST_PEER, found ${exitChecks.size}"
    }
    val exit = exitChecks.single()

    // Verify prologue saved w4 (localPrefixLen) into exit.prefixReg.
    val hasPrologueSave = (0 until minOf(insnCount, 24)).any { idx ->
        isMovWReg(readI32LE(code, idx * 4), rd = exit.prefixReg, rm = 4)
    }
    require(hasPrologueSave) {
        "Could not verify prologue save mov w${exit.prefixReg}, w4 in $SYM_GUESS_BEST_PEER"
    }

    // Verify inner loop compares best bits with signed B.LT (so initializing best = -1 makes any >=0 match win).
    val hasSignedLoopCmp = (0 until insnCount - 1).any { idx ->
        val cmp = decodeCmpWReg(readI32LE(code, idx * 4))
        cmp != null &&
            cmp.second == exit.bestReg &&
            decodeBCond(fn.vaddr + (idx + 1) * 4L, readI32LE(code, (idx + 1) * 4), expectedCond = COND_LT) != null
    }
    require(hasSignedLoopCmp) {
        "Could not verify signed loop comparison cmp wCurr, w${exit.bestReg}; b.lt in $SYM_GUESS_BEST_PEER"
    }

    // Find the two `mov wBest, wzr` initializations (loop init + empty-list early exit).
    val zeroInitIndices = (0 until exit.cmpIdx).filter { idx ->
        isMovWReg(readI32LE(code, idx * 4), rd = exit.bestReg, rm = 31)
    }
    require(zeroInitIndices.size == 2) {
        "Expected exactly 2 zero-initializations of w${exit.bestReg} in $SYM_GUESS_BEST_PEER, found ${zeroInitIndices.size}"
    }

    val result = mutableListOf<ElfBytePatch>()
    for (idx in zeroInitIndices) {
        val vaddr = fn.vaddr + idx * 4L
        result.add(
            ElfBytePatch(
                fileOffset = elf.vaddrToFileOffset(vaddr),
                bytes = encodeInt32LE(encodeMovWMinus1(exit.bestReg)),
                description = "GuessBestCorrespondenceAddress init w${exit.bestReg}=-1 at 0x${vaddr.toString(16)}",
            ),
        )
    }
    val cmpVaddr = fn.vaddr + exit.cmpIdx * 4L
    result.add(
        ElfBytePatch(
            fileOffset = elf.vaddrToFileOffset(cmpVaddr),
            bytes = encodeInt32LE(encodeCmpWWzr(exit.bestReg)),
            description = "GuessBestCorrespondenceAddress relax prefix check to w${exit.bestReg}>=0 at 0x${cmpVaddr.toString(16)}",
        ),
    )
    return result
}

private fun computeEchoSearchPatches(elf: Elf64Arm64Image): List<ElfBytePatch> {
    val fn = elf.requireSymbol(SYM_ECHO_SEARCH)
    require(fn.size in 64L..4096L) { "Unexpected size ${fn.size} for $SYM_ECHO_SEARCH" }
    val code = elf.readBytesAtVaddr(fn.vaddr, fn.size.toInt())
    val insnCount = code.size / 4

    // Locate IPv4 subnet filter: `eor wA, wB, wC ... rev wA, wA; tst wA, wMask; b.ne <after sendto>`
    val matches = mutableListOf<Long>()
    for (idx in 2 until insnCount - 2) {
        val revInsn = readI32LE(code, idx * 4)
        val tstInsn = readI32LE(code, (idx + 1) * 4)
        val bneInsn = readI32LE(code, (idx + 2) * 4)
        if (!isRevW(revInsn) || !isTstWReg(tstInsn)) continue
        val bnePc = fn.vaddr + (idx + 2) * 4L
        val skipTarget = decodeBCond(bnePc, bneInsn, expectedCond = COND_NE) ?: continue
        if (skipTarget <= bnePc || skipTarget >= fn.vaddr + fn.size) continue

        val hasNearbyEor = (maxOf(0, idx - 4) until idx).any { j -> isEorWReg(readI32LE(code, j * 4)) }
        if (!hasNearbyEor) continue

        // Verify the skipped region contains a call to sendto.
        val skipTargetIdx = ((skipTarget - fn.vaddr) / 4L).toInt()
        val skipsSendTo = ((idx + 3) until skipTargetIdx).any { j ->
            val blTarget = decodeBlTarget(fn.vaddr + j * 4L, readI32LE(code, j * 4))
            blTarget != null && elf.resolveCallTargetSymbol(blTarget) == "sendto"
        }
        if (skipsSendTo) {
            matches.add(bnePc)
        }
    }

    require(matches.size == 1) {
        "Expected exactly 1 IPv4 subnet filter branch in $SYM_ECHO_SEARCH, found ${matches.size}"
    }
    val patchVaddr = matches.single()
    return listOf(
        ElfBytePatch(
            fileOffset = elf.vaddrToFileOffset(patchVaddr),
            bytes = encodeInt32LE(AARCH64_NOP),
            description = "EchoSearchCorrespondenceAddresses bypass IPv4 subnet check at 0x${patchVaddr.toString(16)}",
        ),
    )
}

private fun computeRestartSkipBindToDevicePatches(elf: Elf64Arm64Image): List<ElfBytePatch> {
    val fn = elf.requireSymbol(SYM_UDP_RESTART)
    require(fn.size in 128L..8192L) { "Unexpected size ${fn.size} for $SYM_UDP_RESTART" }
    val code = elf.readBytesAtVaddr(fn.vaddr, fn.size.toInt())
    val insnCount = code.size / 4

    // Locate `setsockopt(fd, SOL_SOCKET=1, SO_BINDTODEVICE=25, ...)` and its guarding `cbz xN, <skipTarget>`.
    // Skipping SO_BINDTODEVICE lets Android's VPN routing table (WireGuard tun0/wg0) route UDP packets properly.
    val matches = mutableListOf<Pair<Long, Long>>()
    for (idx in 0 until insnCount) {
        val blPc = fn.vaddr + idx * 4L
        val target = decodeBlTarget(blPc, readI32LE(code, idx * 4)) ?: continue
        if (elf.resolveCallTargetSymbol(target) != "setsockopt") continue

        val windowStart = maxOf(0, idx - 6)
        val hasSolSocket = (windowStart until idx).any { j ->
            decodeMovWImm(readI32LE(code, j * 4), expectedRd = 1) == SOL_SOCKET
        }
        val hasBindToDevice = (windowStart until idx).any { j ->
            decodeMovWImm(readI32LE(code, j * 4), expectedRd = 2) == SO_BINDTODEVICE
        }
        if (!hasSolSocket || !hasBindToDevice) continue

        // Find the nearest preceding `cbz` that jumps past this setsockopt call (within the function).
        var guard: Pair<Long, Long>? = null
        for (j in (idx - 1) downTo maxOf(0, idx - 24)) {
            val cbzPc = fn.vaddr + j * 4L
            val skipTarget = decodeCbz(cbzPc, readI32LE(code, j * 4)) ?: continue
            if (skipTarget > blPc && skipTarget < fn.vaddr + fn.size) {
                guard = cbzPc to skipTarget
                break
            }
        }
        if (guard != null) {
            matches.add(guard)
        }
    }

    require(matches.size == 1) {
        "Expected exactly 1 SO_BINDTODEVICE guard in $SYM_UDP_RESTART, found ${matches.size}"
    }
    val (cbzPc, skipTarget) = matches.single()
    return listOf(
        ElfBytePatch(
            fileOffset = elf.vaddrToFileOffset(cbzPc),
            bytes = encodeInt32LE(encodeB(cbzPc, skipTarget)),
            description = "SVLDataLinkTransferUDP::Restart skip SO_BINDTODEVICE at 0x${cbzPc.toString(16)} -> 0x${skipTarget.toString(16)}",
        ),
    )
}

val allowRemoteVr = patch(SteamLinkPatchStrings.allowRemoteVr) {
    description(SteamLinkPatchStrings.allowRemoteVrDescription)
    compatibleWith(STEAMLINK_VR_PACKAGE)
    enabledByDefault(false)

    val hostOption = stringOption(
        key = "host",
        title = SteamLinkPatchStrings.optRemoteVrHostTitle,
        description = SteamLinkPatchStrings.optRemoteVrHostDescription,
        default = "",
    )

    execute {
        val host = options[hostOption].trim()
        require(host.isEmpty() || isValidHostAddress(host)) {
            "host must be blank or a valid IPv4/IPv6 address (optionally with :port), got '$host'"
        }

        val storeUncompressed = shouldStoreNativeLibsUncompressed()

        val shellPatches = files.patchElf64Arm64(LIB_SHELL_PATH, storeUncompressed) { elf ->
            computeShellLocalNetworkPatches(elf)
        }
        val vrLinkPatches = files.patchElf64Arm64(LIB_VRLINK_PATH, storeUncompressed) { elf ->
            computeVrLinkSubnetPatches(elf)
        }

        if (host.isNotEmpty()) {
            steamLinkOnCreate.before {
                RemoteVrBridge.configureHost(thisObject, string(host))
            }
        }
        steamLinkStartVrLink.before {
            param(0).assign(RemoteVrBridge.rewriteStartVrArgs(thisObject, param(0), string(host)))
        }

        for (p in shellPatches + vrLinkPatches) {
            log.debug("Applied native patch: ${p.description} (fileOffset=0x${p.fileOffset.toString(16)})")
        }
        log.info(
            "Enabled remote/VPN VR streaming (${shellPatches.size} shell patch, ${vrLinkPatches.size} vrlink patches, " +
                "host=${host.ifEmpty { "auto" }})",
        )
    }
}
