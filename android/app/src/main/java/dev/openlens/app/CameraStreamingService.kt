// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import dev.openlens.camera.CameraEngine
import dev.openlens.camera.CameraControlState
import dev.openlens.camera.CameraListener
import dev.openlens.camera.CameraSummary
import dev.openlens.camera.CertifiedPresets
import dev.openlens.camera.Facing
import dev.openlens.camera.VideoPreset
import dev.openlens.encoder.ActualEncoderFormat
import dev.openlens.encoder.AvcEncoder
import dev.openlens.encoder.EncodedAccessUnit
import dev.openlens.encoder.EncoderConfig
import dev.openlens.encoder.EncoderListener
import dev.openlens.protocol.IncrementalParser
import dev.openlens.protocol.MessageFlags
import dev.openlens.protocol.MessageType
import dev.openlens.protocol.ParseResult
import dev.openlens.protocol.WireCodec
import dev.openlens.protocol.WireHeader
import dev.openlens.protocol.WireMessage
import java.io.Closeable
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

class CameraStreamingService : Service(), CameraListener, EncoderListener {
    private val running = AtomicBoolean(false)
    private val connectionActive = AtomicBoolean(false)
    private val sequence = AtomicLong(1)
    private val queue = ArrayBlockingQueue<WireMessage>(3)
    private var client: Closeable? = null
    private var output: OutputStream? = null
    private var camera: CameraEngine? = null
    private var encoder: AvcEncoder? = null
    private var streamId = 1L
    private var requestedPreset: VideoPreset = CertifiedPresets.FULL_HD_1080P30
    private var facing: Facing = Facing.BACK
    private var controls = CameraControlState()
    private var frames = 0L
    private var bytes = 0L
    private var dropped = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        activeInstance = this
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Camera streaming", NotificationManager.IMPORTANCE_LOW),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            running.set(false)
            connectionActive.set(false)
            runCatching { client?.close() }
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_WIFI) {
            val socket = WifiSocketRegistry.take()
            if (socket == null) {
                publish(SessionState.ERROR, "The secure Wi-Fi connection was no longer available.")
                stopSelf()
                return START_NOT_STICKY
            }
            if (!running.compareAndSet(false, true)) {
                runCatching { socket.close() }
                return START_NOT_STICKY
            }
            startForeground(
                NOTIFICATION_ID,
                notification("Starting secure Wi-Fi camera"),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA,
            )
            publish(SessionState.STARTING, "Your computer securely requested the camera. Starting automatically…")
            thread(name = "OpenLensWifiSession") {
                try {
                    serveConnection(socket.inputStream, socket.outputStream, socket, "Wi-Fi")
                } catch (error: Throwable) {
                    if (running.get()) publish(SessionState.ERROR, error.message ?: "Wi-Fi camera session failed.")
                } finally {
                    running.set(false)
                    stopSelf()
                }
            }
            return START_NOT_STICKY
        }
        publish(SessionState.ERROR, "OpenLens only accepts paired local Wi-Fi camera requests.")
        stopSelf()
        return START_NOT_STICKY
    }

    private fun serveConnection(input: InputStream, destination: OutputStream, connection: Closeable, transport: String) {
        client = connection
        output = destination
        connectionActive.set(true)
        queue.clear()
        val writer = thread(name = "OpenLensWriter") { writerLoop() }
        try {
            val parser = IncrementalParser()
            val buffer = ByteArray(64 * 1024)
            while (running.get() && connectionActive.get()) {
                val count = input.read(buffer)
                if (count < 0) break
                parser.feed(buffer.copyOf(count))
                while (true) {
                    when (val result = parser.next()) {
                        ParseResult.NeedMoreData -> break
                        is ParseResult.Invalid -> throw IllegalStateException(result.reason)
                        is ParseResult.Complete -> handle(result.message)
                    }
                }
            }
        } finally {
            connectionActive.set(false)
            writer.interrupt()
            writer.join(2_000)
            camera?.close()
            camera = null
            encoder?.close()
            encoder = null
            runCatching { connection.close() }
            output = null
            client = null
            queue.clear()
            if (running.get() && transport == "Wi-Fi") {
                publish(SessionState.RECOVERING, "Wi-Fi connection lost. Reconnect from the desktop app.")
            }
        }
    }

    private fun handle(message: WireMessage) {
        when (message.header.type) {
            MessageType.HELLO.wireValue -> {
                sendMetadata(MessageType.HELLO_ACK, """{"schema":1,"server":"android","version":"0.1.0"}""")
                sendMetadata(MessageType.CAPABILITIES, capabilitiesJson())
            }
            MessageType.CONFIGURE.wireValue -> {
                val json = message.payload.decodeToString()
                val basePreset = if (json.intValue("height", 1080) == 720) {
                    CertifiedPresets.HD_720P30
                } else {
                    CertifiedPresets.FULL_HD_1080P30
                }
                requestedPreset = basePreset.copy(
                    bitrate = json.intValue("bitrate", basePreset.bitrate).coerceIn(2_000_000, 16_000_000),
                )
                facing = if (json.stringValue("facing", "back") == "front") Facing.FRONT else Facing.BACK
                startCapture(requestedPreset)
                sendMetadata(
                    MessageType.CONFIGURED,
                    """{"schema":1,"width":${requestedPreset.width},"height":${requestedPreset.height},"fps":${requestedPreset.fps},"bitrate":${requestedPreset.bitrate},"facing":"${facing.name.lowercase()}"}""",
                )
            }
            MessageType.PING.wireValue -> sendMetadata(MessageType.PONG, "{}", MessageFlags.ACKNOWLEDGEMENT)
            MessageType.CONTROL.wireValue -> {
                val json = message.payload.decodeToString()
                applyControls(
                    CameraControlState(
                        zoom = json.floatValue("zoom", controls.zoom),
                        exposure = json.intValue("exposure", controls.exposure),
                        torch = json.booleanValue("torch", controls.torch),
                    ),
                )
            }
            MessageType.END_STREAM.wireValue -> stopSelf()
        }
    }

    private fun startCapture(preset: VideoPreset) {
        camera?.close()
        encoder?.close()
        streamId += 1
        val newEncoder = AvcEncoder(this)
        encoder = newEncoder
        val encoderSurface = newEncoder.start(EncoderConfig(preset.width, preset.height, preset.fps, preset.bitrate))
        val surfaces = buildList {
            add(encoderSurface)
            PreviewSurfaceRegistry.surface?.takeIf { it.isValid }?.let(::add)
        }
        val newCamera = CameraEngine(this, this)
        camera = newCamera
        newCamera.start(facing, preset, surfaces)
        newEncoder.requestKeyframe()
    }

    override fun onCameraStarted(summary: CameraSummary) {
        controls = camera?.setControls(controls) ?: controls
        SessionStatus.snapshot = SessionStatus.snapshot.copy(
            facing = summary.facing.name.lowercase(),
            zoom = controls.zoom,
            exposure = controls.exposure,
            torch = controls.torch,
        )
        publish(SessionState.STREAMING, "Streaming ${requestedPreset.label} from the ${summary.facing.name.lowercase()} camera.")
        startForeground(
            NOTIFICATION_ID,
            notification("Streaming ${requestedPreset.label} securely"),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA,
        )
    }

    override fun onCameraError(error: Throwable) {
        sendMetadata(MessageType.ERROR, """{"schema":1,"code":"camera","reason":${error.message.jsonString()}}""")
        publish(SessionState.ERROR, error.message ?: "Camera failed.")
        stopSelf()
    }

    override fun onFormat(format: ActualEncoderFormat) {
        SessionStatus.snapshot = SessionStatus.snapshot.copy(
            encoder = format.name,
            width = format.width,
            height = format.height,
            fps = format.fps,
            bitrate = format.bitrate,
        )
    }

    override fun onAccessUnit(unit: EncodedAccessUnit) {
        if (!running.get() || !connectionActive.get()) return
        val type = if (unit.isCodecConfig) MessageType.VIDEO_CONFIG else MessageType.VIDEO_FRAME
        var flags = if (unit.isCodecConfig) MessageFlags.CONFIG else 0
        if (unit.isKeyframe) flags = flags or MessageFlags.KEYFRAME
        if (unit.isEndOfStream) flags = flags or MessageFlags.END_OF_STREAM
        val message = WireMessage(
            WireHeader(
                type = type.wireValue,
                flags = flags,
                streamId = streamId,
                sequence = sequence.getAndIncrement().toULong(),
                ptsUs = unit.ptsUs.coerceAtLeast(0).toULong(),
            ),
            unit.bytes,
        )
        if (!queue.offer(message)) {
            queue.poll()
            if (!queue.offer(message)) dropped += 1
            dropped += 1
        }
    }

    override fun onEncoderError(error: Throwable) {
        sendMetadata(MessageType.ERROR, """{"schema":1,"code":"encoder","reason":${error.message.jsonString()}}""")
        publish(SessionState.ERROR, error.message ?: "Encoder failed.")
        stopSelf()
    }

    private fun writerLoop() {
        try {
            while (running.get() && connectionActive.get()) {
                val message = queue.poll(2, TimeUnit.SECONDS)
                if (message == null) {
                    sendMetadata(MessageType.PING, "{}", 0)
                    continue
                }
                send(message)
                if (message.header.type == MessageType.VIDEO_FRAME.wireValue) frames += 1
                bytes += message.payload.size
                if (frames % 30L == 0L) publishStats()
            }
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        } catch (error: Throwable) {
            if (running.get()) publish(SessionState.RECOVERING, error.message ?: "Connection lost; reconnecting…")
            connectionActive.set(false)
            runCatching { client?.close() }
        }
    }

    @Synchronized
    private fun send(message: WireMessage) {
        val bytes = WireCodec.serialize(message)
        requireNotNull(output) { "desktop connection is not ready" }.apply {
            write(bytes)
            flush()
        }
    }

    private fun sendMetadata(type: MessageType, json: String, flags: Int = MessageFlags.REQUIRED) {
        if (output == null) return
        send(
            WireMessage(
                WireHeader(type = type.wireValue, flags = flags, streamId = streamId, sequence = sequence.getAndIncrement().toULong()),
                json.encodeToByteArray(),
            ),
        )
    }

    private fun capabilitiesJson(): String {
        val summaries = runCatching { CameraEngine(this, this).use { it.enumerate() } }.getOrDefault(emptyList())
        val cameras = summaries.joinToString(",") { cameraSummary ->
            """{"id":${cameraSummary.id.jsonString()},"facing":${cameraSummary.facing.name.lowercase().jsonString()},"zoomMin":${cameraSummary.zoomMinimum},"zoomMax":${cameraSummary.zoomMaximum},"torch":${cameraSummary.supportsTorch},"sensorOrientation":${cameraSummary.sensorOrientation},"presets":[${cameraSummary.presets.joinToString(",") { it.label.jsonString() }}]}"""
        }
        return """{"schema":1,"cameras":[$cameras]}"""
    }

    private fun publishStats() {
        SessionStatus.snapshot = SessionStatus.snapshot.copy(frames = frames, bytes = bytes, dropped = dropped)
        sendMetadata(MessageType.STATS, """{"schema":1,"frames":$frames,"bytes":$bytes,"dropped":$dropped,"queue":${queue.size}}""", 0)
    }

    private fun publish(state: SessionState, detail: String) {
        SessionStatus.snapshot = SessionStatus.snapshot.copy(state = state, detail = detail, frames = frames, bytes = bytes, dropped = dropped)
    }

    @Synchronized
    private fun applyControls(requested: CameraControlState) {
        controls = camera?.setControls(requested) ?: requested
        SessionStatus.snapshot = SessionStatus.snapshot.copy(
            zoom = controls.zoom,
            exposure = controls.exposure,
            torch = controls.torch,
        )
        sendMetadata(
            MessageType.CONTROL_ACK,
            """{"schema":1,"zoom":${controls.zoom},"exposure":${controls.exposure},"torch":${controls.torch}}""",
            MessageFlags.ACKNOWLEDGEMENT,
        )
    }

    @Synchronized
    private fun switchFacing(requested: Facing) {
        if (facing == requested || SessionStatus.snapshot.state != SessionState.STREAMING) return
        facing = requested
        publish(SessionState.STARTING, "Switching camera…")
        runCatching { startCapture(requestedPreset) }
            .onFailure(::onCameraError)
    }

    private fun notification(text: String): Notification {
        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, CameraStreamingService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_openlens)
            .setContentTitle("OpenLens camera")
            .setContentText(text)
            .setContentIntent(openIntent)
            .setOngoing(true)
            .addAction(0, "Stop", stopIntent)
            .build()
    }

    override fun onDestroy() {
        running.set(false)
        connectionActive.set(false)
        queue.offer(WireMessage(WireHeader(type = MessageType.END_STREAM.wireValue)))
        camera?.close()
        camera = null
        encoder?.close()
        encoder = null
        runCatching { client?.close() }
        output = null
        client = null
        publish(SessionState.STOPPED, "Camera stopped.")
        if (activeInstance === this) activeInstance = null
        super.onDestroy()
    }

    companion object {
        const val ACTION_STOP = "dev.openlens.app.STOP"
        const val ACTION_WIFI = "dev.openlens.app.WIFI"
        private const val CHANNEL_ID = "openlens_camera"
        private const val NOTIFICATION_ID = 42
        @Volatile
        private var activeInstance: CameraStreamingService? = null

        fun updateControls(zoom: Float, exposure: Int, torch: Boolean) {
            activeInstance?.applyControls(CameraControlState(zoom, exposure, torch))
        }

        fun setFacing(front: Boolean) {
            activeInstance?.switchFacing(if (front) Facing.FRONT else Facing.BACK)
        }

        fun tapFocus(normalizedX: Float, normalizedY: Float) {
            activeInstance?.camera?.tapFocus(normalizedX, normalizedY)
        }

    }
}

private fun String?.jsonString(): String = buildString {
    append('"')
    this@jsonString.orEmpty().forEach { character ->
        when (character) {
            '"' -> append("\\\"")
            '\\' -> append("\\\\")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(character)
        }
    }
    append('"')
}

private fun String.intValue(name: String, fallback: Int): Int =
    Regex("\\\"${Regex.escape(name)}\\\"\\s*:\\s*(-?\\d+)")
        .find(this)?.groupValues?.getOrNull(1)?.toIntOrNull() ?: fallback

private fun String.floatValue(name: String, fallback: Float): Float =
    Regex("\\\"${Regex.escape(name)}\\\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)")
        .find(this)?.groupValues?.getOrNull(1)?.toFloatOrNull() ?: fallback

private fun String.booleanValue(name: String, fallback: Boolean): Boolean =
    Regex("\\\"${Regex.escape(name)}\\\"\\s*:\\s*(true|false)")
        .find(this)?.groupValues?.getOrNull(1)?.toBooleanStrictOrNull() ?: fallback

private fun String.stringValue(name: String, fallback: String): String =
    Regex("\\\"${Regex.escape(name)}\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"")
        .find(this)?.groupValues?.getOrNull(1) ?: fallback
