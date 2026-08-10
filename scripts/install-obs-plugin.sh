#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
plugin="$repo_root/build/desktop-debug/desktop/obs/openlens-obs.so"
destination="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/openlens-obs/bin/64bit"

if [[ ! -f "$plugin" ]]; then
  echo "Build OpenLens first with ./scripts/build-desktop.sh" >&2
  exit 1
fi

install -d "$destination"
install -m 0755 "$plugin" "$destination/openlens-obs.so"
echo "Installed OpenLens for the native OBS package at $destination/openlens-obs.so"
