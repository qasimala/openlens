# OBS setup

## V4L2 virtual camera

Start the paired phone in OpenLens Desktop, then add **Video Capture Device
(V4L2)** in OBS. Select OpenLens or `/dev/video42`, 1920×1080 or 1280×720,
30 fps, and I420/YU12. The desktop app owns the secure Wi-Fi session; stopping
or closing it releases the phone camera.

## Native OBS source

Run `scripts/install-obs-plugin.sh`, restart native OBS, and add **OpenLens
Phone Camera**. With one paired phone available the source discovers it
automatically. Its properties cover quality, bitrate, rear/front camera, zoom,
exposure, torch, mirror, rotation, latency buffer, and stopping while hidden.

Only one source may control a phone. The plugin publishes I420 frames as BT.709
limited-range asynchronous video with monotonic timestamps and shows a red
disconnect slate while it searches for the same pinned identity.

The developer installer writes only
`~/.config/obs-studio/plugins/openlens-obs/bin/64bit/openlens-obs.so`. Flatpak
OBS integration is not yet certified because sandbox access to Avahi, identity
files, and native plugins needs separate packaging.
