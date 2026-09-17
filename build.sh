#!/bin/bash
# build.sh - build the dylib and the test tools.
#
# Uses the CommandLineTools toolchain explicitly, because /usr/bin/make and the
# Xcode linker refuse to run until the Xcode license is accepted, while the CLT
# toolchain works without that step.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
CC=(xcrun --sdk macosx clang)
FRAMEWORKS=(-framework CoreFoundation -framework IOKit)

mkdir -p build

echo "Building dylib..."
# Universal so the same dylib loads into both Apple Silicon and Intel app slices.
"${CC[@]}" -dynamiclib -arch arm64 -arch x86_64 \
  -o build/libICanSeeMyTrackpadNow.dylib src/patch.c \
  "${FRAMEWORKS[@]}" -undefined dynamic_lookup
# Apple Silicon requires at least an ad-hoc signature for injection.
codesign -f -s - build/libICanSeeMyTrackpadNow.dylib

echo "Building tools..."
for t in probe appsim verify callback_test; do
  "${CC[@]}" -o "build/$t" "src/$t.c" "${FRAMEWORKS[@]}"
done

echo "Done:"
ls -1 build
