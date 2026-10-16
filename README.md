# SyncWifi

Low-latency, peer-to-peer audio sync between Android phones over **Wi-Fi Direct**.

One phone (the **master**) captures whatever audio the device is playing via
`MediaProjection` and streams it as UDP PCM to a second phone (the **client**),
which plays it back through a drift-corrected jitter buffer. Pairing is done by
scanning a QR code. No router, no internet, no accounts.

The repo also contains the **desktop prototypes** this work grew out of: a
PipeWire/Linux tool and a Windows WASAPI tool that keep a pair of Bluetooth
earbuds in sync.

## Layout

```
protocol/     Wire format + sync math, shared verbatim by every target.
              split by concern: wire.h (packet), ring.h, drift.h (PI),
              jitter.h (AIMD). sync_wifi.h is the umbrella + single
              source of truth — change it here once.

android/      The Android product. Standalone Gradle project (open the
              android/ folder in Android Studio or build with ./gradlew).
              Kotlin lives in com.syncwifi.{ui,p2p,stream,util}.

desktop/      Reference implementations and tools:
  pipewire/   Linux PipeWire earbud-sync (pipewire_sync).
  windows/    Windows WASAPI earbud-sync (pipewire_sync_win) + diagnostics.
  tools/      sync_master / sync_client — a protocol test pair with no UI.

docs/         Architecture, build setup, user guide, troubleshooting.
```

## Quick start

**Android** — see [docs/ANDROID.md](docs/ANDROID.md):

```bash
cd android
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk
```

Install the same APK on both phones. On the master tap **START MASTER**, on the
client tap **SCAN TO JOIN** and scan the master's QR. Then start playback on the
master; the client follows.

**Desktop (Linux)** — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md):

```bash
cmake -S desktop -B desktop/build -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/build -j
./desktop/build/sync_master   # in one terminal
./desktop/build/sync_client   # in another
```

**Desktop (Windows)** — see [docs/HOW_TO_USE.md](docs/HOW_TO_USE.md).
Download **`SyncWifi-Setup-1.0.0.exe`** from a GitHub Release, run it
(per-user install, no admin), and launch **SyncWifi** from the Start Menu or
Desktop shortcut. One self-contained `sync_gui.exe` — static SDL2 + static
CRT, no DLLs, no console window anywhere. The build is unsigned: SmartScreen
shows "Windows protected your PC" → click **More info → Run anyway**.
Developers build from source with `desktop/windows/run_win.bat` (MSVC +
vcpkg manifest; the GUI links `x64-windows-static` so it matches the shipped
binary exactly).

## Design notes

* **Wi-Fi Direct, not hotspot.** The master becomes the Group Owner; the client
  joins it. No third-party router involved.
* **QR carries identity, not the SSID.** A group's SSID is
  `DIRECT-xx-<model>`, which some OEMs fill with *spaces*
  (`DIRECT-7w-Galaxy F17 5G`) — joining by SSID is unreliable, so the client
  matches on the master's P2P **device name** and joins by MAC address.
* **One shared contract.** `protocol/` defines the packet layout and the
  drift-correcting ring buffer, split by concern (`wire.h`, `ring.h`,
  `drift.h`, `jitter.h`) under one umbrella include. The Android JNI bridge
  and the desktop test tools all compile the same files, so the wire format
  cannot silently drift between targets. A `static_assert` locks the packet
  size at compile time.
* **Testable without a phone.** `desktop/tools/sync_selftest.cpp` drives the
  ring, drift and jitter logic with synthetic data and asserts the controller
  converges — 6000+ checks, runs in milliseconds
  (`cmake --build desktop/build -j && ./desktop/build/sync_selftest`).
* **Adaptive jitter buffer.** AIMD between 40–250 ms: grows on underrun,
  shrinks on a clean window.

## Requirements

| Target  | Needs |
|---------|-------|
| Android | API 29+ (Android 10) — `AudioPlaybackCapture` |
| Linux   | `libpipewire-0.3-dev`, `libspa-0.2-dev`, CMake ≥ 3.16, C++17 |
| Windows | **End users:** Windows 10/11 64-bit — just run `SyncWifi-Setup-1.0.0.exe` from a Release. **Building:** Visual Studio 2022 "Build Tools" / "Desktop development with C++" (MSVC). One-click build: `desktop/windows/run_win.bat` |

## One-shot CI build (all three targets)

GitHub Actions builds the Linux, Windows, and Android targets from this repository
on every push to `main`, on every pull request, and on every version tag (`v*`).
The single workflow file is:

`.github/workflows/build.yml`

- **Linux** (`ubuntu-latest`): `cmake` → `sync_gui`, `sync_master`, `sync_client`,
  `sync_selftest` (self-test always runs), `pipewire_sync`.
- **Windows** (`windows-latest`): MSVC (`cl.exe`) → `pipewire_sync_win.exe` +
  `sync_gui.exe` (ImGui+SDL2 panel via vcpkg, drives the engine over `--ui`;
  **static** SDL2 + `/MT`, verified by a `dumpbin /dependents` guard) and the
  Inno Setup installer **`SyncWifi-Setup-1.0.0.exe`**.
- **Android** (`ubuntu-latest`): Gradle → `app-debug.apk`.

```bash
# From the repo root. Each target runs independently on CI.

# Linux:
cmake -S desktop -B desktop/build -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/build -j
./desktop/build/sync_selftest

# Windows (one-click batch; needs Build Tools with MSVC):
desktop/windows/run_win.bat

# Android:
cd android && ./gradlew assembleDebug
```

## "Download and run" on release

When a version tag like `v1.2.3` is pushed, GitHub Actions turns that tag into a
**draft** GitHub Release and attaches the binaries built for it:

| Asset | Built by |
|-------|----------|
| `SyncWifi-Setup-1.0.0.exe` (Windows installer, **start here**) | CI `windows` job (Inno Setup) |
| `pipewire_sync_win.exe` + `sync_gui.exe` (Windows portable — self-contained, no DLLs) | CI `windows` job |
| `sync_gui`, `sync_master`, `sync_client`, `sync_selftest`, `pipewire_sync` (Linux) | CI `linux` job |
| `app-debug.apk` (Android) | CI `android` job |

Put the prebuilt binaries in front of the people who need them:
- **Windows**: download `SyncWifi-Setup-1.0.0.exe` and run it — it installs
  `sync_gui.exe` + `pipewire_sync_win.exe` per-user and adds Start Menu /
  Desktop shortcuts; no console window ever appears. Unsigned build: on the
  SmartScreen screen click **More info → Run anyway**. Prefer no install?
  Drop the two `.exe`s into any folder and run `sync_gui.exe` directly.
- **Linux**: run the binaries from the Release page (or your distro packages `pipewire_sync`/`sync_gui`).
- **Android**: install `app-debug.apk` on both phones.

Draft releases are private until you publish them, so you can review the binaries
before anyone downloads them.


## License

Apache License 2.0 — see [LICENSE](LICENSE).
