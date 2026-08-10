// SPDX-License-Identifier: GPL-2.0-or-later
package dev.openlens.app

import android.view.Surface

enum class SessionState {
    PERMISSION_NEEDED,
    READY,
    DESKTOP_REQUEST,
    WAITING_FOR_START,
    STARTING,
    STREAMING,
    RECOVERING,
    ERROR,
    STOPPED,
}

data class SessionSnapshot(
    val state: SessionState = SessionState.STOPPED,
    val detail: String = "Camera stopped.",
    val frames: Long = 0,
    val bytes: Long = 0,
    val dropped: Long = 0,
    val encoder: String = "—",
    val width: Int = 0,
    val height: Int = 0,
    val fps: Int = 0,
    val bitrate: Int = 0,
    val facing: String = "back",
    val zoom: Float = 1f,
    val exposure: Int = 0,
    val torch: Boolean = false,
)

object SessionStatus {
    @Volatile
    var snapshot = SessionSnapshot()
}

object PreviewSurfaceRegistry {
    @Volatile
    var surface: Surface? = null
}
