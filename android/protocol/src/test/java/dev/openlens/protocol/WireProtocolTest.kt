// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class WireProtocolTest {
    @Test
    fun helloMatchesGoldenFixture() {
        val json = """{"schema":1,"client":"desktop","nonce":"0123456789abcdef"}"""
        val message = WireMessage(
            WireHeader(type = MessageType.HELLO.wireValue, flags = MessageFlags.REQUIRED, sequence = 1u),
            json.encodeToByteArray(),
        )
        val golden = readHex("valid/hello.hex")
        assertArrayEquals(golden, WireCodec.serialize(message))
        val parsed = WireCodec.parse(golden)
        assertTrue(parsed is ParseResult.Complete)
        val parsedMessage = (parsed as ParseResult.Complete).message
        assertEquals(message.header.copy(payloadLength = message.payload.size.toLong()), parsedMessage.header)
        assertArrayEquals(message.payload, parsedMessage.payload)
    }

    @Test
    fun parsesOneByteAtATime() {
        val parser = IncrementalParser()
        readHex("valid/hello.hex").forEach { parser.feed(byteArrayOf(it)) }
        assertTrue(parser.next() is ParseResult.Complete)
        assertEquals(0, parser.bufferedBytes())
    }

    @Test
    fun rejectsMalformedFixtures() {
        listOf("bad-magic.hex", "bad-major.hex", "short-header.hex", "oversized-metadata.hex", "unknown-required.hex")
            .forEach { name -> assertTrue(name, WireCodec.parse(readHex("malformed/$name")) is ParseResult.Invalid) }
    }

    @Test
    fun acceptsUnknownOptionalMessage() {
        val result = WireCodec.parse(WireCodec.serialize(WireMessage(WireHeader(type = 0x8000))))
        assertTrue(result is ParseResult.Complete)
        assertFalse(MessageType.isKnown((result as ParseResult.Complete).message.header.type))
    }

    private fun readHex(name: String): ByteArray {
        val text = requireNotNull(javaClass.classLoader?.getResource(name)).readText()
        return text.filterNot(Char::isWhitespace).chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    }
}
