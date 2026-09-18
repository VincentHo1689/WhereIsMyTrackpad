#!/bin/bash
# inject-app.sh - permanently load the dylib into an app bundle.
#
# Adds an LC_LOAD_DYLIB command to the app's executable, copies the dylib into the
# bundle's Frameworks directory, and re-signs the app ad-hoc with the entitlements
# needed to load it under the hardened runtime. After this the app is patched on
# every launch (Finder, Dock, Spotlight) with no wrapper and no environment
# variable.
#
# The original executable and entitlements are backed up first.
#
# Usage:
#   inject-app.sh /Applications/LaunchNext.app
#   inject-app.sh --restore /Applications/LaunchNext.app
set -euo pipefail

DEST="$HOME/Library/Application Support/WhereIsMyTrackpad"
BACKUP_DIR="$DEST/backups"
DYLIB_NAME="libWhereIsMyTrackpad.dylib"
LOAD_PATH="@executable_path/../Frameworks/$DYLIB_NAME"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INJECTOR="$HERE/inject-dylib.py"

RESTORE=0; APP=""
for a in "$@"; do
  case "$a" in
    --restore) RESTORE=1 ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) APP="$a" ;;
  esac
done
[ -n "$APP" ] || { echo "usage: $0 [--restore] /path/to/App.app" >&2; exit 2; }
[ -d "$APP" ] || { echo "error: not an app bundle: $APP" >&2; exit 2; }

# Canonicalise to an absolute path so it works with a relative argument and matches
# the path recorded in injected-apps.txt.
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

[ -f "$DEST/$DYLIB_NAME" ] || { echo "error: $DEST/$DYLIB_NAME not found; run scripts/install.sh first" >&2; exit 2; }

BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" 2>/dev/null || echo unknown)"
NAME="$(basename "$APP" .app)"
EXE_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist" 2>/dev/null || true)"
[ -n "$EXE_NAME" ] || { echo "error: no CFBundleExecutable in $APP" >&2; exit 2; }
BIN="$APP/Contents/MacOS/$EXE_NAME"
[ -f "$BIN" ] || { echo "error: executable not found: $BIN" >&2; exit 2; }
mkdir -p "$BACKUP_DIR"
EXE_BAK="$BACKUP_DIR/$BUNDLE_ID.executable"
ENT_BAK="$BACKUP_DIR/$BUNDLE_ID.entitlements.plist"

# Save the original entitlements before touching the binary (after injection the
# broken signature makes them unreadable).
if [ ! -f "$ENT_BAK" ]; then
  codesign -d --entitlements :- "$APP" > "$ENT_BAK" 2>/dev/null || true
  if ! /usr/libexec/PlistBuddy -c "Print" "$ENT_BAK" >/dev/null 2>&1; then
    printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
      '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
      '<plist version="1.0"><dict/></plist>' > "$ENT_BAK"
  fi
fi

add_key() { /usr/libexec/PlistBuddy -c "Add :$1 bool $2" "$3" 2>/dev/null \
              || /usr/libexec/PlistBuddy -c "Set :$1 $2" "$3"; }

if [ "$RESTORE" -eq 1 ]; then
  # Prefer the pristine backup; also strip the load command so restore is clean even
  # if the backup was made from an already-injected binary.
  if [ -f "$EXE_BAK" ]; then cp "$EXE_BAK" "$BIN"; fi
  python3 "$INJECTOR" remove "$BIN" "$LOAD_PATH" >/dev/null 2>&1 || true
  rm -f "$APP/Contents/Frameworks/$DYLIB_NAME"
  codesign --force --deep --sign - --options runtime --entitlements "$ENT_BAK" "$APP" >/dev/null 2>&1 || true
  if [ -f "$DEST/injected-apps.txt" ]; then
    grep -vxF "$APP" "$DEST/injected-apps.txt" > "$DEST/injected-apps.txt.tmp" 2>/dev/null || true
    mv "$DEST/injected-apps.txt.tmp" "$DEST/injected-apps.txt" 2>/dev/null || true
  fi
  echo "Restored $NAME to its original executable and entitlements."
  exit 0
fi

[ -f "$EXE_BAK" ] || cp "$BIN" "$EXE_BAK"

mkdir -p "$APP/Contents/Frameworks"
cp "$DEST/$DYLIB_NAME" "$APP/Contents/Frameworks/$DYLIB_NAME"
codesign -f -s - "$APP/Contents/Frameworks/$DYLIB_NAME"

echo "Adding load command -> $LOAD_PATH"
codesign --remove-signature "$BIN" 2>/dev/null || true
python3 "$INJECTOR" add "$BIN" "$LOAD_PATH"

WORK="$(mktemp -t wimt_ent).plist"
cp "$ENT_BAK" "$WORK"
add_key com.apple.security.cs.disable-library-validation true "$WORK"
add_key com.apple.security.cs.allow-dyld-environment-variables true "$WORK"

echo "Re-signing $NAME (ad-hoc, hardened runtime preserved)"
codesign --force --deep --sign - --options runtime --entitlements "$WORK" "$APP"
rm -f "$WORK"

echo
echo "Verifying:"
python3 "$INJECTOR" check "$BIN" "$LOAD_PATH" || true
codesign --verify --deep --strict "$APP" 2>&1 | sed 's/^/  /' || true
mkdir -p "$DEST"
grep -qxF "$APP" "$DEST/injected-apps.txt" 2>/dev/null || echo "$APP" >> "$DEST/injected-apps.txt"
echo "$NAME now loads the patch on every launch (no wrapper needed)."
echo "Undo with: $0 --restore \"$APP\""
