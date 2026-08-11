// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder

object WireProtocol {
    const val MAJOR: Int = 2
    const val MINOR: Int = 0
    const val BASE_HEADER_SIZE: Int = 36
    const val MAX_HEADER_SIZE: Int = 4096
    const val MAX_PAYLOAD_SIZE: Int = 8 * 1024 * 1024
    const val MAX_METADATA_SIZE: Int = 256 * 1024
    val MAGIC: ByteArray = byteArrayOf('P'.code.toByte(), 'H'.code.toByte(), 'C'.code.toByte(), 'M'.code.toByte())
}

enum class MessageType(val wireValue: Int) {
    HELLO(1),
    HELLO_ACK(2),
    CAPABILITIES(3),
    CONFIGURE(4),
    CONFIGURED(5),
    VIDEO_CONFIG(6),
    VIDEO_FRAME(7),
    CONTROL(8),
    CONTROL_ACK(9),
    STATS(10),
    PING(11),
    PONG(12),
    ERROR(13),
    END_STREAM(14),
    ORIENTATION(15),
    ;

    companion object {
        fun isKnown(value: Int): Boolean = entries.any { it.wireValue == value }
    }
}

object MessageFlags {
    const val REQUIRED: Int = 1 shl 0
    const val KEYFRAME: Int = 1 shl 1
    const val CONFIG: Int = 1 shl 2
    const val ACKNOWLEDGEMENT: Int = 1 shl 3
    const val END_OF_STREAM: Int = 1 shl 4
}

data class WireHeader(
    val major: Int = WireProtocol.MAJOR,
    val minor: Int = WireProtocol.MINOR,
    val type: Int,
    val flags: Int = 0,
    val headerLength: Int = WireProtocol.BASE_HEADER_SIZE,
    val streamId: Long = 0,
    val sequence: ULong = 0u,
    val ptsUs: ULong = 0u,
    val payloadLength: Long = 0,
)

data class WireMessage(
    val header: WireHeader,
    val payload: ByteArray = byteArrayOf(),
) {
    override fun equals(other: Any?): Boolean =
        other is WireMessage && header == other.header && payload.contentEquals(other.payload)

    override fun hashCode(): Int = 31 * header.hashCode() + payload.contentHashCode()
}

sealed interface ParseResult {
    data class Complete(val message: WireMessage, val consumed: Int) : ParseResult
    data object NeedMoreData : ParseResult
    data class Invalid(val reason: String) : ParseResult
}

object WireCodec {
    fun serialize(message: WireMessage): ByteArray {
        require(message.header.major == WireProtocol.MAJOR) { "unsupported protocol major" }
        require(message.header.headerLength == WireProtocol.BASE_HEADER_SIZE) { "serializer requires base header" }
        require(message.payload.size <= payloadLimit(message.header.type)) { "payload exceeds limit" }
        val buffer = ByteBuffer.allocate(WireProtocol.BASE_HEADER_SIZE + message.payload.size).order(ByteOrder.BIG_ENDIAN)
        buffer.put(WireProtocol.MAGIC)
        buffer.put(message.header.major.toByte())
        buffer.put(message.header.minor.toByte())
        buffer.putShort(message.header.type.toShort())
        buffer.putShort(message.header.flags.toShort())
        buffer.putShort(message.header.headerLength.toShort())
        buffer.putInt(message.header.streamId.toInt())
        buffer.putLong(message.header.sequence.toLong())
        buffer.putLong(message.header.ptsUs.toLong())
        buffer.putInt(message.payload.size)
        buffer.put(message.payload)
        return buffer.array()
    }

    fun parse(bytes: ByteArray): ParseResult {
        if (bytes.size < WireProtocol.BASE_HEADER_SIZE) return ParseResult.NeedMoreData
        if (!bytes.copyOfRange(0, 4).contentEquals(WireProtocol.MAGIC)) return ParseResult.Invalid("invalid PHCM magic")
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        buffer.position(4)
        val major = buffer.get().toUByte().toInt()
        val minor = buffer.get().toUByte().toInt()
        val type = buffer.short.toUShort().toInt()
        val flags = buffer.short.toUShort().toInt()
        val headerLength = buffer.short.toUShort().toInt()
        val streamId = buffer.int.toUInt().toLong()
        val sequence = buffer.long.toULong()
        val ptsUs = buffer.long.toULong()
        val payloadLength = buffer.int.toUInt().toLong()
        if (major != WireProtocol.MAJOR) return ParseResult.Invalid("unsupported protocol major")
        if (headerLength !in WireProtocol.BASE_HEADER_SIZE..WireProtocol.MAX_HEADER_SIZE) {
            return ParseResult.Invalid("invalid header length")
        }
        if (payloadLength > payloadLimit(type)) return ParseResult.Invalid("payload exceeds message limit")
        if (!MessageType.isKnown(type) && flags and MessageFlags.REQUIRED != 0) {
            return ParseResult.Invalid("unknown required message type")
        }
        val totalLength = headerLength.toLong() + payloadLength
        if (totalLength > Int.MAX_VALUE) return ParseResult.Invalid("message size overflow")
        if (bytes.size < totalLength.toInt()) return ParseResult.NeedMoreData
        val payload = bytes.copyOfRange(headerLength, totalLength.toInt())
        return ParseResult.Complete(
            WireMessage(
                WireHeader(major, minor, type, flags, headerLength, streamId, sequence, ptsUs, payloadLength),
                payload,
            ),
            totalLength.toInt(),
        )
    }

    private fun payloadLimit(type: Int): Long =
        if (type == MessageType.VIDEO_CONFIG.wireValue || type == MessageType.VIDEO_FRAME.wireValue) {
            WireProtocol.MAX_PAYLOAD_SIZE.toLong()
        } else {
            WireProtocol.MAX_METADATA_SIZE.toLong()
        }
}

class IncrementalParser {
    private var buffer = byteArrayOf()

    fun feed(bytes: ByteArray) {
        val maximum = WireProtocol.MAX_PAYLOAD_SIZE + WireProtocol.MAX_HEADER_SIZE
        require(bytes.size <= maximum - buffer.size) { "parser buffer limit exceeded" }
        buffer += bytes
    }

    fun next(): ParseResult {
        val result = WireCodec.parse(buffer)
        if (result is ParseResult.Complete) buffer = buffer.copyOfRange(result.consumed, buffer.size)
        return result
    }

    fun bufferedBytes(): Int = buffer.size
    fun reset() { buffer = byteArrayOf() }
}
