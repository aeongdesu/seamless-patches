package app.aeong.steamlink

import app.reseam.patch.FileScope
import java.io.EOFException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

internal class ElfBytePatch(
    val fileOffset: Long,
    val bytes: ByteArray,
    val description: String,
)

internal class ElfSection(
    val name: String,
    val type: Int,
    val vaddr: Long,
    val fileOffset: Long,
    val size: Long,
    val entSize: Long,
)

internal data class ElfSymbol(
    val name: String,
    val vaddr: Long,
    val size: Long,
)

internal class Elf64Arm64Image(
    private val openStream: () -> InputStream,
    private val sections: List<ElfSection>,
) {
    private val sectionsByName: Map<String, ElfSection> = sections.associateBy { it.name }
    private val dynsymBytes: ByteArray
    private val dynstrBytes: ByteArray
    private val relaPltBytes: ByteArray?
    private val pltSection: ElfSection?
    private val pltBytes: ByteArray?
    private val dynsymEntSize: Int

    private val symbolsByName: Map<String, ElfSymbol>
    private val symbolsByVaddr: Map<Long, String>
    private val jumpSlotSymIndexByGotVaddr: Map<Long, Int>

    init {
        val dynsym = section(".dynsym")
        val dynstr = section(".dynstr")
        val relaPlt = sectionsByName[".rela.plt"]
        val plt = sectionsByName[".plt"]
        dynsymEntSize = if (dynsym.entSize > 0L) dynsym.entSize.toInt() else 24
        pltSection = plt

        val ranges = mutableListOf(
            dynsym.fileOffset to dynsym.size.toInt(),
            dynstr.fileOffset to dynstr.size.toInt(),
        )
        if (relaPlt != null && relaPlt.size > 0L) {
            ranges.add(relaPlt.fileOffset to relaPlt.size.toInt())
        }
        if (plt != null && plt.size > 0L) {
            ranges.add(plt.fileOffset to plt.size.toInt())
        }
        val fetched = readSlices(openStream, ranges)
        dynsymBytes = fetched.getValue(dynsym.fileOffset)
        dynstrBytes = fetched.getValue(dynstr.fileOffset)
        relaPltBytes = relaPlt?.let { fetched[it.fileOffset] }
        pltBytes = plt?.let { fetched[it.fileOffset] }

        val byName = HashMap<String, ElfSymbol>()
        val byVaddr = HashMap<Long, String>()
        val symCount = dynsymBytes.size / dynsymEntSize
        for (i in 0 until symCount) {
            val base = i * dynsymEntSize
            val nameOff = readI32LE(dynsymBytes, base)
            val value = readU64LE(dynsymBytes, base + 8)
            val size = readU64LE(dynsymBytes, base + 16)
            val name = readCString(dynstrBytes, nameOff)
            if (name.isNotEmpty()) {
                val sym = ElfSymbol(name, value, size)
                if (!byName.containsKey(name)) {
                    byName[name] = sym
                }
                if (value != 0L && !byVaddr.containsKey(value)) {
                    byVaddr[value] = name
                }
            }
        }
        symbolsByName = byName
        symbolsByVaddr = byVaddr

        val jumpSlots = HashMap<Long, Int>()
        if (relaPlt != null && relaPltBytes != null) {
            val entSize = if (relaPlt.entSize > 0L) relaPlt.entSize.toInt() else 24
            val count = relaPltBytes.size / entSize
            for (i in 0 until count) {
                val base = i * entSize
                val rOffset = readU64LE(relaPltBytes, base)
                val rInfo = readU64LE(relaPltBytes, base + 8)
                val rType = (rInfo and 0xffffffffL).toInt()
                val symIdx = (rInfo ushr 32).toInt()
                if (rType == R_AARCH64_JUMP_SLOT) {
                    jumpSlots[rOffset] = symIdx
                }
            }
        }
        jumpSlotSymIndexByGotVaddr = jumpSlots
    }

    private fun section(name: String): ElfSection =
        sectionsByName[name] ?: error("ELF missing required section '$name'")

    fun requireSymbol(name: String): ElfSymbol =
        symbolsByName[name] ?: error("ELF missing required dynamic symbol '$name'")

    fun vaddrToFileOffset(vaddr: Long): Long {
        for (sec in sections) {
            if (sec.type != SHT_NOBITS && sec.vaddr != 0L && vaddr >= sec.vaddr && vaddr < sec.vaddr + sec.size) {
                return sec.fileOffset + (vaddr - sec.vaddr)
            }
        }
        error("Cannot map ELF virtual address 0x${vaddr.toString(16)} to file offset")
    }

    fun readBytesAtVaddr(vaddr: Long, length: Int): ByteArray {
        val fileOff = vaddrToFileOffset(vaddr)
        return readSlices(openStream, listOf(fileOff to length)).getValue(fileOff)
    }

    fun readCStringAtVaddr(vaddr: Long, maxLen: Int = 256): String? {
        val sec = sections.firstOrNull {
            it.type != SHT_NOBITS && it.vaddr != 0L && vaddr >= it.vaddr && vaddr < it.vaddr + it.size
        } ?: return null
        val avail = (sec.vaddr + sec.size - vaddr).coerceAtMost(maxLen.toLong()).toInt()
        if (avail <= 0) return null
        val bytes = readBytesAtVaddr(vaddr, avail)
        val zeroIdx = bytes.indexOf(0.toByte())
        val end = if (zeroIdx >= 0) zeroIdx else bytes.size
        return String(bytes, 0, end, Charsets.UTF_8)
    }

    fun resolveCallTargetSymbol(targetVaddr: Long): String? {
        val plt = pltSection
        val pltData = pltBytes
        if (plt != null && pltData != null && targetVaddr >= plt.vaddr && targetVaddr + 8L <= plt.vaddr + plt.size) {
            val stubOff = (targetVaddr - plt.vaddr).toInt()
            // Standard AArch64 PLT stub (with optional leading BTI): ADRP x16, page; LDR x17, [x16, #off]
            for (delta in intArrayOf(0, 4)) {
                if (stubOff + delta + 8 > pltData.size) break
                val pc = targetVaddr + delta
                val i0 = readI32LE(pltData, stubOff + delta)
                val i1 = readI32LE(pltData, stubOff + delta + 4)
                val page = decodeAdrp(pc, i0, expectedRd = 16) ?: continue
                val ldrOff = decodeLdr64UnsignedOffset(i1, expectedRt = 17, expectedRn = 16) ?: continue
                val gotVaddr = page + ldrOff
                val symIdx = jumpSlotSymIndexByGotVaddr[gotVaddr] ?: continue
                val base = symIdx * dynsymEntSize
                if (base + 4 <= dynsymBytes.size) {
                    val nameOff = readI32LE(dynsymBytes, base)
                    return readCString(dynstrBytes, nameOff)
                }
            }
        }
        return symbolsByVaddr[targetVaddr]
    }

    companion object {
        private const val ELF_MAGIC = 0x464c457f
        private const val ELFCLASS64 = 2
        private const val ELFDATA2LSB = 1
        private const val EM_AARCH64 = 183
        private const val SHT_NOBITS = 8
        private const val R_AARCH64_JUMP_SLOT = 1026

        fun parse(openStream: () -> InputStream): Elf64Arm64Image {
            val sections = openStream().use { stream ->
                val ehdr = readExact(stream, 64)
                val bb = ByteBuffer.wrap(ehdr).order(ByteOrder.LITTLE_ENDIAN)
                require(bb.getInt(0) == ELF_MAGIC) { "Not a valid ELF binary" }
                require(ehdr[4].toInt() == ELFCLASS64) { "Expected 64-bit ELF (got class ${ehdr[4]})" }
                require(ehdr[5].toInt() == ELFDATA2LSB) { "Expected little-endian ELF" }
                val machine = bb.getShort(18).toInt() and 0xffff
                require(machine == EM_AARCH64) { "Expected AArch64 ELF (e_machine=$EM_AARCH64, got $machine)" }

                val shoff = bb.getLong(40)
                val shentsize = bb.getShort(58).toInt() and 0xffff
                val shnum = bb.getShort(60).toInt() and 0xffff
                val shstrndx = bb.getShort(62).toInt() and 0xffff
                require(shoff >= 64L && shentsize >= 64 && shnum > 0 && shstrndx in 0 until shnum) {
                    "Invalid ELF section header table (shoff=$shoff, shentsize=$shentsize, shnum=$shnum, shstrndx=$shstrndx)"
                }

                skipExact(stream, shoff - 64L)
                val shTableBytes = readExact(stream, shentsize * shnum)
                val shBuf = ByteBuffer.wrap(shTableBytes).order(ByteOrder.LITTLE_ENDIAN)

                data class RawSec(
                    val nameOff: Int,
                    val type: Int,
                    val vaddr: Long,
                    val fileOffset: Long,
                    val size: Long,
                    val entSize: Long,
                )

                val rawSecs = ArrayList<RawSec>(shnum)
                for (i in 0 until shnum) {
                    val base = i * shentsize
                    rawSecs.add(
                        RawSec(
                            nameOff = shBuf.getInt(base),
                            type = shBuf.getInt(base + 4),
                            vaddr = shBuf.getLong(base + 16),
                            fileOffset = shBuf.getLong(base + 24),
                            size = shBuf.getLong(base + 32),
                            entSize = shBuf.getLong(base + 56),
                        ),
                    )
                }

                val shstrRaw = rawSecs[shstrndx]
                val shstrBytes = readSlices(openStream, listOf(shstrRaw.fileOffset to shstrRaw.size.toInt()))
                    .getValue(shstrRaw.fileOffset)

                rawSecs.map { raw ->
                    ElfSection(
                        name = readCString(shstrBytes, raw.nameOff),
                        type = raw.type,
                        vaddr = raw.vaddr,
                        fileOffset = raw.fileOffset,
                        size = raw.size,
                        entSize = raw.entSize,
                    )
                }
            }
            return Elf64Arm64Image(openStream, sections)
        }
    }
}

