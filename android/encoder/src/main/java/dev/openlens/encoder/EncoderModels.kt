// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import java.nio.ByteBuffer

data class EncodedAccessUnit(
    val bytes: ByteArray,
    val ptsUs: Long,
    val isCodecConfig: Boolean,
    val isKeyframe: Boolean,
    val isEndOfStream: Boolean,
) {
    override fun equals(other: Any?): Boolean = other is EncodedAccessUnit &&
        bytes.contentEquals(other.bytes) && ptsUs == other.ptsUs && isCodecConfig == other.isCodecConfig &&
        isKeyframe == other.isKeyframe && isEndOfStream == other.isEndOfStream
    override fun hashCode(): Int = bytes.contentHashCode()
}

data class ActualEncoderFormat(
    val name: String,
    val mime: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val profile: Int,
    val bitrateMode: Int,
)

data class EncoderConfig(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
)

interface EncoderListener {
    fun onFormat(format: ActualEncoderFormat)
    fun onAccessUnit(unit: EncodedAccessUnit)
    fun onEncoderError(error: Throwable)
}

class AvcEncoder(private val listener: EncoderListener) : AutoCloseable {
    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var callbackThread: HandlerThread? = null

    @Synchronized
    fun start(config: EncoderConfig): Surface {
        check(codec == null) { "encoder already started" }
        val thread = HandlerThread("OpenLensEncoder").also(HandlerThread::start)
        callbackThread = thread
        val selectedCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, config.width, config.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, config.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, config.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileMain)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            // Without these, vendor encoders may pipeline many frames internally.
            setInteger(MediaFormat.KEY_LATENCY, 0)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
        }
        selectedCodec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                val output = codec.getOutputBuffer(index)
                if (output == null) {
                    codec.releaseOutputBuffer(index, false)
                    return
                }
                listener.onAccessUnit(output.toAccessUnit(info))
                codec.releaseOutputBuffer(index, false)
            }

            override fun onError(codec: MediaCodec, exception: MediaCodec.CodecException) {
                listener.onEncoderError(exception)
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                listener.onFormat(
                    ActualEncoderFormat(
                        name = codec.name,
                        mime = format.getString(MediaFormat.KEY_MIME) ?: MediaFormat.MIMETYPE_VIDEO_AVC,
                        width = format.getInteger(MediaFormat.KEY_WIDTH),
                        height = format.getInteger(MediaFormat.KEY_HEIGHT),
                        fps = format.getIntegerOrDefault(MediaFormat.KEY_FRAME_RATE, config.fps),
                        bitrate = format.getIntegerOrDefault(MediaFormat.KEY_BIT_RATE, config.bitrate),
                        profile = format.getIntegerOrDefault(MediaFormat.KEY_PROFILE, 0),
                        bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR,
                    ),
                )
                val codecConfig = listOf("csd-0", "csd-1").mapNotNull { key ->
                    format.getByteBuffer(key)?.duplicate()?.let { buffer ->
                        ByteArray(buffer.remaining()).also(buffer::get)
                    }
                }.fold(ByteArray(0)) { accumulated, bytes -> accumulated + bytes }
                if (codecConfig.isNotEmpty()) {
                    listener.onAccessUnit(EncodedAccessUnit(codecConfig, 0L, true, false, false))
                }
            }
        }, Handler(thread.looper))
        selectedCodec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface = selectedCodec.createInputSurface()
        inputSurface = surface
        codec = selectedCodec
        selectedCodec.start()
        return surface
    }

    @Synchronized
    fun requestKeyframe() {
        codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
    }

    @Synchronized
    override fun close() {
        val active = codec
        codec = null
        runCatching { active?.stop() }
        active?.release()
        inputSurface?.release()
        inputSurface = null
        callbackThread?.quitSafely()
        callbackThread = null
    }
}

private fun ByteBuffer.toAccessUnit(info: MediaCodec.BufferInfo): EncodedAccessUnit {
    val copy = duplicate()
    copy.position(info.offset)
    copy.limit(info.offset + info.size)
    val bytes = ByteArray(info.size)
    copy.get(bytes)
    return EncodedAccessUnit(
        bytes = bytes,
        ptsUs = info.presentationTimeUs,
        isCodecConfig = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0,
        isKeyframe = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0,
        isEndOfStream = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0,
    )
}

private fun MediaFormat.getIntegerOrDefault(key: String, fallback: Int): Int =
    if (containsKey(key)) getInteger(key) else fallback
