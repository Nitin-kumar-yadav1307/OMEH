# Agent notes

## What this repo is

`OMEH` — Open Multi Earbuds Hub: low-latency audio sync. The Android app (`android/`) streams a
phone's internal audio to a second phone over Wi-Fi Direct. The `desktop/`
folder holds the earlier Bluetooth-earbud prototypes and a protocol test pair.

Everything shares one wire format and one sync algorithm, defined under
`protocol/`: `wire.h` (packet), `ring.h`, `drift.h` (PI), `jitter.h` (AIMD),
with `sync_wifi.h` as the umbrella include. **That is the contract — change it
once, there, and every target follows.** After touching it, run the self-test:

```bash
cmake --build desktop/build -j && ./desktop/build/sync_selftest
```

## Build commands

```bash
# Android (writes android/app/build/outputs/apk/debug/app-debug.apk)
cd android && ./gradlew assembleDebug

# Desktop (Linux) — configure + build out-of-source, never in-tree
cmake -S desktop -B desktop/build -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/build -j
```

Environment for the Android build: `JAVA_HOME` → JDK 17, `ANDROID_HOME` →
SDK, and `android/local.properties` containing `sdk.dir=...`.

## Layout and where things belong

```
protocol/                    shared header + PROTOCOL.md — the wire contract
android/                     standalone Gradle project (open THIS folder)
  app/src/main/cpp/          JNI bridge; include path → ../../../../../protocol
  app/src/main/java/com/syncwifi/
    SyncWifiApp.kt           Application; installs CrashHandler
    CrashHandler.kt          uncaught exceptions → file + CrashActivity
    ui/                      MainActivity, CrashActivity
    p2p/                     WifiP2pManagerWrapper
    stream/                  MasterService, ClientService, NativeBridge
    util/                    AppLog
desktop/
  pipewire/                  Linux PipeWire earbud-sync (self-contained)
    pipewire_sync.cpp          driver: doc comment + ordered #includes + main
    sync_{config,types,playback,streams,pulse,registry,health}.h
  windows/                   Windows WASAPI earbud-sync (self-contained)
    pipewire_sync_win.cpp      driver: doc comment + ordered #includes + main
    win_{config,types,device,capture,source,proctap,pipeline,align,render,poll,health,main}.h
  tools/                     sync_master / sync_client (use protocol/)
docs/                        ARCHITECTURE, ANDROID, HOW_TO_USE, TROUBLESHOOTING
```

## Conventions

- **Do not commit or push without being asked.** Leave changes in the working
  tree and report what changed.
- **Do not commit binaries.** `.gitignore` excludes `*.exe`, `*.pdb`, `*.o`,
  `*.so`, `*.wav`, `*.log` and the extensionless Linux executables. Build
  out-of-source into `desktop/build/`.
- Kotlin lives in `com.syncwifi.{ui,p2p,stream,util}`. If you add a subpackage,
  update the component names in `AndroidManifest.xml` — they are relative
  (`.stream.MasterService`) and a wrong name crashes at launch.
- The self-contained earbud prototypes (`desktop/pipewire/`,
  `desktop/windows/`) are split into **single-TU module headers** — each
  `sync_*.h`/`win_*.h` is included in order into one `.cpp`, so the compiled
  program is token-for-token identical to the old monolith. Keep it that way:
  a real multi-`.cpp` split would require cross-TU linkage and would break the
  Windows `run_win.bat`/MSYS2 single-file build (`g++ pipewire_sync_win.cpp`),
  which cannot be compile-tested on Linux. Verify the Linux side with
  `cmake --build desktop/build -j` after touching them.
- `minSdk 29` (Android 10) — required for `AudioPlaybackCapture`.
- On-device logging goes through `AppLog`, never raw `Log`, so messages show up
  in the app's log panel as well as logcat (tag `SyncWifi`).
- Comments explain *why*, not *what*. Hard-won platform findings (hidden-API
  behaviour, OEM quirks, band mismatch) are worth a comment at the site.

## Verify after changing Kotlin

The build is the test — there is no unit-test suite. Always run:

```bash
cd android && ./gradlew assembleDebug
```

A wrong manifest component name or a missing import fails here, not on device.

## Verify after changing the protocol or native code

```bash
cmake --build desktop/build -j        # tools compile against the headers
./desktop/build/sync_selftest         # policy/controller regressions
cd android && ./gradlew assembleDebug # JNI bridge compiles against them too
```

All three must pass: the headers are consumed by two independent toolchains,
and the self-test is what catches a controller regression without a phone.

Keep the JNI symbols in step with the Kotlin package. `NativeBridge.kt` is in
`com.syncwifi.stream`, so the exports in `native-lib.cpp` must be
`Java_com_syncwifi_stream_NativeBridge_*`. Moving the Kotlin file without
renaming the C++ side compiles and installs fine, then throws
`UnsatisfiedLinkError` on the first native call.

## Platform pitfalls already hit (do not re-learn these)

- `Activity.RESULT_OK == -1`, so a `-1` sentinel for "extra missing" collides
  with a successful result.
- `WifiP2pGroup` has no `ipAddress` property.
- `WifiP2pConfig.Builder().build()` throws on Android 15+ unless a peer
  address or name+passphrase is set.
- SSIDs containing spaces break `WifiNetworkSpecifier` joins — silently.
- The P2P group SSID is not the peer's `deviceName`; the QR carries the device
  name for that reason.
- A device already inside a group cannot invite new peers.
- `MediaProjection` cannot capture DRM audio (Netflix → silence).