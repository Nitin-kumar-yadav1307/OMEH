# Architecture

## The problem

Play the same audio on two devices so that they stay sample-accurate over
time, without any infrastructure (no router, no internet, no accounts).

## The shape of the solution

```
┌────────────── master ──────────────┐         ┌──────────── client ────────────┐
│ MediaProjection                    │         │                                │
│   → AudioRecord (internal mix)     │  UDP    │ DatagramSocket                  │
│   → masterBuild() [JNI → sync_     │ ──────► │   → clientFeed()  (ring)        │
│     wifi.h] packs magic/seq/pts/PCM│  48200  │   → clientRender() (pull)       │
│ Wi-Fi Direct GROUP OWNER           │         │   → AudioTrack                  │
└────────────────────────────────────┘         └────────────────────────────────┘
```

The master is the **Wi-Fi Direct Group Owner**; the client joins it. Traffic
never touches a third-party network.

## `protocol/` — the single source of truth

The sync engine is one contract split into four focused headers, so each
concern stays readable and testable on its own. `sync_wifi.h` is an **umbrella**
that includes the rest — every target keeps compiling one include:

| Header | Owns | Why it's separate |
|--------|------|-------------------|
| `wire.h` | constants + `SyncWifiPacket` | the on-wire contract; a `static_assert` locks the packet size |
| `ring.h` | `SyncWifiRing` | sample ring with a float read position |
| `drift.h` | `SyncWifiDrift` | the PI controller — pure math, no I/O |
| `jitter.h` | `SyncWifiJitter` | the AIMD latency policy — pure policy, no I/O |
| `sync_wifi.h` | *(umbrella)* | one include for all of the above |

The last two are pure functions over plain structs — no JNI, no clock, no
allocation — which is what makes them testable without a phone.

Consumed by:

| Target | File | How |
|--------|------|-----|
| Android (JNI) | `android/app/src/main/cpp/native-lib.cpp` | `#include "sync_wifi.h"` via the CMake include dir |
| Desktop test master | `desktop/tools/sync_master.cpp` | same |
| Desktop test client | `desktop/tools/sync_client.cpp` | same |
| **Unit tests** | `desktop/tools/sync_selftest.cpp` | same — 6000+ checks, no phones |

The two **earbud-sync** tools (`desktop/pipewire/`, `desktop/windows/`) are
*self-contained* with their own wire format — they are the earlier prototype
this project grew from, not consumers of the shared headers.

Because the headers are shared, the Android app and the desktop test pair can
be pointed at each other: that is how the protocol is validated without phones.
For policy changes, run the self-test first — it is the fast loop:

```bash
cmake --build desktop/build -j && ./desktop/build/sync_selftest
```

One trap the self-test does not catch: JNI symbols encode the full Kotlin
package (`Java_com_syncwifi_stream_NativeBridge_*`). Moving a Kotlin file to a
new package renames what the linker expects, but the compiler stays silent —
it only fails at runtime with `UnsatisfiedLinkError`. Keep the two in step.

## The jitter buffer

UDP on Wi-Fi Direct arrives with jitter and occasional loss. The client keeps
a ring buffer and renders from a target fill level.

The target level adapts **AIMD** (additive increase, multiplicative decrease):

- On an underrun (buffer starved, audible glitch) → grow the target by a fixed
  step. Costs latency, buys robustness.
- On a clean window → shrink the target back down. Buys latency, risks glitching.

Bounded to **40–250 ms**. The drift PI controller runs underneath, correcting
the slow clock mismatch between the two phones' audio oscillators.

## Android layering

```
com.syncwifi
├── SyncWifiApp.kt    Application entry. Decides whether to install the
│                     CrashHandler (never in the ":crash" process).
├── CrashHandler.kt   Uncaught exceptions → filesDir + CrashActivity.
│
├── ui/               MainActivity (roles, QR, permissions, status panel)
│                     CrashActivity (runs in its own ":crash" process so it
│                     survives the crash it is displaying)
├── p2p/              WifiP2pManagerWrapper — lifecycle-safe async wrapper
│                     over WifiP2pManager (broadcast receivers, channel)
├── stream/           MasterService  — capture + send (foreground service)
│                     ClientService  — receive + play (foreground service)
│                     NativeBridge   — JNI surface, port constants
└── util/             AppLog — on-device ring-buffered log, mirrored to logcat
```

**Why the services are separate classes, not one.** They have genuinely
different lifecycles and permission sets: the master needs a
`MediaProjection` and `mediaProjection` FGS type; the client needs
`AudioTrack` and a `ConnectivityManager` network callback to hold the P2P
link. Folding them together produced the class that this split replaced.

**Why `WifiP2pManagerWrapper` exists.** `WifiP2pManager` is asynchronous and
callback-driven, and its channel is tied to a `Looper`. Raw use leaks
receivers and races the channel. The wrapper owns both.

## Known constraints

- **Band mismatch.** A Wi-Fi Direct group runs on exactly one channel. A
  2.4 GHz-only client cannot hear a 5 GHz group owner at all — the invitation
  never completes. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
- **DRM.** `MediaProjection` cannot capture DRM-protected audio (Netflix);
  it captures silence.
- **`minSdk 29`.** `AudioPlaybackCapture` requires Android 10.
- **No public API to force the group band.** `createGroup()` takes no
  configuration; the operating channel is chosen by the framework.