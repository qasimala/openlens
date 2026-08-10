// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PairingCryptoTest {
    @Test
    fun sharedVectorMatches() {
        val attempt = ByteArray(32) { it.toByte() }
        val phoneNonce = ByteArray(32) { (it + 32).toByte() }
        val desktopNonce = ByteArray(32) { (it + 64).toByte() }
        val phoneSpki = ByteArray(32) { 0xa1.toByte() }
        val desktopSpki = ByteArray(32) { 0xd2.toByte() }
        val phoneCommit = PairingCrypto.commitment(
            attempt, PairingCrypto.Role.PHONE, phoneNonce, phoneSpki, desktopSpki,
        )
        val desktopCommit = PairingCrypto.commitment(
            attempt, PairingCrypto.Role.DESKTOP, desktopNonce, phoneSpki, desktopSpki,
        )
        val transcript = PairingCrypto.transcript(
            attempt, phoneSpki, desktopSpki, phoneCommit, desktopCommit, phoneNonce, desktopNonce,
        )
        assertEquals("4a6c4bf3a8db672c0561afaaf0417375274292417be383eed41fa3cb647db3df", phoneCommit.hex())
        assertEquals("fb6660355dfcf8f3470f3ad4fab4b0c5bb2239f02bc901c3d3ca008d901fec65", desktopCommit.hex())
        assertEquals("e7e057d66dccc1881004f88e356551d7529c6d104e882425794bdf472e560c15", transcript.hex())
        assertEquals("141454", PairingCrypto.sixDigitSas(transcript))
    }

    @Test
    fun commitmentsAreRoleAndNonceBound() {
        val value = ByteArray(32) { it.toByte() }
        val phone = PairingCrypto.commitment(value, PairingCrypto.Role.PHONE, value, value, value)
        val desktop = PairingCrypto.commitment(value, PairingCrypto.Role.DESKTOP, value, value, value)
        assertFalse(phone.contentEquals(desktop))
    }

    @Test
    fun constantTimeComparisonHandlesEqualAndDifferentValues() {
        assertTrue(PairingCrypto.constantTimeEquals(ByteArray(32) { 7 }, ByteArray(32) { 7 }))
        assertFalse(PairingCrypto.constantTimeEquals(ByteArray(32) { 7 }, ByteArray(32) { 8 }))
        assertFalse(PairingCrypto.constantTimeEquals(ByteArray(31), ByteArray(32)))
    }

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it) }
}
