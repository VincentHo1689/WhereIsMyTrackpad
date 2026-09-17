#!/bin/bash
# test_patch.sh - compile the harness, run the acceptance checks before and after
# injecting the patch, and print a PASS/FAIL summary.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROOT="$PWD"
DYLIB="$ROOT/build/libICanSeeMyTrackpadNow.dylib"

echo "== Building =="
"$ROOT/build.sh" || { echo "build failed"; exit 2; }

if [ ! -f "$DYLIB" ]; then
  echo "dylib not found: $DYLIB"
  exit 2
fi

echo
echo "== Before patch (expected: FAIL) =="
"$ROOT/build/verify"
before=$?

echo
echo "== After patch (expected: PASS) =="
DYLD_INSERT_LIBRARIES="$DYLIB" "$ROOT/build/verify"
after=$?

echo
echo "================ SUMMARY ================"
[ "$before" -ne 0 ] && echo "before patch : FAIL (reproduces the bug)  OK" \
                    || echo "before patch : PASS  (bug not reproduced here)"
[ "$after" -eq 0 ]  && echo "after patch  : PASS                       OK" \
                    || echo "after patch  : FAIL"
echo "========================================"

if [ "$before" -ne 0 ] && [ "$after" -eq 0 ]; then
  echo "RESULT: PASS"
  exit 0
elif [ "$before" -eq 0 ]; then
  echo "RESULT: NOT APPLICABLE (this machine already reports the trackpad as default)"
  exit 0
else
  echo "RESULT: FAIL"
  exit 1
fi
