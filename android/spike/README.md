# OpenLens Android Phase 0 spike

This disposable Android app inventories Camera2 and AVC encoder capabilities on a real phone, then runs a 30-second `1920x1080@30` Camera2-to-MediaCodec probe. It writes:

- `phase0-capabilities.json`
- `phase0-probe.json`
- `phase0-1080p30.mp4`

The probe uses two Camera2 outputs: an on-device preview `Surface` and the hardware encoder input `Surface`. It records output rate, frames, keyframes, effective bitrate, clean end-of-stream handling, and the distribution from each camera surface timestamp to the corresponding encoded output being dequeued.

## Build

Requirements: JDK 17 or 21 and Android SDK 36. Set `sdk.dir` in the ignored `local.properties` file, then run:

```bash
cd android/spike
JAVA_HOME=/path/to/jdk-21 ./gradlew :app:assembleDebug
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## Run

Install the APK, grant camera permission, and open it. The visible buttons scan capabilities and run/stop the probe. For lab automation, a cold activity start accepts `--ez auto_probe true`; this debug-only mode turns the screen on, permits display over the keyguard, and keeps the screen awake for the timed test.

The result files live in the app's private `files` directory. A debuggable build can inspect them with `adb shell run-as dev.openlens.spike`.

## Scope

This is evidence-gathering code, not the production app. It intentionally omits transport, audio, service lifecycle, reconnection, polished controls, and compatibility abstractions.
