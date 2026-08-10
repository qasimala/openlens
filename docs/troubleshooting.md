# Troubleshooting

- **Phone does not appear:** open OpenLens on the phone; confirm both devices
  are on the same LAN and client isolation is disabled. Check that Avahi is
  running and multicast DNS (`UDP 5353`) is allowed.
- **Discovery says Access denied/unavailable:** start or repair the system Avahi
  daemon and its D-Bus access. The app reports this without falling back to an
  insecure connection.
- **Codes differ:** cancel on both devices. Do not approve the pairing. Retry on
  a trusted network.
- **Identity changed:** forget the phone on the PC and the computer on the
  phone, then pair again. This is expected after reinstalling or clearing app
  storage.
- **Camera permission:** open the phone app and tap **Allow camera** once.
- **Camera does not start in the background:** bring the phone app to the
  foreground and retry. Android may restrict foreground-service starts after
  the app has been force-stopped.
- **No `/dev/video42`:** review the v4l2loopback setup. OpenLens does not load
  kernel modules automatically.
- **Black or stale OBS source:** run `openlens synthetic` to separate the
  V4L2/OBS path from phone/network decoding.
- **Hot phone or unstable Wi-Fi:** select 720p30 or lower the bitrate, keep the
  phone near the access point, and stop if the device becomes uncomfortable.

`openlens doctor --json` creates a small local diagnostic summary. It contains
discovery readiness and counts, not camera frames, certificate pins, hardware
serials, or account data.
