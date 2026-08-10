#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$repo_root/build/desktop-debug/desktop/gui/openlens-desktop"
desktop_file="$repo_root/packaging/linux/dev.openlens.OpenLens.desktop"
icon="$repo_root/packaging/linux/openlens.svg"
plugin="$repo_root/build/desktop-debug/desktop/obs/openlens-obs.so"
local_root="${XDG_DATA_HOME:-$HOME/.local/share}"

if [[ ! -x "$binary" ]]; then
  echo "Build OpenLens first with ./scripts/build-desktop.sh" >&2
  exit 2
fi

install -d "$HOME/.local/bin" "$local_root/applications" "$local_root/icons/hicolor/scalable/apps" "$local_root/openlens"
install -m 0755 "$binary" "$HOME/.local/bin/openlens-desktop"
install -m 0755 "$plugin" "$local_root/openlens/openlens-obs.so"
install -m 0644 "$desktop_file" "$local_root/applications/dev.openlens.OpenLens.desktop"
install -m 0644 "$icon" "$local_root/icons/hicolor/scalable/apps/openlens.svg"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$local_root/applications" >/dev/null 2>&1 || true
fi

echo "OpenLens Desktop is installed for this user. Open it from the application menu."