internal fun FileScope.patchElf64Arm64(
    apkPath: String,
    storeUncompressed: Boolean,
    computePatches: (Elf64Arm64Image) -> List<ElfBytePatch>,
): List<ElfBytePatch> {
    val openStream = {
        stream(apkPath) ?: error("APK missing expected native library '$apkPath'")
    }
    val image = Elf64Arm64Image.parse(openStream)
    val patches = computePatches(image).sortedBy { it.fileOffset }
    require(patches.isNotEmpty()) { "No patches computed for '$apkPath'" }
    for (i in 1 until patches.size) {
        val prev = patches[i - 1]
        val curr = patches[i]
        require(prev.fileOffset + prev.bytes.size <= curr.fileOffset) {
            "Overlapping ELF patches in '$apkPath' at 0x${curr.fileOffset.toString(16)}"
        }
    }
    openStream().use { input ->
        val patched = PatchingInputStream(input, patches)
        if (storeUncompressed) {
            writeStored(apkPath, patched)
        } else {
            write(apkPath, patched)
        }
    }
    return patches
}

private class PatchingInputStream(
    private val delegate: InputStream,
    private val patches: List<ElfBytePatch>,
) : InputStream() {
    private var position = 0L
    private var nextPatchIdx = 0

    override fun read(): Int {
        val single = ByteArray(1)
        val n = read(single, 0, 1)
        return if (n < 0) -1 else single[0].toInt() and 0xff
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val count = delegate.read(b, off, len)
        if (count <= 0) return count

        val chunkStart = position
        val chunkEnd = position + count
        position = chunkEnd

        while (nextPatchIdx < patches.size && patches[nextPatchIdx].fileOffset + patches[nextPatchIdx].bytes.size <= chunkStart) {
            nextPatchIdx++
        }
        var idx = nextPatchIdx
        while (idx < patches.size && patches[idx].fileOffset < chunkEnd) {
            val p = patches[idx]
            val pStart = p.fileOffset
            val pEnd = pStart + p.bytes.size
            val overlapStart = maxOf(chunkStart, pStart)
            val overlapEnd = minOf(chunkEnd, pEnd)
            val dstOff = off + (overlapStart - chunkStart).toInt()
            val srcOff = (overlapStart - pStart).toInt()
            val copyLen = (overlapEnd - overlapStart).toInt()
            System.arraycopy(p.bytes, srcOff, b, dstOff, copyLen)
            idx++
        }
        return count
    }

    override fun close() = delegate.close()
}

