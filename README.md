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

**Desktop (Windows)** — see [docs/HOW_TO_USE.md](docs/HOW_TO_USE.md). No
programming needed: double-click `START_HERE.bat`.

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
| Windows | MSYS2 (ucrt64) with `g++`, or cross-compile with `zig c++` |

## License

Apache License 2.0 — see [LICENSE](LICENSE).
