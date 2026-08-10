# ADR-001: Phase 0 stop/go decision

- Status: Accepted — GO
- Date: 2026-08-08
- Decision owners: project maintainer

## Context

OpenLens could either build a purpose-made Android camera and desktop receiver, or wrap scrcpy's mature camera transport. Phase 0 must prevent a costly rewrite of solved infrastructure while establishing whether a custom stack has a defensible product and technical advantage.

Measured evidence is recorded in `docs/phase0/benchmark.md` and the structured files under `artifacts/phase0/s24-ultra/`.

## Decision

Proceed with the custom OpenLens architecture from plan v3. Keep scrcpy as the mandatory baseline and a documented fallback, but do not import or fork its source in the first implementation.

The reason to proceed is not basic camera transport—scrcpy already does that well. The reason is the combined product surface:

1. Camera2 controls and discoverability that scrcpy does not currently provide: manual exposure/focus/white balance, stabilization policy, lens-aware presets, and safe capability-driven UI.
2. A stable OpenLens protocol with telemetry, negotiation, explicit errors, recovery, and future audio synchronization.
3. First-class OBS onboarding and diagnostics rather than a collection of shell flags and kernel-module knowledge.
4. A measured custom phone path that holds 1080p30, ends cleanly, exposes low-level capabilities, and shows lower packet-timing variance in the initial comparison.

## Guardrails

- Phase 1 must not expand into image-processing features before the minimal USB transport and OBS path are proven.
- Reuse concepts and public platform APIs, but keep the first implementation clean-room and do not copy scrcpy code.
- Continue measuring against the same scrcpy version and settings at every performance gate.
- If the custom transport cannot match scrcpy reliability within the Phase 1 budget, retain the OpenLens UX/control plane and reframe transport around an external scrcpy adapter.
- Do not claim glass-to-OBS latency from the phone-only MediaCodec metric.

## Acceptance evidence

- `/dev/video42` accepted and returned I420, NV12, and YUYV test frames through the installed v4l2loopback module.
- OBS 32.2.1's bundled `v4l2_input` source dequeued 243 1920×1080 frames at 29.976 fps from the OpenLens device.
- scrcpy camera-to-V4L2 was exercised as the directly comparable baseline, including a 120-frame readback.
- The custom phone path and scrcpy baseline both held approximately 30 fps in their 30-second runs without a thermal status event.

Physical glass-to-OBS latency and the 30-minute thermal/reconnect soak remain mandatory regression gates before the Phase 1 implementation can be called stable. They do not block the architecture decision because Phase 0 has already established camera/encoder feasibility, kernel/OBS compatibility, a baseline fallback, and material product differentiation.

## Consequences

The next implementation work remains the narrow v3 path: protocol framing, Android Camera2/MediaCodec service, USB bulk transport, host receiver, and V4L2 sink. The project will carry explicit stop/reframe checkpoints rather than treating the custom transport as irreversible.
