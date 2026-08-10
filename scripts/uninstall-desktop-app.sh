#!/usr/bin/env bash
set -euo pipefail

local_root="${XDG_DATA_HOME:-$HOME/.local/share}"
rm -f "$HOME/.local/bin/openlens-desktop"
rm -f "$local_root/applications/dev.openlens.OpenLens.desktop"
rm -f "$local_root/icons/hicolor/scalable/apps/openlens.svg"
rm -f "$local_root/openlens/openlens-obs.so"
rmdir "$local_root/openlens" 2>/dev/null || true
echo "Removed only the OpenLens Desktop files installed for this user."
