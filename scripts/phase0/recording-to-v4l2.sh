#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
input="${1:-$repo_root/artifacts/phase0/s24-ultra/phase0-1080p30.mp4}"
device="${2:-/dev/video42}"

if [[ ! -f "$input" ]]; then
  echo "Missing input recording: $input" >&2
  exit 2
fi
if [[ ! -c "$device" ]]; then
  echo "Missing V4L2 loopback device: $device" >&2
  exit 2
fi

echo "Streaming $input to $device. Press Ctrl+C to stop."
exec ffmpeg -hide_banner -loglevel warning \
  -re -stream_loop -1 -i "$input" -an \
  -vf "scale=1920:1080:flags=lanczos:in_range=pc:out_range=tv,format=yuv420p,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709" \
  -colorspace bt709 -color_range tv \
  -f v4l2 "$device"
