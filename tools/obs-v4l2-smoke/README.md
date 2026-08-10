# OBS V4L2 source smoke test

This Phase 0 harness loads the system OBS installation's bundled `linux-v4l2` plugin through libobs, creates a real `v4l2_input` source for `/dev/video42`, and counts frames dequeued by that OBS plugin. It succeeds only when it receives at least 120 frames at 1920×1080 and at least 25 fps.

It tests the same source implementation used by the OBS desktop application without changing the user's OBS profiles or scene collections.

Build with CMake and Ninja, then run while a producer is writing 1920×1080 YU12 frames to `/dev/video42`:

```bash
cmake -S tools/obs-v4l2-smoke -B tools/obs-v4l2-smoke/build -G Ninja
cmake --build tools/obs-v4l2-smoke/build
tools/obs-v4l2-smoke/build/openlens-obs-v4l2-smoke /dev/video42
```

Requirements: OBS/libobs development headers matching the installed OBS version and the bundled `linux-v4l2` plugin.
