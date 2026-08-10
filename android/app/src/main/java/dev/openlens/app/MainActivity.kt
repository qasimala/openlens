// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    private var permissionGranted by mutableStateOf(false)
    private var wifiHost: WifiHostController? = null
    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        permissionGranted = granted
        refreshStatus()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissionGranted = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        wifiHost = WifiHostController(this).also(WifiHostController::start)
        refreshStatus()
        setContent {
            var snapshot by remember { mutableStateOf(currentSnapshot()) }
            LaunchedEffect(Unit) {
                while (true) {
                    snapshot = currentSnapshot()
                    delay(250)
                }
            }
            OpenLensScreen(
                snapshot = snapshot,
                wifi = WifiHostStatus.snapshot,
                pairing = WifiPairingStatus.snapshot,
                onAllow = { requestCamera.launch(Manifest.permission.CAMERA) },
                onPairDecision = WifiPairingStatus::decide,
                onForgetComputer = { wifiHost?.forgetComputer() },
                onStop = ::stopSession,
                onControls = CameraStreamingService::updateControls,
                onFacing = CameraStreamingService::setFacing,
                onFocus = CameraStreamingService::tapFocus,
            )
        }
    }

    override fun onDestroy() {
        wifiHost?.close()
        wifiHost = null
        super.onDestroy()
    }

    private fun refreshStatus() {
        SessionStatus.snapshot = when {
            !permissionGranted -> SessionSnapshot(SessionState.PERMISSION_NEEDED, "Camera permission is needed before your phone can become a camera.")
            else -> SessionSnapshot(SessionState.READY, WifiHostStatus.snapshot.detail)
        }
    }

    private fun currentSnapshot(): SessionSnapshot = when {
        !permissionGranted -> SessionSnapshot(SessionState.PERMISSION_NEEDED, "Camera permission is needed before your phone can become a camera.")
        SessionStatus.snapshot.state in setOf(SessionState.STOPPED, SessionState.READY) ->
            SessionSnapshot(SessionState.READY, WifiHostStatus.snapshot.detail)
        else -> SessionStatus.snapshot
    }

    private fun stopSession() {
        startService(Intent(this, CameraStreamingService::class.java).setAction(CameraStreamingService.ACTION_STOP))
    }
}

