// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.delay
import java.util.Locale

private val Background = Color(0xFF07130F)
private val Panel = Color(0xFF10231C)
private val PanelStrong = Color(0xFF17352A)
private val Accent = Color(0xFF62E6A6)
private val AccentDark = Color(0xFF163D2E)
private val PrimaryText = Color(0xFFF0FFF7)
private val SecondaryText = Color(0xFFA9C9B9)
private val Danger = Color(0xFFFF887C)

class MainActivity : ComponentActivity() {
    private var permissionGranted by mutableStateOf(false)
    private var wifiHost: WifiHostController? = null
    private var usbHost: UsbAccessoryController? = null
    private val requestCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        permissionGranted = granted
        refreshStatus()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissionGranted = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        val host = WifiHostController(this).also(WifiHostController::start)
        wifiHost = host
        usbHost = UsbAccessoryController(this, host).also(UsbAccessoryController::start)
        handleAccessoryIntent(intent)
        refreshStatus()
        setContent {
            var snapshot by remember { mutableStateOf(currentSnapshot()) }
            var wifi by remember { mutableStateOf(WifiHostStatus.snapshot) }
            var pairing by remember { mutableStateOf(WifiPairingStatus.snapshot) }
            var settings by remember { mutableStateOf(OpenLensPreferences.load(this)) }
            LaunchedEffect(Unit) {
                while (true) {
                    snapshot = currentSnapshot()
                    wifi = WifiHostStatus.snapshot
                    pairing = WifiPairingStatus.snapshot
                    delay(250)
                }
            }
            LaunchedEffect(settings.keepScreenAwake) {
                if (settings.keepScreenAwake) {
                    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                } else {
                    window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
            }
            OpenLensScreen(
                snapshot = snapshot,
                wifi = wifi,
                pairing = pairing,
                settings = settings,
                onSettings = { updated ->
                    settings = updated
                    OpenLensPreferences.save(this, updated)
                },
                onAllow = { requestCamera.launch(Manifest.permission.CAMERA) },
                onPairDecision = WifiPairingStatus::decide,
                onRefreshPairing = { wifiHost?.refreshPairingWindow() },
                onForgetComputer = { wifiHost?.forgetComputer() },
                onStop = ::stopSession,
                onControls = CameraStreamingService::updateControls,
                onFacing = CameraStreamingService::setFacing,
                onFocus = CameraStreamingService::tapFocus,
            )
        }
    }

    override fun onNewIntent(intent: android.content.Intent) {
        super.onNewIntent(intent)
        handleAccessoryIntent(intent)
    }

    private fun handleAccessoryIntent(intent: android.content.Intent?) {
        if (intent?.action != android.hardware.usb.UsbManager.ACTION_USB_ACCESSORY_ATTACHED) return
        val accessory = androidx.core.content.IntentCompat.getParcelableExtra(
            intent,
            android.hardware.usb.UsbManager.EXTRA_ACCESSORY,
            android.hardware.usb.UsbAccessory::class.java,
        )
        accessory?.let { usbHost?.attach(it) }
    }

    override fun onDestroy() {
        usbHost?.close()
        usbHost = null
        wifiHost?.close()
        wifiHost = null
        super.onDestroy()
    }

    private fun refreshStatus() {
        SessionStatus.snapshot = when {
            !permissionGranted -> SessionSnapshot(
                SessionState.PERMISSION_NEEDED,
                "Allow camera access once, then OpenLens can be controlled from your computer.",
            )
            else -> SessionSnapshot(SessionState.READY, WifiHostStatus.snapshot.detail)
        }
    }

