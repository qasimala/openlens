# OpenLens

OpenLens turns an Android phone into a private, low-latency camera source for
OBS over your local Wi-Fi. It has no account, cloud service, telemetry,
watermark, or internet relay.

The current reference system is a Samsung Galaxy S24 Ultra, CachyOS Linux, and
OBS 32. Android captures with Camera2 and hardware H.264 `MediaCodec`; Linux
discovers the phone with DNS-SD/Avahi, authenticates it with pinned mutual TLS,
decodes with FFmpeg, and sends frames to V4L2 or the native OBS source.

## Normal use

1. Install and open OpenLens on the phone and PC while both are on the same
   local network.
2. Allow the one-time Android camera permission.
3. Select the phone in OpenLens Desktop and click **Pair phone**.
4. Check that the six-digit codes match, then confirm on both devices.
5. Click **Start camera** on the PC. Later sessions need no code or phone tap.
6. In OBS, select the OpenLens V4L2 device or add **OpenLens Phone Camera**.

The PC controls start, stop, camera facing, quality, bitrate, zoom, exposure,
torch, rotation, and mirroring. Android always retains its privacy indicator,
foreground notification, and emergency Stop action.

## Status

This is a development alpha. Wi-Fi discovery, commitment-based one-time
pairing, remembered SPKI identities, TLS 1.3 media transport, Android capture,
the graphical desktop app, CLI, V4L2 sink, and native OBS plugin are
implemented. The debug APK is not a signed release. Physical Wi-Fi roaming,
latency, long soak, cross-device, and release-signing gates still require
recorded device evidence.

## Build

Requirements: JDK 21, Android SDK 36, Gradle 8.13, C++20, CMake, Ninja, FFmpeg
development libraries, OpenSSL 3, Avahi client development libraries, Qt 6.5+
Widgets, and optionally libobs development headers. ADB is not a runtime or
desktop build dependency.

```sh
cd android
./gradlew :app:assembleDebug :app:lintDebug :camera:testDebugUnitTest :protocol:testDebugUnitTest
cd ..
./scripts/build-desktop.sh
```

The APK is created at `android/app/build/outputs/apk/debug/app-debug.apk`. Copy
it to the phone and install it with Android's normal package installer. A USB
debugging tool may be used by developers to install a debug build, but OpenLens
does not use it after installation.

Launch `build/desktop-debug/desktop/gui/openlens-desktop`, or run
`./scripts/install-desktop-app.sh` and open **OpenLens** from the application
menu. See the [desktop guide](docs/desktop.md) and
[Linux setup](docs/setup/linux.md).

## Optional diagnostic commands

```text
openlens doctor [--json]
openlens devices [--json]
openlens pair [--id DEVICE_ID]
openlens forget --id DEVICE_ID
openlens start [--id DEVICE_ID] [--preset 1080p30|720p30] [--video DEVICE]
               [--facing back|front] [--bitrate BPS] [--zoom RATIO]
               [--exposure STEPS] [--torch]
openlens receive --sink null [--seconds N] [--output explicit.h264]
openlens synthetic [--video DEVICE] [--seconds N]
```

Most people do not need these commands. The desktop interface handles
discovery, pairing, camera settings, and OBS setup.

The normative media framing is [protocol/PROTOCOL.md](protocol/PROTOCOL.md).
The Wi-Fi security and migration design is
[plans/codex/wifi-migration-plan-v2.html](plans/codex/wifi-migration-plan-v2.html).

## Contributing and security

Contributions are welcome under GPL-2.0-or-later with a DCO sign-off. Read
[CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) first.