// --- AArch64 instruction decoding / encoding helpers ---

internal const val AARCH64_NOP: Int = 0xd503201f.toInt()

internal fun encodeInt32LE(value: Int): ByteArray = byteArrayOf(
    (value and 0xff).toByte(),
    ((value ushr 8) and 0xff).toByte(),
    ((value ushr 16) and 0xff).toByte(),
    ((value ushr 24) and 0xff).toByte(),
)

internal fun readI32LE(bytes: ByteArray, offset: Int): Int =
    (bytes[offset].toInt() and 0xff) or
        ((bytes[offset + 1].toInt() and 0xff) shl 8) or
        ((bytes[offset + 2].toInt() and 0xff) shl 16) or
        ((bytes[offset + 3].toInt() and 0xff) shl 24)

private fun readU64LE(bytes: ByteArray, offset: Int): Long {
    var result = 0L
    for (i in 0 until 8) {
        result = result or ((bytes[offset + i].toLong() and 0xffL) shl (i * 8))
    }
    return result
}

internal fun decodeBlTarget(pc: Long, insn: Int): Long? {
    if ((insn and 0xfc000000.toInt()) != 0x94000000.toInt()) return null
    val imm26 = (insn shl 6) shr 6
    return pc + (imm26.toLong() shl 2)
}

