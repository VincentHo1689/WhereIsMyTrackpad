# ICanSeeMyTrackpadNow — Technical Report

## 1. Environment

- Machine: MacBook Pro 13" (2022), Apple M2, **with Touch Bar**.
  (The brief stated M1; the hardware identification is M2. Same device class for
  this bug: Apple Silicon + Touch Bar.)
- OS: macOS 26.6.2 (build 25G83), arm64.
- SIP: enabled. Secure Boot: Full Security. No custom `boot-args`.
- Xcode license not accepted, so the system linker is unusable. All builds use
  `DEVELOPER_DIR=/Library/Developer/CommandLineTools xcrun --sdk macosx clang`.
  `/usr/bin/make` is also blocked by the same license gate, so the project uses a
  plain `build.sh`.

## 2. Recon: how many multitouch devices are there?

IOKit exposes two `AppleMultitouchDevice` services:

| Property           | Trackpad        | Touch Bar       |
|--------------------|-----------------|-----------------|
| Family ID          | 113             | 176             |
| Transport          | FIFO            | SPI             |
| Sensor Rows        | 18              | 2               |
| Product            | `Apple Internal Keyboard / Trackpad` | (none) |
| Built-in           | yes             | yes             |

The Touch Bar is identifiable from IOKit alone (Family ID 176 / SPI transport /
2 sensor rows), which is what the patch uses to tell them apart.

## 3. Root cause

`MTDeviceCreateList()` already returns **both** devices, but the Touch Bar comes
first. `MTDeviceCreateDefault()` returns the **Touch Bar consistently**. Both
LaunchNext and StrokeMouse obtain their device from `MTDeviceCreateDefault`
(StrokeMouse even has the string `"Default multitouch device unavailable"`), so
neither ever receives trackpad contact frames. This was reproduced on the machine:

```
before patch : MTDeviceCreateDefault family=176   list=[176, 113]
```

## 4. macOS 26 ABI notes

Determined from `dyld_info -exports` on the shared-cache-only framework binary
(the `.framework` directory has no on-disk binary, so `nm`/`otool` do not work):

- `MTDeviceGetFamilyID(MTDeviceRef, int *out) -> int` — **out-parameter** style.
  The return value is a status (e.g. `-536870206`); the family id is written to the
  out pointer. Code that treats the return value as the family id is wrong on this
  OS version.
- `MTDeviceGetService(MTDeviceRef) -> io_service_t` — one-arg, returns the service.
- `MTDeviceCreateFromService(io_service_t) -> MTDeviceRef` — exists and is the key
  to obtaining a usable reference (see §6).
- `MTDeviceGetSensorRows` / `MTDeviceGetSensorColumns` **do not exist**; replaced
  by `MTDeviceGetSensorDimensions` / `MTDeviceGetOpenRows`. The brief's API
  assumptions are from an older surface.
- `MTRegisterContactFrameCallback` / `MTUnregisterContactFrameCallback` exist.
- `MTDeviceCreateList` and `MTDeviceCreateDefault` each return a fresh `+1` object
  per call; the same physical device yields distinct objects across call sites.

## 5. Interposition gotchas (both cost real debugging time)

1. **`dlsym` self-recursion.** During interposition,
   `dlsym(frameworkHandle, "MTDeviceCreateDefault")` resolves to *our own*
   interposer, so the wrapper calls itself and hangs. The correct pattern is a
   direct self-reference in the interposing image —
   `static MTDeviceDefault_t orig = MTDeviceCreateDefault;` — which dyld binds to
   the original. `dlsym(RTLD_NEXT, …)` also fails because the target app `dlopen`s
   the framework *after* our constructor runs.

2. **Re-entrancy produces a dead reference.** Calling the original
   `MTDeviceCreateList()` from inside the interposed `MTDeviceCreateDefault` yields
   a trackpad `MTDeviceRef` that is **not startable**: `MTDeviceIsRunning()` stays
   `0` and the framework never logs device recognition. The fix is to build the
   reference with `MTDeviceCreateFromService()` from an IOKit-enumerated
   `AppleMultitouchDevice` service. That reference starts correctly and the
   framework logs:

   ```
   *** Recognized (0x71) family*** (26 cols X 18 rows)
   ```

## 6. Implementation

`src/patch.c`:

- Places `__DATA,__interpose` entries for `MTDeviceCreateDefault`,
  `MTDeviceCreateList` and `MTDeviceGetFamilyID`.
- Resolves helpers lazily against the framework handle (`MTDeviceGetFamilyID`,
  `MTDeviceCreateFromService`, `MTDeviceStart`, `MTDeviceIsRunning`).
- `family_of()` handles the out-parameter ABI, with a fallback probe for other
  layouts.
- Enumerates IOKit `AppleMultitouchDevice` services, scores them (prefers built-in,
  FIFO transport, many sensor rows), and picks the trackpad.
- `resolve_trackpad()` calls `MTDeviceCreateFromService()` on that service and
  caches the result for the process lifetime.
- `my_createDefault()` substitutes the trackpad when the framework default is the
  Touch Bar (family 176).
- `my_createList()` returns a reordered list with non-176 devices first.
- `my_getFamily()` remaps the built-in trackpad's raw family 113 to 108 (the
  "MacBook trackpad" family) so apps that only classify known trackpad families
  accept it. External devices (when `MTDeviceIsBuiltIn()` is false) and family 176
  are left untouched; `ICSMT_KEEP_FAMILY=1` disables the remap.
