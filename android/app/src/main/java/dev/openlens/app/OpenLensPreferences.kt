// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.content.Context

enum class CameraPreference { DESKTOP, REAR, FRONT }
enum class QualityPreference { DESKTOP, FULL_HD, HD }
enum class BitratePreference { DESKTOP, DATA_SAVER, BALANCED, QUALITY }

data class OpenLensSettings(
    val camera: CameraPreference = CameraPreference.DESKTOP,
    val quality: QualityPreference = QualityPreference.DESKTOP,
    val bitrate: BitratePreference = BitratePreference.DESKTOP,
    val mirrorFrontPreview: Boolean = true,
    val showGrid: Boolean = false,
    val showStats: Boolean = true,
    val keepScreenAwake: Boolean = true,
)

object OpenLensPreferences {
    private const val STORE = "openlens_settings"

    fun load(context: Context): OpenLensSettings {
        val values = context.getSharedPreferences(STORE, Context.MODE_PRIVATE)
        return OpenLensSettings(
            camera = values.enumValue("camera", CameraPreference.DESKTOP),
            quality = values.enumValue("quality", QualityPreference.DESKTOP),
            bitrate = values.enumValue("bitrate", BitratePreference.DESKTOP),
            mirrorFrontPreview = values.getBoolean("mirror_front_preview", true),
            showGrid = values.getBoolean("show_grid", false),
            showStats = values.getBoolean("show_stats", true),
            keepScreenAwake = values.getBoolean("keep_screen_awake", true),
        )
    }

    fun save(context: Context, settings: OpenLensSettings) {
        context.getSharedPreferences(STORE, Context.MODE_PRIVATE).edit()
            .putString("camera", settings.camera.name)
            .putString("quality", settings.quality.name)
            .putString("bitrate", settings.bitrate.name)
            .putBoolean("mirror_front_preview", settings.mirrorFrontPreview)
            .putBoolean("show_grid", settings.showGrid)
            .putBoolean("show_stats", settings.showStats)
            .putBoolean("keep_screen_awake", settings.keepScreenAwake)
            .apply()
    }

    private inline fun <reified T : Enum<T>> android.content.SharedPreferences.enumValue(
        key: String,
        fallback: T,
    ): T = runCatching { enumValueOf<T>(getString(key, fallback.name).orEmpty()) }.getOrDefault(fallback)
}
