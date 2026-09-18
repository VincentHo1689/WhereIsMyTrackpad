#!/bin/bash
# run-with-patch.sh - launch an app bundle with the trackpad patch injected.
# Usage: run-with-patch.sh /Applications/LaunchNext.app [args...]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DYLIB="$HERE/libWhereIsMyTrackpad.dylib"
[ -f "$DYLIB" ] || DYLIB="$HERE/../build/libWhereIsMyTrackpad.dylib"
if [ ! -f "$DYLIB" ]; then
  echo "error: libWhereIsMyTrackpad.dylib not found next to this script" >&2
  exit 2
fi

APP="${1:-}"
[ -n "$APP" ] || { echo "usage: $0 /path/to/App.app [args...]" >&2; exit 2; }
shift || true

# Accept either a bundle path or a bare app name found in /Applications.
if [ ! -d "$APP" ]; then
  if [ -d "/Applications/$APP.app" ]; then APP="/Applications/$APP.app"; fi
fi
[ -d "$APP" ] || { echo "error: not an app bundle: $APP" >&2; exit 2; }

EXEC="$(defaults read "$APP/Contents/Info.plist" CFBundleExecutable 2>/dev/null || true)"
[ -n "$EXEC" ] || { echo "error: cannot read CFBundleExecutable for $APP" >&2; exit 2; }
BIN="$APP/Contents/MacOS/$EXEC"
[ -x "$BIN" ] || { echo "error: executable not found: $BIN" >&2; exit 2; }

if ! codesign -d --entitlements :- "$APP" 2>/dev/null | grep -q 'allow-dyld-environment-variables'; then
  if codesign -dv "$APP" 2>&1 | grep -q 'flags=.*runtime'; then
    echo "warning: $APP uses the hardened runtime and is not signed to allow" >&2
    echo "         DYLD_INSERT_LIBRARIES; the patch will be silently ignored." >&2
    echo "         Make it injectable first: $HERE/prepare-app.sh \"$APP\"" >&2
  fi
fi

echo "Launching $APP with patch injected"
echo "  dylib: $DYLIB"
exec env DYLD_INSERT_LIBRARIES="$DYLIB" "$BIN" "$@"
