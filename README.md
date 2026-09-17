# ICanSeeMyTrackpadNow

A macOS dylib that fixes apps which see the **Touch Bar** instead of the **built-in
trackpad** when they use Apple's private `MultitouchSupport.framework`.

## The bug

On MacBooks with a Touch Bar, `MultitouchSupport` exposes two multitouch devices:

| Device   | Family ID | Transport | Rows x Cols |
|----------|-----------|-----------|-------------|
| Trackpad | 113       | FIFO      | 18 x 26     |
| Touch Bar| 176       | SPI       | 2 x 60      |

`MTDeviceCreateDefault()` returns the **Touch Bar** (family 176), and
`MTDeviceCreateList()` returns the Touch Bar **first**. Apps such as LaunchNext and
StrokeMouse call `MTDeviceCreateDefault()` (or take the first entry of the list), so
they end up listening to the Touch Bar and never see trackpad gestures.

Verified on a 2022 MacBook Pro 13" (M2, Touch Bar), macOS 26:

```
before patch : MTDeviceCreateDefault family=176   list=[176, 113]
after  patch : MTDeviceCreateDefault family=113   list=[113, 176]
```

## The fix

`src/patch.c` interposes three symbols in `MultitouchSupport`:

- `MTDeviceCreateDefault` — if the framework returns the Touch Bar, substitute the
  built-in trackpad.
- `MTDeviceCreateList` — reorder so non-Touch-Bar devices come first.
- `MTDeviceGetFamilyID` — report the built-in trackpad (raw family 113) as family
  **108** ("MacBook trackpad"). Several apps, including LaunchNext, classify 113 as
  a Magic Mouse and therefore refuse to treat it as a trackpad. Set
  `ICSMT_KEEP_FAMILY=1` to disable just this remap.

The trackpad reference is rebuilt with `MTDeviceCreateFromService()` from an
IOKit-enumerated `AppleMultitouchDevice` service, because a reference obtained by
re-entering `MTDeviceCreateList()` during interposition is **non-startable** (the
framework never brings it up). The Touch Bar is left completely intact.

### LaunchNext: a second, app-side cause

Even with the above, LaunchNext's gesture feature needs two settings fixed: it is
disabled by default (`gestureEnabled = 0`), and it stores a fixed multitouch device
ID which on a Touch Bar MacBook is typically the **Touch Bar** (family 176), not the
trackpad. `scripts/fix-launchnext-gestures.sh` enables the feature, switches to
automatic device selection, and points the stored selection at the built-in
trackpad. Quit LaunchNext before running it.

## Requirements

- Apple Silicon or Intel Mac with a Touch Bar (on machines without one the patch is
  a no-op and everything keeps working).
- macOS 13+ (verified on macOS 26).
- The CommandLineTools toolchain (the Xcode linker refuses to build until its
  license is accepted; this project uses CLT to avoid that).

## Build

```sh
./build.sh          # produces build/libICanSeeMyTrackpadNow.dylib and test tools
```

## Install

```sh
./scripts/install.sh            # installs the dylib + helper scripts for the current user
./scripts/install.sh --global   # also set DYLD_INSERT_LIBRARIES system-wide (see Limitations)
```

This installs into `~/Library/Application Support/ICanSeeMyTrackpadNow/` and puts an
`icsmt` CLI on `~/.local/bin`.

## Usage

### Recommended: patch an app permanently (no wrapper)

`inject-app.sh` copies the dylib into the app bundle, adds an `LC_LOAD_DYLIB` load
command so the app loads it on **every** launch (Finder, Dock, Spotlight), and
re-signs it ad-hoc with the entitlements required to load a third-party library
under the hardened runtime. After this, the app is patched with no wrapper and no
environment variable.

```sh
~/Library/Application\ Support/ICanSeeMyTrackpadNow/inject-app.sh /Applications/LaunchNext.app
```

The original executable and entitlements are backed up; revert any time with:

```sh
~/Library/Application\ Support/ICanSeeMyTrackpadNow/inject-app.sh --restore /Applications/LaunchNext.app
```

Quit the app before patching, then reopen it normally.

### Alternative: launch through the wrapper (does not modify the app)