    private fun currentSnapshot(): SessionSnapshot = when {
        !permissionGranted -> SessionSnapshot(
            SessionState.PERMISSION_NEEDED,
            "Allow camera access once, then OpenLens can be controlled from your computer.",
        )
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
    settings: OpenLensSettings,
    onSettings: (OpenLensSettings) -> Unit,
    onAllow: () -> Unit,
    onPairDecision: (Boolean) -> Unit,
    onRefreshPairing: () -> Unit,
    onForgetComputer: () -> Unit,
    onStop: () -> Unit,
    onControls: (Float, Int, Boolean) -> Unit,
    onFacing: (Boolean) -> Unit,
    onFocus: (Float, Float) -> Unit,
) {
    var selectedTab by remember { mutableIntStateOf(0) }
    val streaming = snapshot.state in setOf(SessionState.STARTING, SessionState.STREAMING, SessionState.RECOVERING)
    MaterialTheme {
        Surface(modifier = Modifier.fillMaxSize(), color = Background) {
            BoxWithConstraints(Modifier.fillMaxSize()) {
                if (maxWidth > maxHeight) {
                    Row(
                        modifier = Modifier.fillMaxSize().padding(horizontal = 18.dp, vertical = 14.dp),
                        horizontalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        Column(
                            modifier = Modifier.weight(1.35f),
                            verticalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            BrandHeader(snapshot, wifi)
                            PreviewCard(snapshot, streaming, settings, onFocus)
                        }
                        Column(
                            modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()),
                            verticalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            if (pairing.active) PairingCard(pairing, onPairDecision)
                            TabSelector(selectedTab, onSelected = { selectedTab = it })
                            TabContent(
                                selectedTab,
                                snapshot,
                                wifi,
                                streaming,
                                settings,
                                onSettings,
                                onAllow,
                                onRefreshPairing,
                                onForgetComputer,
                                onStop,
                                onControls,
                                onFacing,
                            )
                            PrivacyFooter()
                        }
                    }
                } else {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .verticalScroll(rememberScrollState())
                            .padding(horizontal = 18.dp, vertical = 16.dp),
                        verticalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        BrandHeader(snapshot, wifi)
                        if (pairing.active) PairingCard(pairing, onPairDecision)
                        PreviewCard(snapshot, streaming, settings, onFocus)
                        TabSelector(selectedTab, onSelected = { selectedTab = it })
                        TabContent(
                            selectedTab,
                            snapshot,
                            wifi,
                            streaming,
                            settings,
                            onSettings,
                            onAllow,
                            onRefreshPairing,
                            onForgetComputer,
                            onStop,
                            onControls,
                            onFacing,
                        )
                        PrivacyFooter()
                    }
                }
            }
        }
    }
}

@Composable
private fun TabContent(
    selectedTab: Int,
    snapshot: SessionSnapshot,
    wifi: WifiHostSnapshot,
    streaming: Boolean,
    settings: OpenLensSettings,
    onSettings: (OpenLensSettings) -> Unit,
    onAllow: () -> Unit,
    onRefreshPairing: () -> Unit,
    onForgetComputer: () -> Unit,
    onStop: () -> Unit,
    onControls: (Float, Int, Boolean) -> Unit,
    onFacing: (Boolean) -> Unit,
) {
    if (selectedTab == 0) {
        CameraPage(
            snapshot,
            wifi,
            streaming,
            settings.showStats,
            onAllow,
            onRefreshPairing,
            onStop,
            onControls,
            onFacing,
        )
    } else {
        SettingsPage(settings, wifi.paired, onSettings, onForgetComputer)
    }
}

@Composable
private fun PrivacyFooter() {
    Text(
        "Private by design · Local Wi‑Fi · No account or cloud",
        modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
        style = MaterialTheme.typography.labelMedium,
        color = SecondaryText,
    )
}

@Composable
private fun BrandHeader(snapshot: SessionSnapshot, wifi: WifiHostSnapshot) {
    val (label, color) = when {
        snapshot.state == SessionState.STREAMING -> "LIVE" to Accent
        snapshot.state == SessionState.ERROR -> "NEEDS ATTENTION" to Danger
        wifi.paired -> "READY" to Accent
        else -> "PAIRING" to Color(0xFFFFCF70)
    }
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f)) {
            Text("OpenLens", color = PrimaryText, fontSize = 30.sp, fontWeight = FontWeight.Bold)
            Text("Your phone, now a camera", color = SecondaryText, style = MaterialTheme.typography.bodyMedium)
        }
        Surface(color = color.copy(alpha = 0.13f), shape = RoundedCornerShape(50)) {
            Row(
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 7.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(7.dp),
            ) {
                Box(Modifier.size(7.dp).background(color, CircleShape))
                Text(label, color = color, style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.Bold)
            }
        }
    }
}

