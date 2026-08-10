#!/usr/bin/env bash
set -euo pipefail

plugin_root="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/openlens-obs"
plugin="$plugin_root/bin/64bit/openlens-obs.so"

if [[ -f "$plugin" ]]; then
  rm "$plugin"
  rmdir "$plugin_root/bin/64bit" "$plugin_root/bin" "$plugin_root" 2>/dev/null || true
  echo "Removed only the OpenLens OBS plugin."
else
  echo "OpenLens OBS plugin is not installed."
fi
