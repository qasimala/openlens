# ADR-005: Versioned 36-byte multiplexed protocol

- Status: Accepted
- Date: 2026-08-08

OpenLens uses one ordered stream with a 36-byte big-endian `PHCM` header. It multiplexes JSON metadata/control and binary H.264 access units. Hard limits precede allocation, unknown required types fail, unknown optional types skip, and reconfiguration creates a new stream ID beginning with codec configuration and a keyframe.

The normative contract lives in `protocol/PROTOCOL.md`.