@Composable
private fun PairingCard(pairing: WifiPairingSnapshot, onDecision: (Boolean) -> Unit) {
    Surface(color = AccentDark, shape = RoundedCornerShape(22.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("Confirm this computer", color = Accent, fontWeight = FontWeight.Bold)
            Text(pairing.code, color = PrimaryText, fontSize = 44.sp, fontWeight = FontWeight.Bold, letterSpacing = 5.sp)
            Text(pairing.detail, color = SecondaryText)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(onClick = { onDecision(true) }, modifier = Modifier.weight(1f)) { Text("Codes match") }
                OutlinedButton(onClick = { onDecision(false) }, modifier = Modifier.weight(1f)) { Text("Cancel") }
            }
        }
    }
}

@Composable
private fun PreviewCard(
    snapshot: SessionSnapshot,
    streaming: Boolean,
    settings: OpenLensSettings,
    onFocus: (Float, Float) -> Unit,
) {
    // The camera renders upright for the current orientation, so the preview
    // box must follow it: 16:9 in landscape, 9:16 (narrowed, centred) in
    // portrait — otherwise portrait video is stretched into a landscape box.
    val portrait = LocalConfiguration.current.orientation == Configuration.ORIENTATION_PORTRAIT
    val shapeModifier = if (portrait) {
        Modifier
            .fillMaxWidth(0.62f)
            .aspectRatio(9f / 16f)
    } else {
        Modifier
            .fillMaxWidth()
            .aspectRatio(16f / 9f)
    }
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
    Box(
        modifier = shapeModifier
            .clip(RoundedCornerShape(22.dp))
            .background(Color.Black),
    ) {
        CameraPreview(
            modifier = Modifier.fillMaxSize(),
            mirror = settings.mirrorFrontPreview && snapshot.facing == "front",
            onFocus = onFocus,
        )
        if (settings.showGrid) RuleOfThirdsGrid(Modifier.fillMaxSize())
        Surface(
            modifier = Modifier.align(Alignment.TopStart).padding(12.dp),
            color = Color.Black.copy(alpha = 0.62f),
            shape = RoundedCornerShape(50),
        ) {
            Row(
                Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Box(Modifier.size(7.dp).background(if (streaming) Danger else SecondaryText, CircleShape))
                Text(if (streaming) "LIVE" else "PREVIEW", color = Color.White, style = MaterialTheme.typography.labelSmall)
            }
        }
        if (snapshot.width > 0) {
            Text(
                "${snapshot.height}p · ${snapshot.fps} fps",
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(12.dp)
                    .background(Color.Black.copy(alpha = 0.62f), RoundedCornerShape(50))
                    .padding(horizontal = 10.dp, vertical = 6.dp),
                color = Color.White,
                style = MaterialTheme.typography.labelSmall,
            )
        }
        if (!streaming) {
            Text(
                "Preview appears when the desktop starts the camera",
                modifier = Modifier.align(Alignment.Center).padding(24.dp),
                color = Color.White.copy(alpha = 0.68f),
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
    }
}

@Composable
private fun TabSelector(selected: Int, onSelected: (Int) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().background(Panel, RoundedCornerShape(14.dp)).padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        listOf("Camera", "Settings").forEachIndexed { index, label ->
            Button(
                onClick = { onSelected(index) },
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (selected == index) PanelStrong else Color.Transparent,
                    contentColor = if (selected == index) Accent else SecondaryText,
                ),
                shape = RoundedCornerShape(10.dp),
            ) { Text(label, fontWeight = FontWeight.SemiBold) }
        }
    }
}

@Composable
private fun CameraPage(
    snapshot: SessionSnapshot,
    wifi: WifiHostSnapshot,
    streaming: Boolean,
    showStats: Boolean,
    onAllow: () -> Unit,
    onRefreshPairing: () -> Unit,
    onStop: () -> Unit,
    onControls: (Float, Int, Boolean) -> Unit,
    onFacing: (Boolean) -> Unit,
) {
    if (snapshot.state == SessionState.PERMISSION_NEEDED) {
        InfoCard("One permission to get started", snapshot.detail) {
            Button(onClick = onAllow, modifier = Modifier.fillMaxWidth()) { Text("Allow camera access") }
        }
        return
    }
    if (!streaming) {
        InfoCard(
            if (wifi.paired) "Ready for your computer" else "Pair your computer",
            snapshot.detail,
        ) {
            if (!wifi.paired) {
                Button(onClick = onRefreshPairing, modifier = Modifier.fillMaxWidth()) {
                    Text("Allow pairing for 2 minutes")
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Box(Modifier.size(8.dp).background(Accent, CircleShape))
                Text("OpenLens can start automatically from the desktop app", color = SecondaryText)
            }
        }
        return
    }
    if (snapshot.state == SessionState.STREAMING) {
        ControlPanel(snapshot, onControls, onFacing)
        if (showStats) Stats(snapshot)
    } else {
        InfoCard("Preparing camera", snapshot.detail)
    }
    Button(
        onClick = onStop,
        modifier = Modifier.fillMaxWidth(),
        colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF542A28), contentColor = Color(0xFFFFDAD5)),
    ) { Text("Stop camera") }
}

