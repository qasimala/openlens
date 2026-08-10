#!/usr/bin/env bash
set -euo pipefail

output="${1:-artifacts/generated/test-pattern-1080p30.h264}"
mkdir -p "$(dirname "$output")"
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1920x1080:rate=30 \
  -frames:v 60 -c:v libx264 -preset ultrafast -tune zerolatency -profile:v main \
  -x264-params keyint=30:min-keyint=30:scenecut=0:bframes=0:threads=1 \
  -pix_fmt yuv420p -f h264 -y "$output"
sha256sum "$output"
