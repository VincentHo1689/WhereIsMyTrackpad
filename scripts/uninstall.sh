#!/bin/bash
# uninstall.sh - remove the patch and restore original behavior.
set -euo pipefail

DEST="$HOME/Library/Application Support/WhereIsMyTrackpad"
DYLIB="$DEST/libWhereIsMyTrackpad.dylib"
BINDIR="$HOME/.local/bin"

# Only clear the global variable if it points at our dylib.
if [ "$(launchctl getenv DYLD_INSERT_LIBRARIES 2>/dev/null || true)" = "$DYLIB" ]; then
  launchctl unsetenv DYLD_INSERT_LIBRARIES
  echo "Cleared global DYLD_INSERT_LIBRARIES."
fi

# Revert any apps that were permanently patched.
if [ -f "$DEST/injected-apps.txt" ]; then
  while IFS= read -r app; do
    [ -d "$app" ] || continue
    "$DEST/inject-app.sh" --restore "$app" || true
  done < "$DEST/injected-apps.txt"
fi

if [ -L "$BINDIR/wimt" ]; then rm -f "$BINDIR/wimt"; fi

if [ -d "$DEST" ]; then
  rm -rf "$DEST"
  echo "Removed $DEST"
else
  echo "Nothing to remove at $DEST"
fi

echo "Uninstalled. Restart any apps that were launched with the patch."