Apps signed with the hardened runtime ignore `DYLD_INSERT_LIBRARIES`, so they must
first be made injectable, then launched through the wrapper:

```sh
D="~/Library/Application Support/ICanSeeMyTrackpadNow"
"$D/prepare-app.sh" --copy /Applications/StrokeMouse.app     # patched copy, original untouched
"$D/run-with-patch.sh" "$D/Apps/StrokeMouse.app"             # or: icsmt run ...
```

Use `prepare-app.sh --restore` to revert in-place changes.

## Verify

The bundled test harness runs the acceptance checks before and after the patch:

```sh
./test/test_patch.sh
```

Expected: `RESULT: PASS` — before the patch the default device is family 176 and the
checks fail; after injection the default is family 108 (the trackpad, remapped from
113 for app compatibility), the list is reordered, and the Touch Bar still starts.

To prove contact frames really arrive, run the interactive test and touch the
trackpad when prompted:

```sh
DYLD_INSERT_LIBRARIES="$PWD/build/libICanSeeMyTrackpadNow.dylib" ./build/callback_test 15
```

Expected final line: `RESULT frames=<n> maxFingers=<m>` with `n > 0`.

## Troubleshooting

The dylib appends a few lines per launch to `~/Library/Logs/ICanSeeMyTrackpadNow.log`,
including when the app was opened from Finder/Dock. A healthy LaunchNext session ends
with:

```
app registered a contact callback on family=113
app started device family=113
```

`family=113` is the trackpad; `176` is the Touch Bar. If the log is empty, the dylib
is not loading (re-run `inject-app.sh`, or check the app is not SIP-protected). Set
`ICSMT_DEBUG=1` to also print these lines to stderr. Set `ICSMT_KEEP_FAMILY=1` to
disable the family remap.

### LaunchNext still does nothing with gestures

LaunchNext has its own gesture settings that are off by default and, on a Touch Bar
MacBook, point at the Touch Bar instead of the trackpad:

```sh
~/Library/Application\ Support/ICanSeeMyTrackpadNow/fix-launchnext-gestures.sh
```

Quit LaunchNext first, run it, then reopen LaunchNext.

## Uninstall

```sh
./scripts/uninstall.sh                                   # from the clone
"$HOME/Library/Application Support/ICanSeeMyTrackpadNow/uninstall.sh"   # installed copy
```

This reverts every permanently patched app (`inject-app.sh --restore`) and removes the
installed files.

## Limitations

- **Hardened runtime.** Injection requires the target to be signed with
  `com.apple.security.cs.allow-dyld-environment-variables` and
  `com.apple.security.cs.disable-library-validation`. SIP-protected system
  processes cannot be injected at all.
- **Signature changes.** Both scripts replace the app's ad-hoc signature. If the app
  self-updates (e.g. StrokeMouse's Sparkle), the update restores the original and you
  must patch it again. Apps signed with a Developer ID are re-signed ad-hoc, which
  breaks Gatekeeper for them, so patching is intended for ad-hoc-signed apps.
- **`--global` is coarse.** It injects into every newly launched non-SIP app;
  incompatible apps may misbehave. Prefer per-app patching.
- Only affects processes that load `MultitouchSupport.framework` after injection.

## Layout

```
build.sh                     build dylib + test tools
src/patch.c                  the interposing dylib
src/verify.c                 acceptance checks (PASS/FAIL)
src/callback_test.c          interactive contact-frame test
src/probe.c, appsim.c        helpers used during development
test/test_patch.sh           before/after acceptance run
scripts/install.sh           install
scripts/uninstall.sh         remove (and revert patched apps)
scripts/inject-app.sh        permanently load the dylib into an app (recommended)
scripts/inject-dylib.py      Mach-O LC_LOAD_DYLIB helper used by inject-app.sh
scripts/prepare-app.sh       add injection entitlements to an app
scripts/run-with-patch.sh    launch an app with the patch (wrapper alternative)
scripts/fix-launchnext-gestures.sh  enable LaunchNext gestures on the trackpad
scripts/icsmt                management CLI
reports/TECHNICAL_REPORT.md  detailed findings
```
