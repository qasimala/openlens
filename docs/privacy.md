# Privacy

OpenLens has no account, cloud service, advertisements, analytics, telemetry,
or required internet. The phone advertises a generic OpenLens service only
while its app is open. This reveals to the local network that an OpenLens
device is available and includes a random app-installation ID; it does not
publish the phone model, hardware serial, camera capabilities, pairing code, or
certificate pin.

Camera control and H.264 video travel only through TLS 1.3. First use requires a
matching six-digit code confirmed on both devices. Later connections require
both sides to prove possession of the previously pinned identity key. Android's
camera privacy indicator, persistent notification, and Stop control remain
active while capture runs.

The app requests camera and internet permissions; `INTERNET` is Android's name
for local socket access too. It does not request microphone, location, storage,
contacts, accounts, or background-start permission. Raw H.264 is written only
when the user supplies an explicit output path. Diagnostics never contain
frames or cryptographic pins.
