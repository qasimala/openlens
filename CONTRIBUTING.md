# Contributing

OpenLens welcomes focused bug reports, hardware compatibility evidence, tests,
documentation, and code. Keep changes clean-room: do not copy scrcpy or other
third-party source into this project.

Before opening a change:

1. Build and test both Android and desktop code.
2. Run Android lint and the sanitizer preset for protocol/native changes.
3. Update protocol fixtures, specification, and changelog together.
4. Never include full device serials, camera frames, private logs, or signing keys.
5. Add `Signed-off-by: Name <email>` to commits to certify the Developer
   Certificate of Origin 1.1.

Compatibility reports should include phone model, Android/One UI version,
host distribution/kernel, OBS package/version, requested preset, result, and
the redacted output of `openlens probe --json`.

Do not report a green automated build as proof of latency, thermal behavior,
reconnect reliability, or a physical release gate. Attach measured evidence.
