// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.protocol

import java.security.MessageDigest
import java.util.Locale

object PairingCrypto {
    enum class Role(val wireValue: Byte) { PHONE(1), DESKTOP(2) }

    private const val COMMIT_DOMAIN = "OpenLens Pair v2 commit"
    private const val TRANSCRIPT_DOMAIN = "OpenLens Pair v2 transcript"
    private const val SAS_DOMAIN = "OpenLens Pair v2 sas"
    private const val SAS_RETRY_DOMAIN = "OpenLens Pair v2 sas retry"
    private const val SAS_ACCEPTANCE_LIMIT = 16_000_000

    fun sha256(bytes: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(bytes)

    fun commitment(
        attempt: ByteArray,
        role: Role,
        nonce: ByteArray,
        phoneSpki: ByteArray,
        desktopSpki: ByteArray,
    ): ByteArray {
        require32(attempt, "attempt")
        require32(nonce, "nonce")
        require32(phoneSpki, "phone SPKI pin")
        require32(desktopSpki, "desktop SPKI pin")
        return sha256(
            COMMIT_DOMAIN.encodeToByteArray() + attempt + byteArrayOf(role.wireValue) + nonce +
                phoneSpki + desktopSpki,
        )
    }

    fun transcript(
        attempt: ByteArray,
        phoneSpki: ByteArray,
        desktopSpki: ByteArray,
        phoneCommitment: ByteArray,
        desktopCommitment: ByteArray,
        phoneNonce: ByteArray,
        desktopNonce: ByteArray,
    ): ByteArray {
        listOf(
            "attempt" to attempt,
            "phone SPKI pin" to phoneSpki,
            "desktop SPKI pin" to desktopSpki,
            "phone commitment" to phoneCommitment,
            "desktop commitment" to desktopCommitment,
            "phone nonce" to phoneNonce,
            "desktop nonce" to desktopNonce,
        ).forEach { (name, value) -> require32(value, name) }
        return sha256(
            TRANSCRIPT_DOMAIN.encodeToByteArray() + attempt + phoneSpki + desktopSpki +
                phoneCommitment + desktopCommitment + phoneNonce + desktopNonce,
        )
    }

    fun sixDigitSas(transcriptHash: ByteArray): String {
        require32(transcriptHash, "transcript hash")
        var material = sha256(SAS_DOMAIN.encodeToByteArray() + transcriptHash)
        var counter = 0
        while (first24Bits(material) >= SAS_ACCEPTANCE_LIMIT) {
            counter += 1
            material = sha256(
                SAS_RETRY_DOMAIN.encodeToByteArray() + transcriptHash + byteArrayOf(
                    (counter ushr 24).toByte(),
                    (counter ushr 16).toByte(),
                    (counter ushr 8).toByte(),
                    counter.toByte(),
                ),
            )
        }
        return String.format(Locale.ROOT, "%06d", first24Bits(material) % 1_000_000)
    }

    fun constantTimeEquals(left: ByteArray, right: ByteArray): Boolean =
        MessageDigest.isEqual(left, right)

    private fun first24Bits(value: ByteArray): Int =
        (value[0].toUByte().toInt() shl 16) or
            (value[1].toUByte().toInt() shl 8) or
            value[2].toUByte().toInt()

    private fun require32(value: ByteArray, name: String) {
        require(value.size == 32) { "$name must be 32 bytes" }
    }
}
