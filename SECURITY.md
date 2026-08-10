# Security policy

OpenLens is pre-release. Until a public private-reporting address is chosen,
do not publish an exploit, device identifier, captured frame, or private log.
Contact the repository owner privately and include the affected revision,
impact, reproduction steps, and whether the issue is already public.

Security boundaries include hostile local networks, DNS-SD spoofing, TLS peer
identity, one-time pairing commitments and confirmation, untrusted protocol
payloads, camera foreground-service lifecycle, support-bundle privacy, and
package/signing integrity.

The app requires camera permission. It does not request microphone, location,
storage, contacts, or account permission. Android's `INTERNET` permission is
used only for local discovery and encrypted local sockets. The app does not
start at boot. Camera use begins only after a paired PC request while the phone
app is available and remains represented by Android's foreground-service
indicator.