private fun decodeAdrp(pc: Long, insn: Int, expectedRd: Int? = null): Long? {
    if ((insn and 0x9f000000.toInt()) != 0x90000000.toInt()) return null
    val rd = insn and 0x1f
    if (expectedRd != null && rd != expectedRd) return null
    val immlo = (insn ushr 29) and 0x3
    val immhi = (insn ushr 5) and 0x7ffff
    val imm21 = (((immhi shl 2) or immlo) shl 11) shr 11
    return (pc and -4096L) + (imm21.toLong() shl 12)
}

private fun decodeAddImm64(insn: Int): Triple<Int, Int, Long>? {
    if ((insn and 0xff800000.toInt()) != 0x91000000.toInt()) return null
    val rd = insn and 0x1f
    val rn = (insn ushr 5) and 0x1f
    val imm12 = ((insn ushr 10) and 0xfff).toLong()
    val shift = if (((insn ushr 22) and 1) != 0) 12 else 0
    return Triple(rd, rn, imm12 shl shift)
}

private fun decodeLdr64UnsignedOffset(insn: Int, expectedRt: Int? = null, expectedRn: Int? = null): Long? {
    if ((insn and 0xffc00000.toInt()) != 0xf9400000.toInt()) return null
    val rt = insn and 0x1f
    val rn = (insn ushr 5) and 0x1f
    if (expectedRt != null && rt != expectedRt) return null
    if (expectedRn != null && rn != expectedRn) return null
    return (((insn ushr 10) and 0xfff).toLong()) shl 3
}

internal fun decodeTbz(pc: Long, insn: Int, expectedBit: Int? = null): Long? {
    // TBZ (op=0): bit 30..24 = 0110110 => mask 0x7f000000 == 0x36000000
    if ((insn and 0x7f000000) != 0x36000000) return null
    val bit = (((insn ushr 31) and 1) shl 5) or ((insn ushr 19) and 0x1f)
    if (expectedBit != null && bit != expectedBit) return null
    val imm14 = (((insn ushr 5) and 0x3fff) shl 18) shr 18
    return pc + (imm14.toLong() shl 2)
}

internal fun decodeBCond(pc: Long, insn: Int, expectedCond: Int? = null): Long? {
    if ((insn and 0xff000010.toInt()) != 0x54000000) return null
    val cond = insn and 0xf
    if (expectedCond != null && cond != expectedCond) return null
    val imm19 = (((insn ushr 5) and 0x7ffff) shl 13) shr 13
    return pc + (imm19.toLong() shl 2)
}

internal fun decodeCbz(pc: Long, insn: Int): Long? {
    // CBZ (32-bit 0x34000000 or 64-bit 0xb4000000): mask 0x7f000000 == 0x34000000
    if ((insn and 0x7f000000) != 0x34000000) return null
    val imm19 = (((insn ushr 5) and 0x7ffff) shl 13) shr 13
    return pc + (imm19.toLong() shl 2)
}

internal fun decodeMovWImm(insn: Int, expectedRd: Int? = null): Int? {
    // MOVZ Wd, #imm16 (shift=0): 0101 0010 100 iiiiiiiiiiiiiiii ddddd
    if ((insn and 0xffe00000.toInt()) != 0x52800000) return null
    val rd = insn and 0x1f
    if (expectedRd != null && rd != expectedRd) return null
    return (insn ushr 5) and 0xffff
}

internal fun encodeB(pc: Long, target: Long): Int {
    val delta = target - pc
    require((delta and 3L) == 0L) { "Unaligned branch target 0x${target.toString(16)} from 0x${pc.toString(16)}" }
    val imm26 = delta shr 2
    require(imm26 in -(1L shl 25)..((1L shl 25) - 1L)) {
        "Branch target out of range: delta=$delta"
    }
    return 0x14000000 or (imm26.toInt() and 0x03ffffff)
}

