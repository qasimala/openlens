# Dependency and license inventory

This development inventory is not a release SBOM or legal opinion.

| Component | Use | Link/distribution model |
|---|---|---|
| Android SDK / Camera2 / MediaCodec / NSD / Android Keystore | Platform camera, discovery, identity and TLS APIs | Supplied by Android device/toolchain |
| Jetpack Compose, Activity, Core | Android UI/runtime | Gradle dependencies |
| Kotlin / Gradle / Android Gradle Plugin | Build toolchain | Build-time |
| FFmpeg libavcodec/libavutil/libswscale | H.264 decode and I420 conversion | Dynamically linked system libraries |
| OpenSSL 3 | Desktop TLS 1.3, identity and SHA-256 | Dynamically linked system library |
| Avahi client | Local DNS-SD/mDNS discovery | Dynamically linked system library |
| libobs | Native OBS source | Dynamically linked system library |
| v4l2loopback | Linux virtual camera | External kernel module; not bundled |
| JUnit | Android unit tests | Test-only |

Before distributing a binary, generate the resolved Gradle/native dependency
graph, record FFmpeg configuration, verify notices, create SPDX/CycloneDX
SBOMs, and establish release signing and publishing ownership.