- Pass-through interposers for `MTRegisterContactFrameCallback` and `MTDeviceStart`
  log which device an app actually starts (debug only; behaviour unchanged).
- Debug logging is gated behind `ICSMT_DEBUG=1`.

## 7. Verification

`test/test_patch.sh` builds the tools and runs `src/verify.c` before and after
injection. Actual result on the target machine:

```
before patch : FAIL  (default family=176, list=[176,113])
after  patch : PASS
  [PASS] default device is the built-in trackpad - MTDeviceCreateDefault family=108
  [PASS] trackpad family is app-recognizable (108, not 113) - family=108
  [PASS] default device starts - running=1
  [PASS] device list is non-empty - count=2
  [PASS] trackpad present in list
  [PASS] touch bar present in list
  [PASS] trackpad is ordered first - first=108
  [PASS] touch bar is ordered last - last=176
  [PASS] touch bar still starts - running=1
  [PASS] first listed device (trackpad) starts - running=1
```

Contact-frame delivery (`src/callback_test.c`) requires a physical touch. Actual
run on the target machine:

```
default family=113 (Trackpad)
*** Recognized (0x71) family*** (26 cols X 18 rows)
running=1
Touch the TRACKPAD for 15 seconds...
RESULT frames=1725 maxFingers=1
```

The substituted device is recognized, starts, and delivers live contact frames
(1725 frames in 15 s), confirming the fix end to end.

## 8. Real-app injection: hardened runtime

Both target apps are **hardened-runtime** signed and ad-hoc (no Team ID):

```
LaunchNext : flags=0x10002(adhoc,runtime)  entitlements: get-task-allow
StrokeMouse: flags=0x10000(runtime)        entitlements: app-sandbox=false,
                                                         automation.apple-events,
                                                         cs.disable-library-validation
```

A hardened-runtime process **silently ignores `DYLD_INSERT_LIBRARIES`** unless
signed with `com.apple.security.cs.allow-dyld-environment-variables`, and it refuses
a third-party dylib unless `com.apple.security.cs.disable-library-validation` is
also present. Proof:

```
# as-is: no output from the injected dylib
$ DYLD_INSERT_LIBRARIES=...libICanSeeMyTrackpadNow.dylib .../LaunchNext
(nothing)

# after re-signing ad-hoc with the two entitlements added:
$ DYLD_INSERT_LIBRARIES=... .../LaunchNext
[ICanSeeMyTrackpadNow] loaded (createList=... createDefault=...)
[ICanSeeMyTrackpadNow] reordered device list (2 entries)
```

Injecting into the real LaunchNext confirms the app-side selection: with gestures
enabled the hook logs

```
[ICanSeeMyTrackpadNow] app registered a contact callback on family=113
[ICanSeeMyTrackpadNow] app started device family=113
```

i.e. LaunchNext now binds its contact callback to the **trackpad** rather than the
Touch Bar.

### 8.1 LaunchNext's own device database

LaunchNext vendors OpenMultitouchSupport. Its
`OpenMTManager.m` maps family 113 to **"Magic Mouse"** with `isTrackpad = NO`, while
family 108 maps to "MacBook Trackpad". `GestureInputDevice.isRecommendedGestureDevice`
requires a trackpad, so before the remap the automatic gesture selection selected
nothing and the trackpad was unusable. The `MTDeviceGetFamilyID` interposer (family
113 -> 108) is what makes LaunchNext treat it as a trackpad.

Separately, LaunchNext's gesture feature is **disabled by default** and its
`gestureSelectedDeviceIDs` pointed at the Touch Bar's multitouch ID
(the Touch Bar's multitouch ID, family 176). `scripts/fix-launchnext-gestures.sh` enables the
feature, switches to automatic selection, and points the stored ID at the built-in
trackpad.

`scripts/inject-app.sh` goes further and removes the need for a wrapper entirely: it
copies the dylib into the app's `Contents/Frameworks/`, writes an `LC_LOAD_DYLIB`
load command into the executable (into existing load-command padding, so no offsets
move), and re-signs ad-hoc with the same two entitlements. The app then loads the
patch on every launch, including launches from Finder/Dock/Spotlight. The original
executable is backed up and `--restore` reverts it.

`scripts/prepare-app.sh` performs the entitlements-only re-sign, merging the two entitlements into
the app's existing ones and preserving the hardened-runtime flag. Because both apps
are already ad-hoc, no Developer ID signature is lost. `--copy` creates a patched
copy so the original stays pristine; `--restore` reverts in-place changes.

## 9. Limitations

- SIP-protected system processes cannot be injected regardless of signing.
- The app's signature is replaced; self-updating apps (StrokeMouse uses Sparkle)
  revert on update and must be re-prepared.
- Global `launchctl setenv DYLD_INSERT_LIBRARIES` injects into all newly launched
  non-SIP apps and may destabilise incompatible ones; opt-in only.
- Only affects processes that load `MultitouchSupport.framework` after injection.

## 10. Conclusion

The bug is not a missing device — `MultitouchSupport` already exposes the trackpad —
but a default-selection bug: `MTDeviceCreateDefault()` prefers the Touch Bar. The
patch corrects device selection and classification at the framework boundary, keeps
the Touch Bar functional, and is verified end to end on the target hardware: the
substituted trackpad starts and delivers live contact frames, and LaunchNext binds
its gesture callback to it. Two real-world obstacles remain, both addressed: the
hardened runtime (re-sign the target app with the documented entitlements) and apps
that store their own device choice (fix the app-side selection, e.g.
`scripts/fix-launchnext-gestures.sh`).
