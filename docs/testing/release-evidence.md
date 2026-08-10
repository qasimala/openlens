# Release evidence ledger

This ledger distinguishes implemented code from measured release gates. A gate
is **PASS** only when retained physical evidence supports it.

Last local verification: 2026-08-10 on the reference CachyOS host. The S24
Ultra was disconnected during final Wi-Fi installation testing.

| Gate | Current result | Evidence |
|---|---|---|
| Phase 0 custom Camera2/MediaCodec feasibility | PASS | `docs/phase0/benchmark.md`, S24 artifacts |
| Cross-language protocol v2 fixtures and pairing vector | PASS | Kotlin and C++ unit tests; shared SAS `141454` |
| C++ warning-clean build | PASS | local CMake/Ninja build |
| Android build, unit tests, lint | PASS | local Gradle build |
| ASan/UBSan protocol/core tests | PASS with local LSan disabled | LSan cannot run under the desktop sandbox ptrace policy; CI keeps leak detection enabled |
| Synthetic I420 → `/dev/video42` | PASS | Three-second real-device write; Phase 0 OBS recording proof |
| Native OBS module build/load/source registration | PASS | Links to installed libobs 32.2.1; libobs loader registered `openlens_phone_camera` |
| OpenLens Desktop GUI build/startup | PASS | Qt warning-clean build, off-screen startup test, real-system readiness render, desktop-entry validation |
| Desktop TLS identity create/save/load/forget | PASS | OpenSSL identity-store core test with private temporary storage |
| Android release build and lint-vital | PASS | Unsigned release APK built successfully |
| Desktop release build/tests | PASS | Release GUI, core and protocol tests pass; OBS smoke is skipped when loader isolation is unavailable |
| Wi-Fi-only artifact gate | PASS | Release APK, GUI, CLI and OBS module scanned by `scripts/verify-wifi-only-release.sh`; no retired transport marker |
| Debug artifact integrity/SBOM | PASS | APK/CLI/plugin SHA-256 manifest verifies; SPDX 2.3 document generated |
| Privacy manifest review | PASS | Only Camera, Internet and camera foreground-service permissions; no microphone/location/storage/account permission |
| Legacy S24 PC-controlled USB baseline | PASS | Pre-migration baseline: unlocked S24 delivered 126 decoded frames in five seconds |
| S24 NSD → Avahi discovery with new APK | NOT RUN | Phone disconnected before the Wi-Fi APK could be installed |
| S24 commit/reveal pairing and strict pinned reconnect | NOT RUN | Requires both-device code confirmation on the physical phone |
| S24 PC-controlled Wi-Fi start and OBS frames | NOT RUN | Requires the new APK on the reference phone |
| 1080p30 60-minute dropped-frame gate | NOT RUN | Requires unlocked, attended physical soak |
| Two-hour V4L2/OBS recording | NOT RUN | Requires attended release run and retained recording/stats |
| 50 start/stop cycles | NOT RUN | Requires an automated repetition run and retained per-cycle results |
| 20 Wi-Fi interruptions, reconnect ≤5 s | NOT RUN | Requires attended network test |
| External median/p95 latency | NOT RUN | Requires high-frame-rate external camera/timer capture |
| No internet destinations and encrypted LAN payload | NOT RUN | Requires host/device traffic capture |
| Native OBS two-hour/beta latency gates | NOT RUN | Requires plugin installation and attended OBS run |
| Release signature/public identity | OWNER DECISION REQUIRED | Public name, permanent app ID, signing identity, and publishing approval are intentionally unresolved |

No version tag or release claim may be created while a mandatory row is
`NOT RUN`, `BLOCKED`, or `OWNER DECISION REQUIRED`.
