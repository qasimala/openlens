# OpenLens protocol v2.0

Status: normative draft implemented by the Kotlin and C++ protocol libraries.

OpenLens multiplexes control, diagnostics, and H.264 video over one ordered byte stream carried inside an authenticated TLS 1.3 connection on the local network. DNS-SD discovery is not trusted for identity; both endpoints require the SPKI pin saved during one-time pairing before camera messages are accepted. All unsigned integers are big-endian.

Before framed media begins, a new TLS connection carries one bounded ASCII request line: `PAIR 2` for the isolated enrollment exchange or `OPENLENS 2` for a normal pinned session. Pairing uses the commit/reveal construction and dual confirmation specified in `plans/codex/wifi-migration-plan-v2.html`. Pairing sockets cannot start the camera.

## Base header

Every message starts with 36 base bytes. A future minor version may append header bytes by increasing `header_length`; v0.1 readers validate and skip that extension.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `PHCM` |
| 4 | 1 | Major version, currently 2 |
| 5 | 1 | Minor version, currently 0 |
| 6 | 2 | Message type |
| 8 | 2 | Flags |
| 10 | 2 | Header length, currently 36 |
| 12 | 4 | Stream ID |
| 16 | 8 | Connection sequence |
| 24 | 8 | Monotonic media PTS in microseconds, or zero |
| 32 | 4 | Payload length |

Limits are checked before allocation: 8 MiB globally, 256 KiB for non-video metadata, a 4096-byte extended header, and 4096×2160 decoded video.

## Message types

| ID | Name | Payload |
|---:|---|---|
| 1 | HELLO | JSON client/version/nonce |
| 2 | HELLO_ACK | JSON negotiated version/session |
| 3 | CAPABILITIES | Versioned JSON camera/encoder capabilities |
| 4 | CONFIGURE | JSON requested preset and controls |
| 5 | CONFIGURED | JSON requested/applied settings and stream ID |
| 6 | VIDEO_CONFIG | Exact MediaCodec AVC codec-specific bytes |
| 7 | VIDEO_FRAME | One encoded AVC access unit |
| 8 | CONTROL | JSON transaction and requested control |
| 9 | CONTROL_ACK | JSON requested/applied value or stable error |
| 10 | STATS | JSON bounded counters/thermal state |
| 11 | PING | Empty or JSON diagnostic payload |
| 12 | PONG | Mirrors PING transaction data |
| 13 | ERROR | JSON stable error code, message, recoverability |
| 14 | END_STREAM | Empty or JSON stop reason |

Flags: bit 0 required, bit 1 keyframe, bit 2 codec configuration, bit 3 acknowledgement, bit 4 end-of-stream. Unknown required message types terminate the connection. Unknown optional message types are safely skipped.

The desktop sends HELLO. The phone replies HELLO_ACK and CAPABILITIES. The desktop sends CONFIGURE; the phone replies CONFIGURED. Each new stream begins with VIDEO_CONFIG and a keyframe. Sequence numbers increase monotonically per connection. Reconfiguration creates a new non-zero stream ID. PING/PONG drives heartbeat detection. END_STREAM is graceful; EOF without END_STREAM is a discontinuity.

A different major version is incompatible. A higher minor version is accepted only through supported features and skippable optional messages. Fixture changes require a changelog entry and byte-for-byte Kotlin/C++ tests.
