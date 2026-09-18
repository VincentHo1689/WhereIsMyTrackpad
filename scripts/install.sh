#!/bin/bash
# install.sh - install WhereIsMyTrackpad into the user's home directory.
#
# Default (safe): per-app injection. Nothing global is changed; you launch apps
# through the provided wrapper.
#
# --global: additionally sets the DYLD_INSERT_LIBRARIES environment globally via
# launchctl, so every newly launched non-SIP app is injected. This is riskier and
# is opt-in only.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HOME/Library/Application Support/WhereIsMyTrackpad"
DYLIB_NAME="libWhereIsMyTrackpad.dylib"

GLOBAL=0
for a in "$@"; do
  case "$a" in
    --global) GLOBAL=1 ;;
    -h|--help) echo "usage: $0 [--global]"; exit 0 ;;
  esac
done

if [ ! -f "$HERE/../build/$DYLIB_NAME" ]; then
  echo "Building dylib..."
  "$HERE/../build.sh"
fi

mkdir -p "$DEST"
cp "$HERE/../build/$DYLIB_NAME" "$DEST/$DYLIB_NAME"

# Apple Silicon requires at least an ad-hoc signature for injection.
codesign -f -s - "$DEST/$DYLIB_NAME"

# Copy the wrapper + CLI alongside the dylib (wrapper expects the dylib next to it).
cp "$HERE/run-with-patch.sh" "$DEST/run-with-patch.sh"
cp "$HERE/wimt" "$DEST/wimt"
cp "$HERE/prepare-app.sh" "$DEST/prepare-app.sh"
cp "$HERE/fix-launchnext-gestures.sh" "$DEST/fix-launchnext-gestures.sh"
cp "$HERE/inject-app.sh" "$DEST/inject-app.sh"
cp "$HERE/inject-dylib.py" "$DEST/inject-dylib.py"
cp "$HERE/uninstall.sh" "$DEST/uninstall.sh"
chmod +x "$DEST/run-with-patch.sh" "$DEST/wimt" "$DEST/prepare-app.sh" \
         "$DEST/fix-launchnext-gestures.sh" "$DEST/inject-app.sh" "$DEST/inject-dylib.py" \
         "$DEST/uninstall.sh"

# Expose the CLI on PATH without sudo where possible.
BINDIR="$HOME/.local/bin"
mkdir -p "$BINDIR"
ln -sf "$DEST/wimt" "$BINDIR/wimt"

echo
echo "Installed to: $DEST"
echo "CLI:          $BINDIR/wimt  (add $BINDIR to PATH if not already)"

if [ "$GLOBAL" -eq 1 ]; then
  echo
  echo "WARNING: enabling global injection via launchctl. Apps with Hardened Runtime"
  echo "or SIP protection will ignore it, and incompatible apps may crash. Prefer the"
  echo "per-app wrapper unless you specifically want global injection."
  launchctl setenv DYLD_INSERT_LIBRARIES "$DEST/$DYLIB_NAME"
  echo "Global injection enabled. Restart target apps to take effect."
  echo "Disable with: launchctl unsetenv DYLD_INSERT_LIBRARIES  (or run uninstall.sh)"
else
  echo
  echo "Per-app usage:"
  echo "  \"$DEST/run-with-patch.sh\" /Applications/LaunchNext.app"
  echo "  \"$BINDIR/wimt\" run /Applications/LaunchNext.app"
  echo
  echo "Re-run with --global for system-wide injection (not recommended)."
fi