@Composable
private fun OpenLensScreen(
    snapshot: SessionSnapshot,
    wifi: WifiHostSnapshot,
    pairing: WifiPairingSnapshot,
    onAllow: () -> Unit,
    onPairDecision: (Boolean) -> Unit,
    onForgetComputer: () -> Unit,
    onStop: () -> Unit,
    onControls: (Float, Int, Boolean) -> Unit,
    onFacing: (Boolean) -> Unit,
    onFocus: (Float, Float) -> Unit,
) {
    val streaming = snapshot.state in setOf(SessionState.STARTING, SessionState.STREAMING, SessionState.RECOVERING)
    MaterialTheme {
        Surface(modifier = Modifier.fillMaxSize(), color = Color(0xFF071A14)) {
            Column(
                modifier = Modifier
                    .padding(20.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                Text("OpenLens", style = MaterialTheme.typography.headlineLarge, color = Color(0xFFF2FFF9))
                Text(snapshot.detail, color = Color(0xFFD2EEE2))
                if (pairing.active) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .background(Color(0xFF0E2A21), RoundedCornerShape(16.dp))
                            .padding(18.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        Text("Pairing code", color = Color(0xFFA9D9C2))
                        Text(pairing.code, style = MaterialTheme.typography.displayMedium, color = Color.White)
                        Text(pairing.detail, color = Color(0xFFD2EEE2))
                        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                            Button(onClick = { onPairDecision(true) }, modifier = Modifier.weight(1f)) {
                                Text("Codes match")
                            }
                            OutlinedButton(onClick = { onPairDecision(false) }, modifier = Modifier.weight(1f)) {
                                Text("Cancel")
                            }
                        }
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    when {
                        snapshot.state == SessionState.PERMISSION_NEEDED ->
                            Button(onClick = onAllow, modifier = Modifier.fillMaxWidth()) { Text("Allow camera") }
                        streaming ->
                            Button(onClick = onStop, modifier = Modifier.fillMaxWidth()) { Text("Stop camera") }
                    }
                }
                CameraPreview(
                    Modifier
                        .fillMaxWidth()
                        .aspectRatio(16f / 9f)
                        .background(Color.Black, RoundedCornerShape(16.dp)),
                    onFocus,
                )
                if (streaming) Text("Tap the preview to focus", color = Color(0xFF76A990))
                if (!streaming && wifi.paired && !pairing.active) {
                    OutlinedButton(onClick = onForgetComputer, modifier = Modifier.fillMaxWidth()) {
                        Text("Forget paired computer")
                    }
                }
                if (snapshot.state == SessionState.STREAMING) {
                    Stats(snapshot)
                    Text("Camera controls", color = Color(0xFFA9D9C2))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (snapshot.facing == "back") Button(onClick = { onFacing(false) }) { Text("Rear") }
                        else OutlinedButton(onClick = { onFacing(false) }) { Text("Rear") }
                        if (snapshot.facing == "front") Button(onClick = { onFacing(true) }) { Text("Front") }
                        else OutlinedButton(onClick = { onFacing(true) }) { Text("Front") }
                        OutlinedButton(
                            onClick = { onControls(snapshot.zoom, snapshot.exposure, !snapshot.torch) },
                        ) { Text(if (snapshot.torch) "Torch on" else "Torch off") }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(
                            onClick = { onControls((snapshot.zoom - 0.5f).coerceAtLeast(1f), snapshot.exposure, snapshot.torch) },
                        ) { Text("Zoom −") }
                        Text("${"%.1f".format(snapshot.zoom)}×", color = Color.White, modifier = Modifier.padding(vertical = 12.dp))
                        OutlinedButton(
                            onClick = { onControls(snapshot.zoom + 0.5f, snapshot.exposure, snapshot.torch) },
                        ) { Text("Zoom +") }
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(
                            onClick = { onControls(snapshot.zoom, snapshot.exposure - 1, snapshot.torch) },
                        ) { Text("Exposure −") }
                        Text("EV step ${snapshot.exposure}", color = Color.White, modifier = Modifier.padding(vertical = 12.dp))
                        OutlinedButton(
                            onClick = { onControls(snapshot.zoom, snapshot.exposure + 1, snapshot.torch) },
                        ) { Text("Exposure +") }
                    }
                }
                Text(
                    "No account · No cloud · Local Wi-Fi",
                    style = MaterialTheme.typography.labelMedium,
                    color = Color(0xFF76A990),
                )
            }
        }
    }
}

@Composable
private fun CameraPreview(modifier: Modifier, onFocus: (Float, Float) -> Unit) {
    AndroidView(
        modifier = modifier.pointerInput(onFocus) {
            detectTapGestures { offset ->
                if (size.width > 0 && size.height > 0) {
                    onFocus(offset.x / size.width, offset.y / size.height)
                }
            }
        },
        factory = { context ->
            TextureView(context).apply {
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                        PreviewSurfaceRegistry.surface = Surface(texture)
                    }

                    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) = Unit

                    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                        PreviewSurfaceRegistry.surface?.release()
                        PreviewSurfaceRegistry.surface = null
                        return true
                    }

                    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
                }
            }
        },
    )
}

@Composable
private fun Stats(snapshot: SessionSnapshot) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFF0E2A21), RoundedCornerShape(12.dp))
            .padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Row(Modifier.fillMaxWidth()) {
            Text("${snapshot.width}×${snapshot.height} · ${snapshot.fps} fps", Modifier.weight(1f), color = Color.White)
            Text("${snapshot.bitrate / 1_000_000} Mbps", color = Color.White)
        }
        Text("Encoder: ${snapshot.encoder}", color = Color(0xFFA9D9C2))
        Text("Frames: ${snapshot.frames} · Dropped before USB: ${snapshot.dropped}", color = Color(0xFFA9D9C2))
    }
}
