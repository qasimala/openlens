#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apk="$repo_root/android/app/build/outputs/apk/release/app-release-unsigned.apk"
desktop="$repo_root/build/desktop-release/desktop/gui/openlens-desktop"
cli="$repo_root/build/desktop-release/desktop/cli/openlens"
plugin="$repo_root/build/desktop-release/desktop/obs/openlens-obs.so"
pattern='(^|[^[:alpha:]])adb([^[:alpha:]]|$)|usb debugging|abstract socket'

for required in "$apk" "$desktop" "$cli" "$plugin"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing release artifact: $required" >&2
    exit 2
  fi
done

if unzip -p "$apk" classes.dex | strings | grep -Eiq "$pattern"; then
  echo "The Android release still contains a retired USB/ADB transport marker." >&2
  exit 1
fi

for binary in "$desktop" "$cli" "$plugin"; do
  if strings "$binary" | grep -Eiq "$pattern"; then
    echo "Retired USB/ADB transport marker found in $binary" >&2
    exit 1
  fi
done

echo "Wi-Fi-only release check passed."