@Composable
private fun ControlPanel(
    snapshot: SessionSnapshot,
    onControls: (Float, Int, Boolean) -> Unit,
    onFacing: (Boolean) -> Unit,
) {
    PanelCard {
        SectionTitle("Camera controls", "Tap the preview to focus")
        Text("Lens", color = SecondaryText, style = MaterialTheme.typography.labelMedium)
        ChoiceButtons(
            choices = listOf("Rear", "Front"),
            selected = if (snapshot.facing == "front") 1 else 0,
            onSelected = { onFacing(it == 1) },
        )
        HorizontalDivider(color = Color.White.copy(alpha = 0.08f))
        ValueHeader("Zoom", String.format(Locale.US, "%.1f×", snapshot.zoom))
        Slider(
            value = snapshot.zoom.coerceIn(1f, 10f),
            onValueChange = { onControls(it, snapshot.exposure, snapshot.torch) },
            valueRange = 1f..10f,
        )
        ValueHeader("Exposure", if (snapshot.exposure == 0) "Auto · 0" else "%+d steps".format(snapshot.exposure))
        Slider(
            value = snapshot.exposure.coerceIn(-12, 12).toFloat(),
            onValueChange = { onControls(snapshot.zoom, it.toInt(), snapshot.torch) },
            valueRange = -12f..12f,
            steps = 23,
        )
        SettingToggle(
            title = "Torch",
            subtitle = "Use the rear camera light",
            checked = snapshot.torch,
            onChecked = { onControls(snapshot.zoom, snapshot.exposure, it) },
        )
    }
}

@Composable
private fun SettingsPage(
    settings: OpenLensSettings,
    paired: Boolean,
    onSettings: (OpenLensSettings) -> Unit,
    onForgetComputer: () -> Unit,
) {
    PanelCard {
        SectionTitle("Stream defaults", "The desktop remains in control unless you choose an override")
        PreferenceChoice(
            "Preferred camera",
            listOf("Desktop", "Rear", "Front"),
            settings.camera.ordinal,
        ) { onSettings(settings.copy(camera = CameraPreference.entries[it])) }
        PreferenceChoice(
            "Resolution",
            listOf("Desktop", "1080p", "720p"),
            settings.quality.ordinal,
        ) { onSettings(settings.copy(quality = QualityPreference.entries[it])) }
        PreferenceChoice(
            "Video quality",
            listOf("Desktop", "Saver", "Balanced", "Quality"),
            settings.bitrate.ordinal,
        ) { onSettings(settings.copy(bitrate = BitratePreference.entries[it])) }
        Text(
            "Changes apply the next time the camera starts.",
            color = SecondaryText,
            style = MaterialTheme.typography.labelSmall,
        )
    }
    PanelCard {
        SectionTitle("Preview & display", "These options only affect the phone screen")
        SettingToggle(
            "Mirror front preview",
            "Makes the phone preview feel like a mirror; OBS stays unchanged",
            settings.mirrorFrontPreview,
        ) { onSettings(settings.copy(mirrorFrontPreview = it)) }
        SettingToggle("Composition grid", "Show a rule-of-thirds guide", settings.showGrid) {
            onSettings(settings.copy(showGrid = it))
        }
        SettingToggle("Performance stats", "Show encoder and frame information", settings.showStats) {
            onSettings(settings.copy(showStats = it))
        }
        SettingToggle("Keep screen awake", "Prevent the display from sleeping while OpenLens is open", settings.keepScreenAwake) {
            onSettings(settings.copy(keepScreenAwake = it))
        }
    }
    PanelCard {
        SectionTitle("Paired computer", if (paired) "A computer is trusted on this local network" else "No computer paired yet")
        if (paired) {
            OutlinedButton(onClick = onForgetComputer, modifier = Modifier.fillMaxWidth()) {
                Text("Forget paired computer", color = Danger)
            }
        } else {
            Text("Pairing uses a one-time six-digit code shown on both screens.", color = SecondaryText)
        }
    }
}

