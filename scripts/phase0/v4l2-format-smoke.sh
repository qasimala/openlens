#!/usr/bin/env bash
set -euo pipefail

device="${1:-/dev/video42}"

if [[ ! -c "$device" ]]; then
  echo "Missing $device. Load v4l2loopback first:" >&2
  echo "  sudo modprobe v4l2loopback video_nr=42 card_label=OpenLens exclusive_caps=1" >&2
  exit 2
fi

producer_pid=""
cleanup() {
  if [[ -n "$producer_pid" ]]; then
    kill "$producer_pid" 2>/dev/null || true
    wait "$producer_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

for pixel_format in yuv420p nv12 yuyv422; do
  echo "Testing $pixel_format on $device"
  ffmpeg -hide_banner -loglevel error \
    -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
    -vf "format=$pixel_format" -f v4l2 "$device" &
  producer_pid=$!

  for _ in {1..20}; do
    if v4l2-ctl --device "$device" --get-fmt-video >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done

  v4l2-ctl --device "$device" --get-fmt-video
  ffmpeg -hide_banner -loglevel error \
    -f v4l2 -framerate 30 -video_size 1280x720 -i "$device" \
    -frames:v 90 -f null -

  kill "$producer_pid" 2>/dev/null || true
  wait "$producer_pid" 2>/dev/null || true
  producer_pid=""
done

echo "V4L2 format smoke test passed for I420/yuv420p, NV12, and YUYV/yuyv422."
