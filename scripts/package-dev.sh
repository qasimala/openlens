#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="$repo_root/artifacts/dev/openlens-0.1.0-dev"
apk="$repo_root/android/app/build/outputs/apk/debug/app-debug.apk"
cli="$repo_root/build/desktop-debug/desktop/cli/openlens"
desktop_app="$repo_root/build/desktop-debug/desktop/gui/openlens-desktop"
plugin="$repo_root/build/desktop-debug/desktop/obs/openlens-obs.so"

for required in "$apk" "$cli" "$desktop_app" "$plugin"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing build output: $required" >&2
    exit 2
  fi
done

mkdir -p "$destination"
install -m 0644 "$apk" "$destination/openlens-debug.apk"
install -m 0755 "$cli" "$destination/openlens"
install -m 0755 "$desktop_app" "$destination/openlens-desktop"
install -m 0755 "$plugin" "$destination/openlens-obs.so"
install -m 0644 "$repo_root/packaging/linux/dev.openlens.OpenLens.desktop" "$destination/dev.openlens.OpenLens.desktop"
install -m 0644 "$repo_root/packaging/linux/openlens.svg" "$destination/openlens.svg"
install -m 0644 "$repo_root/LICENSE" "$destination/LICENSE"
install -m 0644 "$repo_root/LICENSES/GPL-2.0-or-later.txt" "$destination/GPL-2.0-or-later.txt"
install -m 0644 "$repo_root/NOTICE" "$destination/NOTICE"

(
  cd "$destination"
  sha256sum GPL-2.0-or-later.txt LICENSE NOTICE dev.openlens.OpenLens.desktop openlens openlens-debug.apk openlens-desktop openlens-obs.so openlens.svg > SHA256SUMS
)

apk_sha="$(sha256sum "$destination/openlens-debug.apk" | cut -d' ' -f1)"
cli_sha="$(sha256sum "$destination/openlens" | cut -d' ' -f1)"
desktop_sha="$(sha256sum "$destination/openlens-desktop" | cut -d' ' -f1)"
plugin_sha="$(sha256sum "$destination/openlens-obs.so" | cut -d' ' -f1)"
generated="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
sed \
  -e "s/@GENERATED@/$generated/g" \
  -e "s/@APK_SHA@/$apk_sha/g" \
  -e "s/@CLI_SHA@/$cli_sha/g" \
  -e "s/@DESKTOP_SHA@/$desktop_sha/g" \
  -e "s/@PLUGIN_SHA@/$plugin_sha/g" \
  "$repo_root/packaging/linux/spdx-template.json" > "$destination/openlens.spdx.json"

echo "Developer bundle created at $destination"
echo "This debug bundle is not a signed or publishable release."