internal fun decodeCmpWReg(insn: Int): Pair<Int, Int>? {
    // SUBS WZR, Wn, Wm (unshifted): 0110 1011 000 mmmmm 000000 nnnnn 11111
    if ((insn and 0xffe0fc1f.toInt()) != 0x6b00001f) return null
    val rn = (insn ushr 5) and 0x1f
    val rm = (insn ushr 16) and 0x1f
    return rn to rm
}

internal fun encodeCmpWWzr(rn: Int): Int = 0x6b1f001f or ((rn and 0x1f) shl 5)

internal fun isCsetWGe(insn: Int): Boolean = (insn and 0xffffffe0.toInt()) == 0x1a9fb7e0

internal fun isMovWReg(insn: Int, rd: Int, rm: Int): Boolean =
    insn == (0x2a0003e0 or ((rm and 0x1f) shl 16) or (rd and 0x1f))

internal fun encodeMovWMinus1(rd: Int): Int = 0x12800000 or (rd and 0x1f)

internal fun isEorWReg(insn: Int): Boolean = (insn and 0xff20fc00.toInt()) == 0x4a000000

internal fun isRevW(insn: Int): Boolean = (insn and 0xfffffc00.toInt()) == 0x5ac00800

internal fun isTstWReg(insn: Int): Boolean = (insn and 0xff20fc1f.toInt()) == 0x6a00001f

internal fun findAdrpAddTargets(code: ByteArray, baseVaddr: Long, startIdx: Int, endIdx: Int): List<Long> {
    val pages = HashMap<Int, Long>()
    val targets = mutableListOf<Long>()
    val safeEnd = minOf(endIdx, code.size / 4)
    for (idx in maxOf(0, startIdx) until safeEnd) {
        val pc = baseVaddr + idx * 4L
        val insn = readI32LE(code, idx * 4)
        val page = decodeAdrp(pc, insn)
        if (page != null) {
            pages[insn and 0x1f] = page
            continue
        }
        val add = decodeAddImm64(insn)
        if (add != null) {
            val (rd, rn, imm) = add
            val basePage = pages[rn]
            if (basePage != null) {
                val resolved = basePage + imm
                targets.add(resolved)
                if (rd == rn) pages.remove(rn)
            }
        }
    }
    return targets
}

// --- Streaming I/O helpers ---

private fun readSlices(openStream: () -> InputStream, rawSlices: List<Pair<Long, Int>>): Map<Long, ByteArray> {
    val uniqueSorted = rawSlices
        .filter { it.second >= 0 }
        .distinctBy { it.first to it.second }
        .sortedBy { it.first }
    val result = HashMap<Long, ByteArray>(uniqueSorted.size)
    openStream().use { stream ->
        var pos = 0L
        for ((offset, length) in uniqueSorted) {
            if (offset < pos) {
                // Rare overlapping slice: reopen a fresh stream for this slice only.
                openStream().use { retry ->
                    skipExact(retry, offset)
                    result[offset] = readExact(retry, length)
                }
            } else {
                skipExact(stream, offset - pos)
                val data = readExact(stream, length)
                pos = offset + length
                result[offset] = data
            }
        }
    }
    return result
}

private fun skipExact(stream: InputStream, bytesToSkip: Long) {
    var remaining = bytesToSkip
    val scratch = ByteArray(8192)
    while (remaining > 0L) {
        val skipped = stream.skip(remaining)
        if (skipped > 0L) {
            remaining -= skipped
        } else {
            val n = stream.read(scratch, 0, minOf(remaining, scratch.size.toLong()).toInt())
            if (n < 0) throw EOFException("Unexpected EOF while skipping $bytesToSkip bytes")
            remaining -= n
        }
    }
}

private fun readExact(stream: InputStream, length: Int): ByteArray {
    val out = ByteArray(length)
    var offset = 0
    while (offset < length) {
        val n = stream.read(out, offset, length - offset)
        if (n < 0) throw EOFException("Unexpected EOF while reading $length bytes (got $offset)")
        offset += n
    }
    return out
}

private fun readCString(table: ByteArray, offset: Int): String {
    if (offset < 0 || offset >= table.size) return ""
    var end = offset
    while (end < table.size && table[end].toInt() != 0) end++
    return String(table, offset, end - offset, Charsets.UTF_8)
}
