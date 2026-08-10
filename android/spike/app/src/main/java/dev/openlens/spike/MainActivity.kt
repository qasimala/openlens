package dev.openlens.spike

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.Locale
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var cameraManager: CameraManager
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusView: TextView
    private lateinit var probeButton: Button
    private lateinit var cameraThread: HandlerThread
    private lateinit var cameraHandler: Handler
    private lateinit var cameraExecutor: Executor

    @Volatile
    private var previewReady = false

    @Volatile
    private var activeProbe: ProbeSession? = null

    private var autoProbeRequested = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        autoProbeRequested = intent.getBooleanExtra(EXTRA_AUTO_PROBE, false)
        cameraManager = getSystemService(CameraManager::class.java)
        cameraThread = HandlerThread("phase0-camera").also { it.start() }
        cameraHandler = Handler(cameraThread.looper)
        cameraExecutor = Executor { command -> cameraHandler.post(command) }
        buildUi()

        appendStatus("Phase 0 spike ready. Grant camera permission, scan, then run the 30-second probe.")
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), CAMERA_PERMISSION_REQUEST)
        } else {
            scanCapabilities()
        }
    }

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.rgb(7, 26, 20))
        }

        surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@MainActivity)
        }
        root.addView(
            surfaceView,
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.35f),
        )

        val controls = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 18, 24, 18)
            setBackgroundColor(Color.rgb(16, 34, 27))
        }

        val title = TextView(this).apply {
            text = getString(R.string.title)
            textSize = 22f
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, 12)
        }
        controls.addView(title)

        val scanButton = Button(this).apply {
            text = getString(R.string.scan_capabilities)
            setOnClickListener { scanCapabilities() }
        }
        controls.addView(scanButton)

        probeButton = Button(this).apply {
            text = getString(R.string.run_probe)
            isEnabled = false
            setOnClickListener { startProbe() }
        }
        controls.addView(probeButton)

        val stopButton = Button(this).apply {
            text = getString(R.string.stop_probe)
            setOnClickListener { activeProbe?.stop("user") }
        }
        controls.addView(stopButton)

        val scroll = ScrollView(this)
        statusView = TextView(this).apply {
            textSize = 12f
            setTextColor(Color.rgb(218, 242, 232))
            setTextIsSelectable(true)
            gravity = Gravity.START
        }
        scroll.addView(statusView)
        controls.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        root.addView(
            controls,
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f),
        )
        setContentView(root)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == CAMERA_PERMISSION_REQUEST &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            appendStatus("Camera permission granted.")
            scanCapabilities()
        } else if (requestCode == CAMERA_PERMISSION_REQUEST) {
            appendStatus("Camera permission denied; the probe cannot run.")
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        holder.setFixedSize(PROBE_WIDTH, PROBE_HEIGHT)
        previewReady = true
        probeButton.isEnabled = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        appendStatus("Preview surface ready.")
        if (autoProbeRequested && probeButton.isEnabled) {
            autoProbeRequested = false
            surfaceView.post(::startProbe)
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        appendStatus("Preview surface: ${width}x$height format=$format")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        previewReady = false
        probeButton.isEnabled = false
        activeProbe?.stop("preview surface destroyed")
    }

    private fun scanCapabilities() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            appendStatus("Grant camera permission before scanning.")
            requestPermissions(arrayOf(Manifest.permission.CAMERA), CAMERA_PERMISSION_REQUEST)
            return
        }

        appendStatus("Scanning Camera2 and MediaCodec capabilities…")
        thread(name = "phase0-capability-scan") {
            runCatching { buildCapabilityReport() }
                .onSuccess { report ->
                    val output = File(filesDir, CAPABILITY_FILE)
                    output.writeText(report.toString(2))
                    logInChunks("CAPABILITIES ${report.toString()}")
                    runOnUiThread {
                        appendStatus(
                            "Capability report saved: ${output.absolutePath}\n" +
                                summarizeCapabilityReport(report),
                        )
                    }
                }
                .onFailure { error ->
                    Log.e(TAG, "Capability scan failed", error)
                    runOnUiThread { appendStatus("Capability scan failed: ${error.message}") }
                }
        }
    }

    private fun buildCapabilityReport(): JSONObject {
        val root = JSONObject()
        root.put("schema_version", 1)
        root.put("created_epoch_ms", System.currentTimeMillis())
        root.put(
            "device",
            JSONObject()
                .put("manufacturer", Build.MANUFACTURER)
                .put("model", Build.MODEL)
                .put("device", Build.DEVICE)
                .put("display_build", Build.DISPLAY)
                .put("android_release", Build.VERSION.RELEASE)
                .put("sdk", Build.VERSION.SDK_INT)
                .put("security_patch", Build.VERSION.SECURITY_PATCH)
                .put("soc_model", Build.SOC_MODEL),
        )

        val cameras = JSONArray()
        for (cameraId in cameraManager.cameraIdList) {
            val c = cameraManager.getCameraCharacteristics(cameraId)
            val camera = JSONObject()
            camera.put("id", cameraId)
            camera.put("facing", facingName(c[CameraCharacteristics.LENS_FACING]))
            camera.put("sensor_orientation", c[CameraCharacteristics.SENSOR_ORIENTATION])
            camera.put("hardware_level", hardwareLevelName(c[CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL]))
            camera.put("flash", c[CameraCharacteristics.FLASH_INFO_AVAILABLE] == true)
            camera.put("physical_ids", jsonArray(c.physicalCameraIds.sorted()))
            camera.put("focal_lengths_mm", jsonArray(c[CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS]))
            camera.put("apertures", jsonArray(c[CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES]))
            camera.put("zoom_ratio_range", rangeJson(c[CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE]))
            camera.put("ae_fps_ranges", rangesJson(c[CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES]))
            camera.put("exposure_compensation_range", rangeJson(c[CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE]))
            camera.put("exposure_compensation_step", c[CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP]?.toString())
            camera.put(
                "video_stabilization_modes",
                jsonArray(c[CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES]),
            )
            camera.put(
                "capabilities",
                jsonArray(
                    c[CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES]
                        ?.map(::capabilityName)
                        ?.sorted(),
                ),
            )

            val map = c[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP]
            val codecSizes = JSONArray()
            map?.getOutputSizes(MediaCodec::class.java)
                ?.sortedWith(compareByDescending<Size> { it.width * it.height }.thenByDescending { it.width })
                ?.forEach { size ->
                    if (size.width <= MAX_REPORTED_WIDTH && size.height <= MAX_REPORTED_HEIGHT) {
                        val minDuration = map.getOutputMinFrameDuration(MediaCodec::class.java, size)
                        val maxFps = if (minDuration > 0) 1_000_000_000.0 / minDuration else null
                        codecSizes.put(
                            JSONObject()
                                .put("width", size.width)
                                .put("height", size.height)
                                .put("min_frame_duration_ns", minDuration)
                                .put("derived_max_fps", maxFps),
                        )
                    }
                }
            camera.put("media_codec_output_sizes", codecSizes)
            cameras.put(camera)
        }
        root.put("cameras", cameras)

        val encoders = JSONArray()
        val codecList = MediaCodecList(MediaCodecList.REGULAR_CODECS)
        codecList.codecInfos
            .filter { info -> info.isEncoder && info.supportedTypes.any { it.equals(MIME_AVC, ignoreCase = true) } }
            .forEach { info ->
                val caps = info.getCapabilitiesForType(MIME_AVC)
                val videoCaps = requireNotNull(caps.videoCapabilities) {
                    "AVC codec ${info.name} did not expose video capabilities"
                }
                val encoderCaps = requireNotNull(caps.encoderCapabilities) {
                    "AVC encoder ${info.name} did not expose encoder capabilities"
                }
                val codec = JSONObject()
                    .put("name", info.name)
                    .put("canonical_name", info.canonicalName)
                    .put("hardware_accelerated", info.isHardwareAccelerated)
                    .put("software_only", info.isSoftwareOnly)
                    .put("vendor", info.isVendor)
                    .put("max_instances", caps.maxSupportedInstances)
                    .put("width_alignment", videoCaps.widthAlignment)
                    .put("height_alignment", videoCaps.heightAlignment)
                    .put("supported_widths", videoCaps.supportedWidths.toString())
                    .put("supported_heights", videoCaps.supportedHeights.toString())
                    .put("bitrate_range", videoCaps.bitrateRange.toString())
                    .put("supports_cbr", encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                    .put("supports_vbr", encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR))
                    .put("supports_cq", encoderCaps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ))
                    .put(
                        "color_formats",
                        jsonArray(caps.colorFormats),
                    )
                codec.put(
                    "profile_levels",
                    JSONArray().also { profiles ->
                        caps.profileLevels.forEach { profile ->
                            profiles.put(JSONObject().put("profile", profile.profile).put("level", profile.level))
                        }
                    },
                )
                codec.put(
                    "fps_1920x1080",
                    runCatching { videoCaps.getSupportedFrameRatesFor(PROBE_WIDTH, PROBE_HEIGHT).toString() }
                        .getOrElse { "unsupported: ${it.javaClass.simpleName}" },
                )
                codec.put(
                    "fps_1280x720",
                    runCatching { videoCaps.getSupportedFrameRatesFor(1280, 720).toString() }
                        .getOrElse { "unsupported: ${it.javaClass.simpleName}" },
                )
                encoders.put(codec)
            }
        root.put("avc_encoders", encoders)

        val power = getSystemService(PowerManager::class.java)
        root.put("thermal_status", power.currentThermalStatus)
        return root
    }

    private fun summarizeCapabilityReport(root: JSONObject): String {
        val lines = mutableListOf<String>()
        val cameras = root.getJSONArray("cameras")
        lines += "Public Camera2 devices: ${cameras.length()}"
        for (index in 0 until cameras.length()) {
            val camera = cameras.getJSONObject(index)
            lines += "• id=${camera.getString("id")} ${camera.getString("facing")}" +
                " focal=${camera.getJSONArray("focal_lengths_mm")}" +
                " zoom=${camera.opt("zoom_ratio_range")}" +
                " physical=${camera.getJSONArray("physical_ids")}" +
                " sizes=${camera.getJSONArray("media_codec_output_sizes").length()}"
        }
        val encoders = root.getJSONArray("avc_encoders")
        lines += "AVC encoders: ${encoders.length()}"
        for (index in 0 until encoders.length()) {
            val codec = encoders.getJSONObject(index)
            lines += "• ${codec.getString("name")} hw=${codec.getBoolean("hardware_accelerated")}" +
                " CBR=${codec.getBoolean("supports_cbr")}" +
                " 1080p=${codec.getString("fps_1920x1080")}"
        }
        return lines.joinToString("\n")
    }

    private fun startProbe() {
        if (activeProbe != null) {
            appendStatus("A probe is already active.")
            return
        }
        if (!previewReady || !surfaceView.holder.surface.isValid) {
            appendStatus("Preview surface is not ready.")
            return
        }
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            appendStatus("Camera permission is required.")
            return
        }

        appendStatus("Starting 30-second 1920x1080@30 encoder probe…")
        probeButton.isEnabled = false
        val probe = ProbeSession()
        activeProbe = probe
        probe.start()
    }

    private inner class ProbeSession {
        private val stopping = AtomicBoolean(false)
        private var cameraDevice: CameraDevice? = null
        private var captureSession: CameraCaptureSession? = null
        private lateinit var codec: MediaCodec
        private lateinit var encoderSurface: Surface
        private lateinit var muxer: MediaMuxer
        private lateinit var outputFile: File
        private var muxerStarted = false
        private var trackIndex = -1
        private var startedNs = 0L
        private var firstPtsUs = -1L
        private var lastPtsUs = -1L
        private var encodedFrames = 0L
        private var keyframes = 0L
        private var encodedBytes = 0L
        private var encoderName = ""
        private var selectedCameraId = ""
        private var actualFormat: MediaFormat? = null
        private val encoderOutputLatenciesMs = mutableListOf<Double>()

        fun start() {
            runCatching {
                selectedCameraId = selectBackCamera()
                prepareEncoder()
                openCamera()
            }.onFailure(::fail)
        }

        private fun selectBackCamera(): String = cameraManager.cameraIdList.firstOrNull { id ->
            cameraManager.getCameraCharacteristics(id)[CameraCharacteristics.LENS_FACING] ==
                CameraCharacteristics.LENS_FACING_BACK
        } ?: error("No rear Camera2 device found")

        private fun prepareEncoder() {
            val codecInfo = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.firstOrNull { info ->
                info.isEncoder &&
                    info.isHardwareAccelerated &&
                    info.supportedTypes.any { it.equals(MIME_AVC, ignoreCase = true) }
            } ?: error("No hardware AVC encoder found")

            encoderName = codecInfo.name
            val capabilities = codecInfo.getCapabilitiesForType(MIME_AVC)
            val encoderCapabilities = requireNotNull(capabilities.encoderCapabilities) {
                "AVC encoder ${codecInfo.name} did not expose encoder capabilities"
            }
            val profile = when {
                capabilities.profileLevels.any { it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileMain } ->
                    MediaCodecInfo.CodecProfileLevel.AVCProfileMain
                else -> MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline
            }

            val format = MediaFormat.createVideoFormat(MIME_AVC, PROBE_WIDTH, PROBE_HEIGHT).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, PROBE_BITRATE)
                setInteger(MediaFormat.KEY_FRAME_RATE, PROBE_FPS)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                setInteger(MediaFormat.KEY_PROFILE, profile)
                if (encoderCapabilities.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
                    setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
                }
            }

            codec = MediaCodec.createByCodecName(encoderName)
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            encoderSurface = codec.createInputSurface()
            outputFile = File(filesDir, PROBE_VIDEO_FILE)
            if (outputFile.exists()) outputFile.delete()
            muxer = MediaMuxer(outputFile.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            codec.start()
            startedNs = System.nanoTime()
            thread(name = "phase0-codec-drain") { drainEncoder() }
        }

        private fun openCamera() {
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                fail(SecurityException("Camera permission was revoked before open"))
                return
            }
            cameraManager.openCamera(
                selectedCameraId,
                cameraExecutor,
                object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        if (stopping.get()) {
                            camera.close()
                            return
                        }
                        cameraDevice = camera
                        createSession(camera)
                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        camera.close()
                        fail(IllegalStateException("Camera disconnected"))
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        camera.close()
                        fail(IllegalStateException("Camera error $error"))
                    }
                },
            )
        }

        private fun createSession(camera: CameraDevice) {
            val previewSurface = surfaceView.holder.surface
            check(previewSurface.isValid) { "Preview surface became invalid" }
            val outputs = listOf(OutputConfiguration(previewSurface), OutputConfiguration(encoderSurface))
            val configuration = SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR,
                outputs,
                cameraExecutor,
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        if (stopping.get()) {
                            session.close()
                            return
                        }
                        captureSession = session
                        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(previewSurface)
                            addTarget(encoderSurface)
                            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                            val characteristics = cameraManager.getCameraCharacteristics(selectedCameraId)
                            val fpsRanges = characteristics[
                                CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                            ]
                            if (fpsRanges?.contains(Range(PROBE_FPS, PROBE_FPS)) == true) {
                                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(PROBE_FPS, PROBE_FPS))
                            }
                        }.build()
                        session.setRepeatingRequest(request, null, cameraHandler)
                        runOnUiThread {
                            appendStatus("Probe streaming from camera $selectedCameraId using $encoderName")
                        }
                        cameraHandler.postDelayed({ stop("30-second duration complete") }, PROBE_DURATION_MS)
                    }

                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        session.close()
                        fail(IllegalStateException("Camera session configuration failed"))
                    }
                },
            )
            camera.createCaptureSession(configuration)
        }

        private fun drainEncoder() {
            val info = MediaCodec.BufferInfo()
            var eosSeen = false
            var stopDeadlineNs = Long.MAX_VALUE
            try {
                while (!eosSeen && System.nanoTime() < stopDeadlineNs) {
                    val index = codec.dequeueOutputBuffer(info, 10_000)
                    when {
                        index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            check(!muxerStarted) { "Encoder format changed twice" }
                            actualFormat = codec.outputFormat
                            trackIndex = muxer.addTrack(codec.outputFormat)
                            muxer.start()
                            muxerStarted = true
                            logInChunks("ENCODER_FORMAT ${codec.outputFormat}")
                        }
                        index >= 0 -> {
                            val buffer = codec.getOutputBuffer(index)
                                ?: error("Encoder returned a null output buffer")
                            if (info.size > 0 && muxerStarted &&
                                info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0
                            ) {
                                buffer.position(info.offset)
                                buffer.limit(info.offset + info.size)
                                muxer.writeSampleData(trackIndex, buffer, info)
                                if (firstPtsUs < 0) firstPtsUs = info.presentationTimeUs
                                lastPtsUs = info.presentationTimeUs
                                encodedFrames++
                                encodedBytes += info.size
                                if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) keyframes++
                                val outputLatencyMs =
                                    (System.nanoTime() / 1_000.0 - info.presentationTimeUs) / 1_000.0
                                if (outputLatencyMs in 0.0..10_000.0) {
                                    encoderOutputLatenciesMs += outputLatencyMs
                                }
                            }
                            eosSeen = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                            codec.releaseOutputBuffer(index, false)
                        }
                    }
                    if (stopping.get() && stopDeadlineNs == Long.MAX_VALUE) {
                        stopDeadlineNs = System.nanoTime() + 3_000_000_000L
                    }
                }
            } catch (error: Throwable) {
                Log.e(TAG, "Encoder drain failed", error)
            } finally {
                finishProbe(eosSeen)
            }
        }

        fun stop(reason: String) {
            if (!stopping.compareAndSet(false, true)) return
            Log.i(TAG, "Stopping probe: $reason")
            cameraHandler.post {
                runCatching { captureSession?.stopRepeating() }
                runCatching { captureSession?.abortCaptures() }
                captureSession?.close()
                captureSession = null
                cameraDevice?.close()
                cameraDevice = null
                runCatching { codec.signalEndOfInputStream() }
            }
            runOnUiThread { appendStatus("Stopping probe: $reason") }
        }

        private fun fail(error: Throwable) {
            Log.e(TAG, "Probe failed", error)
            runOnUiThread { appendStatus("Probe failed: ${error.message}") }
            stop("failure")
        }

        private fun finishProbe(eosSeen: Boolean) {
            runCatching { if (muxerStarted) muxer.stop() }
            runCatching { muxer.release() }
            runCatching { codec.stop() }
            runCatching { codec.release() }
            runCatching { encoderSurface.release() }

            val elapsedSeconds = (System.nanoTime() - startedNs) / 1_000_000_000.0
            val mediaSeconds = if (firstPtsUs >= 0 && lastPtsUs >= firstPtsUs) {
                (lastPtsUs - firstPtsUs) / 1_000_000.0
            } else {
                0.0
            }
            val sortedLatencies = encoderOutputLatenciesMs.sorted()
            val latencySummary = JSONObject()
                .put("samples", sortedLatencies.size)
                .put("min", sortedLatencies.firstOrNull())
                .put("p50", percentile(sortedLatencies, 0.50))
                .put("p95", percentile(sortedLatencies, 0.95))
                .put("p99", percentile(sortedLatencies, 0.99))
                .put("max", sortedLatencies.lastOrNull())
            val result = JSONObject()
                .put("schema_version", 1)
                .put("camera_id", selectedCameraId)
                .put("encoder", encoderName)
                .put("requested_width", PROBE_WIDTH)
                .put("requested_height", PROBE_HEIGHT)
                .put("requested_fps", PROBE_FPS)
                .put("requested_bitrate", PROBE_BITRATE)
                .put("elapsed_seconds", elapsedSeconds)
                .put("media_seconds", mediaSeconds)
                .put("encoded_frames", encodedFrames)
                .put("keyframes", keyframes)
                .put("encoded_bytes", encodedBytes)
                .put("derived_fps", if (mediaSeconds > 0) encodedFrames / mediaSeconds else 0.0)
                .put("derived_bitrate_bps", if (mediaSeconds > 0) (encodedBytes * 8.0 / mediaSeconds).roundToInt() else 0)
                .put("eos_seen", eosSeen)
                .put("actual_format", actualFormat?.toString())
                .put("camera_surface_to_encoder_output_latency_ms", latencySummary)
                .put("video_file", outputFile.name)
            File(filesDir, PROBE_RESULT_FILE).writeText(result.toString(2))
            logInChunks("PROBE_RESULT ${result}")

            runOnUiThread {
                activeProbe = null
                probeButton.isEnabled = previewReady
                appendStatus(
                    String.format(
                        Locale.US,
                        "Probe complete: %.2fs media, %d frames, %.2f fps, %.2f Mbps, %d keyframes, EOS=%s\nSaved %s and %s",
                        mediaSeconds,
                        encodedFrames,
                        if (mediaSeconds > 0) encodedFrames / mediaSeconds else 0.0,
                        if (mediaSeconds > 0) encodedBytes * 8.0 / mediaSeconds / 1_000_000.0 else 0.0,
                        keyframes,
                        eosSeen,
                        outputFile.absolutePath,
                        PROBE_RESULT_FILE,
                    ),
                )
            }
        }

        private fun percentile(sorted: List<Double>, fraction: Double): Double? {
            if (sorted.isEmpty()) return null
            val index = ((sorted.lastIndex * fraction).roundToInt()).coerceIn(sorted.indices)
            return sorted[index]
        }
    }

    private fun appendStatus(message: String) {
        val timestamp = String.format(Locale.US, "%.3f", System.nanoTime() / 1_000_000_000.0)
        statusView.append("[$timestamp] $message\n")
        (statusView.parent as? ScrollView)?.post { (statusView.parent as ScrollView).fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun logInChunks(message: String) {
        var offset = 0
        while (offset < message.length) {
            val end = minOf(offset + LOG_CHUNK_SIZE, message.length)
            Log.i(TAG, message.substring(offset, end))
            offset = end
        }
    }

    private fun facingName(value: Int?): String = when (value) {
        CameraCharacteristics.LENS_FACING_BACK -> "BACK"
        CameraCharacteristics.LENS_FACING_FRONT -> "FRONT"
        CameraCharacteristics.LENS_FACING_EXTERNAL -> "EXTERNAL"
        else -> "UNKNOWN($value)"
    }

    private fun hardwareLevelName(value: Int?): String = when (value) {
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY -> "LEGACY"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED -> "LIMITED"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL -> "FULL"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_3 -> "LEVEL_3"
        CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL -> "EXTERNAL"
        else -> "UNKNOWN($value)"
    }

    private fun capabilityName(value: Int): String = when (value) {
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE -> "BACKWARD_COMPATIBLE"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR -> "MANUAL_SENSOR"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING -> "MANUAL_POST_PROCESSING"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_RAW -> "RAW"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING -> "PRIVATE_REPROCESSING"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS -> "READ_SENSOR_SETTINGS"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE -> "BURST_CAPTURE"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING -> "YUV_REPROCESSING"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO -> "CONSTRAINED_HIGH_SPEED_VIDEO"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA -> "LOGICAL_MULTI_CAMERA"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_DYNAMIC_RANGE_TEN_BIT -> "DYNAMIC_RANGE_TEN_BIT"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_STREAM_USE_CASE -> "STREAM_USE_CASE"
        CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_COLOR_SPACE_PROFILES -> "COLOR_SPACE_PROFILES"
        else -> "CAPABILITY_$value"
    }

    private fun jsonArray(values: Collection<Any?>?): JSONArray = JSONArray().also { array ->
        values?.forEach(array::put)
    }

    private fun jsonArray(values: IntArray?): JSONArray = JSONArray().also { array ->
        values?.forEach(array::put)
    }

    private fun jsonArray(values: FloatArray?): JSONArray = JSONArray().also { array ->
        values?.forEach(array::put)
    }

    private fun <T : Comparable<T>> rangeJson(range: Range<T>?): Any =
        range?.let { JSONObject().put("lower", it.lower).put("upper", it.upper) } ?: JSONObject.NULL

    private fun rangesJson(ranges: Array<Range<Int>>?): JSONArray = JSONArray().also { array ->
        ranges?.forEach { range -> array.put(rangeJson(range)) }
    }

    override fun onDestroy() {
        activeProbe?.stop("activity destroyed")
        cameraThread.quitSafely()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "OpenLensSpike"
        private const val CAMERA_PERMISSION_REQUEST = 42
        private const val MIME_AVC = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val PROBE_WIDTH = 1920
        private const val PROBE_HEIGHT = 1080
        private const val PROBE_FPS = 30
        private const val PROBE_BITRATE = 8_000_000
        private const val PROBE_DURATION_MS = 30_000L
        private const val MAX_REPORTED_WIDTH = 4096
        private const val MAX_REPORTED_HEIGHT = 4096
        private const val LOG_CHUNK_SIZE = 3_500
        private const val CAPABILITY_FILE = "phase0-capabilities.json"
        private const val PROBE_RESULT_FILE = "phase0-probe.json"
        private const val PROBE_VIDEO_FILE = "phase0-1080p30.mp4"
        private const val EXTRA_AUTO_PROBE = "auto_probe"
    }
}
