# ADR-003: C++20 desktop implementation

- Status: Accepted
- Date: 2026-08-08

The desktop core, CLI, V4L2 sink, and OBS plugin use C++20. Hostile input is handled with bounded spans, checked arithmetic, RAII, warnings-as-errors, fuzz targets, and ASan/UBSan builds. This preserves one object model across FFmpeg, Linux device APIs, and libobs.
