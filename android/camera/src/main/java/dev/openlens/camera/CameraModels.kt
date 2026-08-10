// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.SessionConfiguration
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.view.Surface
import java.util.concurrent.Executor

data class VideoPreset(val width: Int, val height: Int, val fps: Int, val bitrate: Int) {
    val label: String get() = "${height}p$fps"
}

data class CameraSummary(
    val id: String,
    val facing: Facing,
    val zoomMinimum: Float,
    val zoomMaximum: Float,
    val supportsTorch: Boolean,
    val sensorOrientation: Int,
    val presets: List<VideoPreset>,
)

data class CameraControlState(
    val zoom: Float = 1f,
    val exposure: Int = 0,
    val torch: Boolean = false,
)

enum class Facing { FRONT, BACK, EXTERNAL }

object CertifiedPresets {
    val HD_720P30 = VideoPreset(1280, 720, 30, 4_000_000)
    val FULL_HD_1080P30 = VideoPreset(1920, 1080, 30, 8_000_000)

    fun choose(requested: VideoPreset, available: Collection<VideoPreset>): VideoPreset? =
        available.firstOrNull { it.width == requested.width && it.height == requested.height && it.fps == requested.fps }
            ?: available.firstOrNull {
                it.width == HD_720P30.width && it.height == HD_720P30.height && it.fps == HD_720P30.fps
            }
}

interface CameraListener {
    fun onCameraStarted(summary: CameraSummary)
    fun onCameraError(error: Throwable)
}

class CameraEngine(context: Context, private val listener: CameraListener) : AutoCloseable {
    private val manager = context.getSystemService(CameraManager::class.java)
    private val applicationContext = context.applicationContext
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var builder: CaptureRequest.Builder? = null
    private var characteristics: CameraCharacteristics? = null
    private var controls = CameraControlState()

    fun enumerate(): List<CameraSummary> = manager.cameraIdList.mapNotNull(::summaryFor)

