#!/bin/bash
# fix-launchnext-gestures.sh - point LaunchNext's gesture feature at the built-in
# trackpad and enable it.
#
# Why this is needed: LaunchNext stores a fixed multitouch device ID. On a
# Touch Bar MacBook that ID is commonly the Touch Bar (family 176), so gestures
# never fire even with the dylib patch. This also enables the feature, which is
# off by default.
#
# Quit LaunchNext first; UserDefaults written while it runs are overwritten on quit.
set -euo pipefail

DOMAIN="${1:-LaunchNext}"

if pgrep -f '/LaunchNext.app/Contents/MacOS/LaunchNext' >/dev/null 2>&1; then
  echo "warning: LaunchNext is running; quit it first or these settings may be overwritten." >&2
fi

# Built-in trackpad = the AppleMultitouchDevice whose Product mentions Trackpad.
ID="$(ioreg -c AppleMultitouchDevice -r -d 1 2>/dev/null | awk '
  /"Multitouch ID"/ { id=$NF }
  /"Product"/ && /Trackpad/ { print id; exit }')"

# Fallback: family 113 is the built-in trackpad on several Apple Silicon MacBooks.
if [ -z "${ID:-}" ]; then
  ID="$(ioreg -c AppleMultitouchDevice -r -d 1 2>/dev/null | awk '
    /"Multitouch ID"/ { id=$NF }
    /"Family ID" = 113/ { print id; exit }')"
fi

if [ -z "${ID:-}" ]; then
  echo "error: could not find the built-in trackpad Multitouch ID" >&2
  exit 1
fi

defaults write "$DOMAIN" gestureEnabled -bool true
defaults write "$DOMAIN" gestureDeviceSelectionMode -string automatic
defaults write "$DOMAIN" gestureSelectedDeviceIDs -array "$ID"

echo "LaunchNext gestures enabled, trackpad ID set to $ID"
echo "Restart LaunchNext with the patch injected (wimt run /Applications/LaunchNext.app)."
