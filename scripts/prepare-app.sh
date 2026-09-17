#!/bin/bash
# prepare-app.sh - make a hardened-runtime app injectable.
#
# Apps built with the hardened runtime silently ignore DYLD_INSERT_LIBRARIES
# unless they are signed with com.apple.security.cs.allow-dyld-environment-variables
# (and disable-library-validation to load a third-party dylib). This script
# re-signs the app ad-hoc with those two entitlements added, preserving whatever
# entitlements it already had.
#
# Both LaunchNext and StrokeMouse ship ad-hoc signed, so this does not replace a
# Developer ID signature. The original entitlements are backed up and can be
# restored with --restore.
#
# Usage:
#   prepare-app.sh /Applications/LaunchNext.app            # in place
#   prepare-app.sh --copy /Applications/StrokeMouse.app    # patched copy, original untouched
#   prepare-app.sh --restore /Applications/LaunchNext.app  # undo
set -euo pipefail

DEST="$HOME/Library/Application Support/ICanSeeMyTrackpadNow"
BACKUP_DIR="$DEST/backups"
APPS_DIR="$DEST/Apps"

COPY=0; RESTORE=0; APP=""
for a in "$@"; do
  case "$a" in
    --copy) COPY=1 ;;
    --restore) RESTORE=1 ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) APP="$a" ;;
  esac
done
[ -n "$APP" ] || { echo "usage: $0 [--copy|--restore] /path/to/App.app" >&2; exit 2; }
[ -d "$APP" ] || { echo "error: not an app bundle: $APP" >&2; exit 2; }

BUNDLE_ID="$(defaults read "$APP/Contents/Info.plist" CFBundleIdentifier 2>/dev/null || echo unknown)"
NAME="$(basename "$APP" .app)"
mkdir -p "$BACKUP_DIR"

add_key() { # plist key value
  /usr/libexec/PlistBuddy -c "Add :$1 bool $2" "$3" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Set :$1 $2" "$3"
}

if [ "$RESTORE" -eq 1 ]; then
  SAVED="$BACKUP_DIR/$BUNDLE_ID.entitlements.plist"
  [ -f "$SAVED" ] || { echo "error: no backup for $BUNDLE_ID" >&2; exit 2; }
  codesign --force --deep --sign - --options runtime --entitlements "$SAVED" "$APP"
  echo "Restored original entitlements for $NAME"
  exit 0
fi

if [ "$COPY" -eq 1 ]; then
  mkdir -p "$APPS_DIR"
  TARGET="$APPS_DIR/$NAME.app"
  rm -rf "$TARGET"
  echo "Copying $APP -> $TARGET"
  cp -R "$APP" "$TARGET"
else
  TARGET="$APP"
fi

WORK="$(mktemp -t icsmt_ent).plist"
codesign -d --entitlements :- "$TARGET" > "$WORK" 2>/dev/null || true
if ! /usr/libexec/PlistBuddy -c "Print" "$WORK" >/dev/null 2>&1; then
  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict/></plist>' > "$WORK"
fi

# Back up the originals once (only for in-place prep; a copy is its own backup).
SAVED="$BACKUP_DIR/$BUNDLE_ID.entitlements.plist"
[ -f "$SAVED" ] || cp "$WORK" "$SAVED"

add_key com.apple.security.cs.allow-dyld-environment-variables true "$WORK"
add_key com.apple.security.cs.disable-library-validation true "$WORK"

echo "Re-signing $TARGET (ad-hoc, hardened runtime preserved)"
codesign --force --deep --sign - --options runtime --entitlements "$WORK" "$TARGET"
rm -f "$WORK"

echo
echo "Verifying:"
codesign -dv --verbose=2 "$TARGET" 2>&1 | grep -E 'flags=|Identifier='
echo
if [ "$COPY" -eq 1 ]; then
  echo "Patched copy ready: $TARGET"
  echo "Launch with: \"$DEST/run-with-patch.sh\" \"$TARGET\""
else
  echo "$NAME is now injectable. Launch with the wrapper (or reinstall the app later to revert)."
fi
