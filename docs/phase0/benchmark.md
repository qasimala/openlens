# Phase 0 measured baseline

Date: 2026-08-08  
Host: CachyOS, kernel 7.1.5-1-cachyos, KDE Wayland  
OBS: 32.2.1  
FFmpeg: 8.1.2  
Phone: Samsung Galaxy S24 Ultra SM-S928B, Android 14/API 34, Snapdragon SM8650  
Connection: USB with ADB, serial recorded locally as R5CX30Z3G9J

## Result summary

The custom Camera2/MediaCodec direction is feasible on the target phone. The primary rear logical camera exposes Level 3 Camera2, four physical camera IDs, fixed 30/60 fps ranges, stabilization, manual sensor/post-processing, 10-bit dynamic range, RAW, and hardware AVC encoding. A 30-second dual-output preview/encode run completed cleanly.

The native Android USB webcam path is unavailable on this exact phone build. Its active, persistent, and applied USB functions are `mtp,conn_gadget,adb`; neither the USB manager state nor exposed functions contain UVC/webcam.

scrcpy 4.1 is a strong baseline and successfully captures the same camera at 1080p30. It already covers camera selection, size, fps, zoom, torch, rotation, recording, and V4L2 output. It does not expose the production control surface planned for OpenLens: manual focus/exposure/white balance, stabilization policy, consistent presets, telemetry, and guided OBS/recovery UX.

## Camera inventory

| ID | Facing | Camera2 level | Focal length | Zoom range | Notes |
|---|---|---:|---:|---:|---|
| 0 | Back | Level 3 | 6.3 mm | 0.6–10× | Logical camera; physical IDs 2, 5, 6, 7; selected for probe |
| 1 | Front | Full | 3.3 mm | 1–8× | 4K30 and 1080p60 surface outputs advertised |
| 2 | Back | Limited | 2.2 mm | 1–8× | Public ultra-wide camera |
| 3 | Front | Full | 3.3 mm | 1–8× | Alternate public front camera |

Camera 0 advertises 1920×1080 MediaCodec surface output at up to 60 fps and 3840×2160 at 30 fps. The Qualcomm hardware AVC encoder (`c2.qti.avc.encoder`) supports CBR/VBR, surface input, up to 8192×8192, and up to 16 instances according to MediaCodec.

## 1080p30 comparison

| Measurement | OpenLens spike | scrcpy 4.1 baseline |
|---|---:|---:|
| Requested mode | 1920×1080, 30 fps, 8 Mbps | 1920×1080, 30 fps, 8 Mbps |
| Encoded frames | 888 | 885 |
| Media duration | 29.674 s by frame timestamps; 29.707 s MP4 | 29.580 s MP4 |
| Average frame rate | 29.93 fps by probe | 29.92 fps by MP4 |
| Effective video bitrate | 6.89 Mbps | 7.93 Mbps |
| AVC profile | Main | High |
| Keyframes | 30 | Not yet recorded in summary |
| Clean encoder EOS | Yes | Process ended cleanly at time limit |
| Full FFmpeg decode | No errors/warnings | Decodes, with 15 repeated timestamp warnings at a 30 Hz output time base |
| Packet interval mean | 33.45 ms | 33.35 ms |
| Packet interval standard deviation | 1.12 ms | 2.20 ms |
| Intervals outside 25–45 ms | 1 of 889 | 15 of 884 |

The scrcpy file's encoded packet PTS/DTS values remain strictly increasing; its warnings are timing irregularities, not evidence of bitstream corruption.

## Phone pipeline latency

The instrumented custom probe compares the monotonic timestamp attached to each camera surface frame with the monotonic clock when its MediaCodec output is dequeued. This measures the phone's camera-surface-to-encoded-buffer path only; it is not camera-to-OBS latency.

| Samples | Minimum | p50 | p95 | p99 | Maximum |
|---:|---:|---:|---:|---:|---:|
| 888 | 64.23 ms | 89.64 ms | 98.17 ms | 101.62 ms | 122.74 ms |

## Thermal observations

No thermal throttling status was reported (`Thermal Status: 0`) after either 30-second run. After the scrcpy baseline the current readings included AP 39.0 °C, skin 34.1 °C, battery 31.2 °C, and USB 29.3 °C. Samsung's first skin throttling threshold is 36 °C. These short observations do not replace the planned 30-minute soak.

## Reproducibility and artifacts

Structured reports and checksums are in `artifacts/phase0/s24-ultra/`. The two MP4 files are intentionally ignored by Git because they are about 25–30 MB each, but remain in the local artifact directory. `SHA256SUMS` records the exact measured files.

## V4L2 and OBS sink proof

The host's signed `v4l2loopback` 0.15.4 module was loaded as `/dev/video42`, card label `OpenLens`, with `exclusive_caps=1`.

The automated format matrix wrote and read 90 frames for each OBS-relevant raw format at 1280×720/30:

| FFmpeg format | V4L2 FourCC | Result |
|---|---|---|
| yuv420p / I420 | YU12 | Pass |
| nv12 | NV12 | Pass |
| yuyv422 | YUYV | Pass |

scrcpy 4.1 then streamed camera ID 0 directly to `/dev/video42` as 1920×1080 YU12. A separate V4L2 reader consumed 120 frames at 30 fps. This path retained the previously observed irregular-timestamp warnings but completed successfully.

Finally, a small libobs harness loaded the exact bundled `linux-v4l2` plugin from OBS 32.2.1 and created a `v4l2_input` source for the active OpenLens feed. The OBS plugin dequeued 243 unique 1920×1080 frames in 8.073 seconds, measuring 29.976 fps. This verifies installed-OBS source compatibility without modifying the user's OBS profiles.

FFmpeg's generic V4L2 output path reports the loopback color metadata as sRGB/BT.601 even when the frames are numerically converted to limited-range BT.709. The production OpenLens V4L2 writer must issue its own `VIDIOC_S_FMT` with Rec.709 transfer/encoding metadata and test the result inside OBS; do not inherit FFmpeg's metadata behavior.

## Open validation work

A physical high-speed clock/LED test is still required for true glass-to-OBS latency. A 30-minute thermal/reconnect soak is also retained as the first Phase 1 regression gate. Neither result should be inferred from the phone-only MediaCodec latency metric.