    @SuppressLint("MissingPermission")
    @Synchronized
    fun start(facing: Facing, preset: VideoPreset, surfaces: List<Surface>) {
        check(applicationContext.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            "camera permission is not granted"
        }
        check(device == null) { "camera already started" }
        require(surfaces.isNotEmpty()) { "camera requires an output surface" }
        val selected = enumerate().firstOrNull { it.facing == facing && CertifiedPresets.choose(preset, it.presets) == preset }
            ?: throw IllegalArgumentException("${preset.label} is unavailable on the selected camera")
        val worker = HandlerThread("OpenLensCamera").also(HandlerThread::start)
        thread = worker
        handler = Handler(worker.looper)
        characteristics = manager.getCameraCharacteristics(selected.id)
        manager.openCamera(selected.id, Executor { command -> handler?.post(command) }, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                device = camera
                createSession(camera, selected, preset, surfaces)
            }

            override fun onDisconnected(camera: CameraDevice) {
                camera.close()
                device = null
                listener.onCameraError(IllegalStateException("camera disconnected"))
            }

            override fun onError(camera: CameraDevice, error: Int) {
                camera.close()
                device = null
                listener.onCameraError(IllegalStateException("camera error $error"))
            }
        })
    }

    private fun createSession(camera: CameraDevice, summary: CameraSummary, preset: VideoPreset, surfaces: List<Surface>) {
        val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
        surfaces.forEach(request::addTarget)
        request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, chooseFpsRange(preset.fps))
        request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        val stabilization = characteristics?.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)
        if (stabilization?.contains(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON) == true) {
            request.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON)
        }
        builder = request
        applyControls(request)
        val executor = Executor { command -> handler?.post(command) }
        val configuration = SessionConfiguration(
            SessionConfiguration.SESSION_REGULAR,
            surfaces.map(::OutputConfiguration),
            executor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(captureSession: CameraCaptureSession) {
                    session = captureSession
                    captureSession.setRepeatingRequest(request.build(), null, handler)
                    listener.onCameraStarted(summary)
                }

                override fun onConfigureFailed(captureSession: CameraCaptureSession) {
                    captureSession.close()
                    listener.onCameraError(IllegalStateException("camera output configuration failed"))
                }
            },
        )
        camera.createCaptureSession(configuration)
    }

    @Synchronized
    fun setControls(value: CameraControlState): CameraControlState {
        val details = characteristics
        if (details == null) {
            controls = value
            return controls
        }
        val zoomRange = details.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) ?: Range(1f, 1f)
        val exposureRange = details.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)
        val torchSupported = details.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
        controls = value.copy(
            zoom = value.zoom.coerceIn(zoomRange.lower, zoomRange.upper),
            exposure = value.exposure.coerceIn(exposureRange.lower, exposureRange.upper),
            torch = value.torch && torchSupported,
        )
        builder?.let { request ->
            applyControls(request)
            session?.setRepeatingRequest(request.build(), null, handler)
        }
        return controls
    }

    @Synchronized
    fun tapFocus(normalizedX: Float, normalizedY: Float): Boolean {
        val details = characteristics ?: return false
        val request = builder ?: return false
        val captureSession = session ?: return false
        val active = details.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return false
        val regionWidth = (active.width() / 10).coerceAtLeast(1)
        val regionHeight = (active.height() / 10).coerceAtLeast(1)
        val centerX = active.left + (active.width() * normalizedX.coerceIn(0f, 1f)).toInt()
        val centerY = active.top + (active.height() * normalizedY.coerceIn(0f, 1f)).toInt()
        val left = (centerX - regionWidth / 2).coerceIn(active.left, active.right - regionWidth)
        val top = (centerY - regionHeight / 2).coerceIn(active.top, active.bottom - regionHeight)
        val region = MeteringRectangle(Rect(left, top, left + regionWidth, top + regionHeight),
            MeteringRectangle.METERING_WEIGHT_MAX)
        if ((details.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0) > 0) {
            request.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
        }
        if ((details.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0) > 0) {
            request.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(region))
        }
        request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
        request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
        captureSession.capture(request.build(), null, handler)
        request.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
        request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        captureSession.setRepeatingRequest(request.build(), null, handler)
        return true
    }

    private fun applyControls(request: CaptureRequest.Builder) {
        request.set(CaptureRequest.CONTROL_ZOOM_RATIO, controls.zoom)
        request.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, controls.exposure)
        request.set(CaptureRequest.FLASH_MODE, if (controls.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF)
    }

    private fun chooseFpsRange(fps: Int): Range<Int> {
        val ranges = characteristics?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
        return ranges.firstOrNull { it.lower == fps && it.upper == fps }
            ?: ranges.filter { it.lower <= fps && it.upper >= fps }.minByOrNull { it.upper - it.lower }
            ?: Range(fps, fps)
    }

    private fun summaryFor(id: String): CameraSummary? {
        val details = manager.getCameraCharacteristics(id)
        val facing = when (details.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> Facing.FRONT
            CameraCharacteristics.LENS_FACING_BACK -> Facing.BACK
            CameraCharacteristics.LENS_FACING_EXTERNAL -> Facing.EXTERNAL
            else -> return null
        }
        val sizes = details.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(android.media.MediaCodec::class.java).orEmpty().toSet()
        val fpsRanges = details.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES).orEmpty()
        val supports30 = fpsRanges.any { it.lower <= 30 && it.upper >= 30 }
        val presets = if (supports30) listOf(CertifiedPresets.FULL_HD_1080P30, CertifiedPresets.HD_720P30)
            .filter { preset -> sizes.any { it.width == preset.width && it.height == preset.height } } else emptyList()
        val zoom = details.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) ?: Range(1f, 1f)
        return CameraSummary(
            id,
            facing,
            zoom.lower,
            zoom.upper,
            details.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true,
            details.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0,
            presets,
        )
    }

    @Synchronized
    override fun close() {
        runCatching { session?.stopRepeating() }
        session?.close()
        session = null
        device?.close()
        device = null
        builder = null
        characteristics = null
        thread?.quitSafely()
        thread = null
        handler = null
    }
}
