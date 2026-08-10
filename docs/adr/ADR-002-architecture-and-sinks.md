# ADR-002: Shared desktop core and independent sinks

- Status: Accepted
- Date: 2026-08-08

Camera2 and MediaCodec are isolated behind Android interfaces. The phone session owns framing and its per-session abstract socket. One C++ desktop core owns ADB, parsing, decode, timing, reconnection, and normalized frames. Null, file, V4L2, and OBS sinks depend on that core contract and never implement transport.

V4L2 ships first for broad Linux compatibility. The native OBS asynchronous source follows without replacing the core or protocol.