@Composable
private fun PreferenceChoice(title: String, choices: List<String>, selected: Int, onSelected: (Int) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, color = PrimaryText, fontWeight = FontWeight.SemiBold)
        ChoiceButtons(choices, selected, onSelected)
    }
}

@Composable
private fun ChoiceButtons(choices: List<String>, selected: Int, onSelected: (Int) -> Unit) {
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        choices.forEachIndexed { index, label ->
            OutlinedButton(
                onClick = { onSelected(index) },
                modifier = Modifier.weight(1f),
                colors = ButtonDefaults.outlinedButtonColors(
                    containerColor = if (selected == index) AccentDark else Color.Transparent,
                    contentColor = if (selected == index) Accent else SecondaryText,
                ),
                border = BorderStroke(
                    1.dp,
                    if (selected == index) Accent.copy(alpha = 0.55f) else Color.White.copy(alpha = 0.12f),
                ),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 4.dp, vertical = 10.dp),
            ) { Text(label, maxLines = 1, fontSize = 12.sp) }
        }
    }
}

@Composable
private fun SettingToggle(title: String, subtitle: String, checked: Boolean, onChecked: (Boolean) -> Unit) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, color = PrimaryText, fontWeight = FontWeight.SemiBold)
            Text(subtitle, color = SecondaryText, style = MaterialTheme.typography.bodySmall)
        }
        Switch(checked = checked, onCheckedChange = onChecked)
    }
}

@Composable
private fun InfoCard(title: String, detail: String, content: @Composable (() -> Unit)? = null) {
    PanelCard {
        SectionTitle(title, detail)
        content?.invoke()
    }
}

@Composable
private fun PanelCard(content: @Composable ColumnScope.() -> Unit) {
    Surface(color = Panel, shape = RoundedCornerShape(20.dp), modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
            content = content,
        )
    }
}

@Composable
private fun SectionTitle(title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(title, color = PrimaryText, fontSize = 18.sp, fontWeight = FontWeight.Bold)
        Text(subtitle, color = SecondaryText, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun ValueHeader(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth()) {
        Text(label, color = PrimaryText, modifier = Modifier.weight(1f), fontWeight = FontWeight.SemiBold)
        Text(value, color = Accent)
    }
}

@Composable
private fun CameraPreview(modifier: Modifier, mirror: Boolean, onFocus: (Float, Float) -> Unit) {
    AndroidView(
        modifier = modifier.pointerInput(onFocus) {
            detectTapGestures { offset ->
                if (size.width > 0 && size.height > 0) onFocus(offset.x / size.width, offset.y / size.height)
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
        update = { view -> view.scaleX = if (mirror) -1f else 1f },
    )
}

@Composable
private fun RuleOfThirdsGrid(modifier: Modifier) {
    Canvas(modifier) {
        val lineColor = Color.White.copy(alpha = 0.34f)
        listOf(size.width / 3f, size.width * 2f / 3f).forEach { x ->
            drawLine(lineColor, Offset(x, 0f), Offset(x, size.height), 1.dp.toPx(), StrokeCap.Round)
        }
        listOf(size.height / 3f, size.height * 2f / 3f).forEach { y ->
            drawLine(lineColor, Offset(0f, y), Offset(size.width, y), 1.dp.toPx(), StrokeCap.Round)
        }
    }
}

@Composable
private fun Stats(snapshot: SessionSnapshot) {
    PanelCard {
        SectionTitle("Performance", "Live encoder and transport health")
        Row(Modifier.fillMaxWidth()) {
            Metric("FORMAT", "${snapshot.width}×${snapshot.height}", Modifier.weight(1f))
            Metric("RATE", "${snapshot.fps} fps", Modifier.weight(1f))
            Metric("BITRATE", "${snapshot.bitrate / 1_000_000} Mbps", Modifier.weight(1f))
        }
        HorizontalDivider(color = Color.White.copy(alpha = 0.08f))
        Text("${snapshot.encoder} · ${snapshot.frames} frames · ${snapshot.dropped} dropped", color = SecondaryText)
    }
}

@Composable
private fun Metric(label: String, value: String, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(label, color = SecondaryText, style = MaterialTheme.typography.labelSmall)
        Text(value, color = PrimaryText, fontWeight = FontWeight.Bold)
    }
}
